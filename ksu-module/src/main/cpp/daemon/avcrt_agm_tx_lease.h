#pragma once

#include "avcrt_cellular_backend.h"
#include "avcrt_lpass_probe.h"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sound/asound.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace avcrt::agm {

// One-shot LPASS lease for a codec TX endpoint chosen by the caller. The
// channel map and mixer establish the local DEC route, not DSP graph ownership.
class TxLease {
    using Id = snd_ctl_elem_id;
    struct Edge { unsigned aif, dec; Id id; };
    struct Mux { Id id{}; unsigned initial = 0; };
    static constexpr unsigned kDecs = 32;

public:
    TxLease() = default;
    TxLease(const TxLease&) = delete;
    TxLease& operator=(const TxLease&) = delete;
    ~TxLease() { if (fd_ >= 0) { release(); close(fd_); } }

    // Caller discovers the ALSA card by PCM name; no fixed card/DEC/PCM ID.
    bool open_card(unsigned card, unsigned device, unsigned subdevices,
                   std::string_view backend, unsigned aif, bool write) {
        if (fd_ >= 0) return false;
        constexpr std::string_view prefix = "CODEC_DMA-LPAIF_RXTX-TX-";
        if (backend.substr(0, prefix.size()) != prefix) return false;
        auto suffix = backend.substr(prefix.size());
        unsigned instance;
        if (!number(suffix, instance) || !suffix.empty()) return false;
        const auto path = "/dev/snd/controlC" + std::to_string(card);
        fd_ = open(path.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
        if (fd_ < 0) return false;
        aif_ = aif;
        device_ = device;
        subdevices_ = subdevices;
        writable_ = write;
        const auto map_name = std::string(backend) + " Channel Map";
        Id ids[32];
        snd_ctl_elem_list list{};
        list.space = 32;
        list.pids = ids;
        do {
            if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.used > list.space ||
                (!list.used && list.offset < list.count) || list.count > 8192) return false;
            for (unsigned i = 0; i < list.used; ++i) {
                const auto& id = ids[i];
                if (id.iface != SNDRV_CTL_ELEM_IFACE_MIXER) continue;
                const auto* raw = reinterpret_cast<const char*>(id.name);
                const std::string_view name(raw, strnlen(raw, sizeof(id.name)));
                if (!observe(name, id, map_name)) return false;
            }
            list.offset += list.used;
        } while (list.offset < list.count);

        const auto read = [&](const Id& id, lpass::Control& ctl) { return control(id, ctl); };
        if (!map_.numid || !mute_.numid || !test_.numid || !power_.numid) return false;
        if (write && (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LOCK, &mute_) < 0 ||
                      ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LOCK, &test_) < 0)) return false;
        const auto s = probe_.snapshot(read);
        const auto lease = probe_.lease(s, read);
        if (s.status != lpass::Status::Present || lease.status != lpass::Status::Present ||
            lease.ms < 50 || lease.ms > 250 || s.mute || s.test_mute || !s.power ||
            static_cast<unsigned long>(s.supported) > UINT32_MAX || !read_map(mask_) ||
            !mask_ || (mask_ & ~static_cast<unsigned long>(s.power))) return false;
        if (!aif_) {
            // Find exactly one AIF from the kernel's channel map and live
            // switches, not from a correspondence between PCM and AIF numbers.
            for (const auto& edge : edges_) {
                if (!(mask_ & (1u << edge.dec))) continue;
                long on;
                if (!scalar(edge.id, on) || (on != 0 && on != 1)) return false;
                if (!on) continue;
                if (!edge.aif || (aif_ && aif_ != edge.aif)) return false;
                aif_ = edge.aif;
            }
            if (!aif_) return false;
        }
        for (unsigned dec = 0; dec < kDecs; ++dec) {
            if (!(mask_ & (1u << dec))) continue;
            for (auto* mux : {&dec_[dec], &dmic_[dec], &smic_[dec]})
                if (!read_enum(mux->id, mux->initial)) return false;
        }
        ready_ = stable();
        return ready_;
    }

    bool stable(const char** reason = nullptr, bool* lease_lost = nullptr) const {
        if (lease_lost) *lease_lost = false;
        const auto fail = [&](const char* why) {
            if (reason) *reason = why;
            return false;
        };
        long power, mute, test;
        uint32_t mask;
        snd_pcm_info pcm{};
        pcm.stream = SNDRV_PCM_STREAM_CAPTURE;
        pcm.device = device_;
        if (ioctl(fd_, SNDRV_CTL_IOCTL_PCM_INFO, &pcm) < 0 ||
            pcm.subdevices_count != subdevices_ || pcm.subdevices_avail != 0) return fail("stock TX PCM changed");
        if (!scalar(power_, power) || !scalar(mute_, mute) || !scalar(test_, test)) return fail("stock TX controls unreadable");
        const auto readback = cellular::mute_readback(active(), mask_, mute, test);
        if (readback == cellular::MuteReadback::Conflict) return fail("unexpected stock TX mute or test mute");
        if (mask_ & ~static_cast<unsigned long>(power)) return fail("stock TX DEC power changed");
        if (!read_map(mask) || mask != mask_) return fail("stock TX channel map changed");
        uint32_t selected = 0, present = 0;
        for (const auto& e : edges_) {
            const uint32_t bit = 1u << e.dec;
            long on;
            if (!scalar(e.id, on) || (on != 0 && on != 1)) return fail("stock TX AIF switch unreadable");
            if (e.aif == aif_) {
                present |= bit;
                if (on) selected |= bit;
            } else if (on && (mask_ & bit)) return fail("stock TX DEC also routed to another AIF");
        }
        if (selected != mask_ || (present & mask_) != mask_) return fail("stock TX DEC to AIF routing changed");
        for (unsigned dec = 0; dec < kDecs; ++dec) {
            if (!(mask_ & (1u << dec))) continue;
            for (auto* mux : {&dec_[dec], &dmic_[dec], &smic_[dec]}) {
                unsigned value;
                if (!enum_value(mux->id, value) || value != mux->initial) return fail("stock TX microphone mux changed");
            }
        }
        if (readback == cellular::MuteReadback::Lost) {
            if (lease_lost) *lease_lost = true;
            return fail("stock TX lease cleared on the same ALSA route");
        }
        return true;
    }

    // Call only after a complete fresh uplink write. Never reset and retry a
    // revoked heartbeat. The initial zero clears a previous expired lease.
    bool renew() {
        if (!ready_ || !writable_) return false;
        lost_on_renewal_ = false;
        return mute_attempt_.renew([&] { return stable() && write_mask(0) && stable(); }, [&] {
            long got;
            int error = 0;
            if (write_mask(mask_, &error) && scalar(mute_, got) && got == static_cast<long>(mask_)) return true;
            // Verify an established lease was cleared, rather than trying
            // another heartbeat. The callback still sees the old active flag.
            if (active() && (!error || error == ESTALE)) stable(nullptr, &lost_on_renewal_);
            return false;
        });
    }

    bool release() {
        return mute_attempt_.release([&] {
            long got;
            if (write_mask(0) && scalar(mute_, got) && got == 0) return true;
            fprintf(stderr, "[avcrtd:agm-tx] could not verify explicit unmute; kernel lease/retry is the fallback\n");
            return false;
        });
    }

    bool active() const { return mute_attempt_.active(); }
    bool lost_on_renewal() const { return lost_on_renewal_; }
    uint32_t mask() const { return mask_; }
    unsigned aif() const { return aif_; }
    bool microphone(std::string& mux, std::string& input) const {
        if (!ready_ || !mask_) return false;
        const auto dec = static_cast<unsigned>(__builtin_ctz(mask_));
        if (!enum_name(dec_[dec], mux)) return false;
        if (mux == "SWR_MIC") return enum_name(smic_[dec], input) && input != "ZERO";
        if (mux == "MSM_DMIC") return enum_name(dmic_[dec], input) && input != "ZERO";
        return false;
    }

private:
    bool enum_name(const Mux& mux, std::string& name) const {
        snd_ctl_elem_info info{};
        info.id = mux.id;
        info.value.enumerated.item = mux.initial;
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 ||
            info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED) return false;
        const auto* raw = info.value.enumerated.name;
        const auto size = strnlen(raw, sizeof(info.value.enumerated.name));
        if (!size || size == sizeof(info.value.enumerated.name)) return false;
        name.assign(raw, size);
        return true;
    }
    static bool number(std::string_view& text, unsigned& value) {
        if (text.empty()) return false;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        if (r.ec != std::errc{}) return false;
        text.remove_prefix(r.ptr - text.data());
        return true;
    }
    static bool unique(Id& dst, const Id& id) {
        if (dst.numid || !id.numid || id.device || id.subdevice || id.index) return false;
        dst = id;
        return true;
    }
    bool observe(std::string_view name, const Id& id, std::string_view map_name) {
        probe_.observe(name, id);
        if (name == map_name) return unique(map_, id);
        if (name == lpass::kNames[0]) return unique(power_, id);
        if (name == lpass::kNames[1]) return unique(test_, id);
        if (name == lpass::kNames[2]) return unique(mute_, id);
        constexpr std::string_view prefix = "TX_AIF", infix = "_CAP Mixer DEC";
        if (name.substr(0, prefix.size()) == prefix) {
            name.remove_prefix(prefix.size());
            unsigned aif, dec;
            if (!number(name, aif) || name.substr(0, infix.size()) != infix) return true;
            name.remove_prefix(infix.size());
            if (!number(name, dec) || !name.empty() || dec >= kDecs) return false;
            for (const auto& edge : edges_) if (edge.aif == aif && edge.dec == dec) return false;
            Id validated{};
            snd_ctl_elem_info info{};
            info.id = id;
            if (!unique(validated, id) || ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 ||
                info.count != 1 || !(info.access & SNDRV_CTL_ELEM_ACCESS_READ) ||
                (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.type != SNDRV_CTL_ELEM_TYPE_INTEGER)) return false;
            edges_.push_back({aif, dec, id});
            return true;
        }
        for (const auto p : {std::string_view("TX DEC"), std::string_view("TX DMIC MUX"),
                             std::string_view("TX SMIC MUX")}) {
            if (name.substr(0, p.size()) != p) continue;
            auto tail = name.substr(p.size());
            unsigned dec;
            if (!number(tail, dec) || tail != (p == "TX DEC" ? " MUX" : "")) continue;
            if (dec >= kDecs) return false;
            auto& mux = p == "TX DEC" ? dec_[dec] : p == "TX DMIC MUX" ? dmic_[dec] : smic_[dec];
            return unique(mux.id, id);
        }
        return true;
    }
    bool value(const Id& id, snd_ctl_elem_value& v) const {
        v.id = id;
        return id.numid && ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0 &&
               memcmp(&id, &v.id, sizeof(id)) == 0;
    }
    bool scalar(const Id& id, long& n) const {
        snd_ctl_elem_value v{};
        if (!value(id, v)) return false;
        n = v.value.integer.value[0];
        return true;
    }
    bool enum_value(const Id& id, unsigned& n) const {
        snd_ctl_elem_value v{};
        if (!value(id, v)) return false;
        n = v.value.enumerated.item[0];
        return true;
    }
    bool read_enum(const Id& id, unsigned& n) const {
        snd_ctl_elem_info i{};
        i.id = id;
        return id.numid && ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &i) == 0 &&
               i.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED && i.count == 1 &&
               (i.access & SNDRV_CTL_ELEM_ACCESS_READ) && enum_value(id, n) && n < i.value.enumerated.items;
    }
    bool control(const Id& id, lpass::Control& c) const {
        snd_ctl_elem_info i{};
        i.id = id;
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &i) < 0 || memcmp(&id, &i.id, sizeof(id))) return false;
        c.integer_scalar = i.type == SNDRV_CTL_ELEM_TYPE_INTEGER && i.count == 1;
        c.readable = i.access & SNDRV_CTL_ELEM_ACCESS_READ;
        c.writable = i.access & SNDRV_CTL_ELEM_ACCESS_WRITE;
        if (!c.integer_scalar || !c.readable) return true;
        c.min = i.value.integer.min;
        c.max = i.value.integer.max;
        return scalar(id, c.value);
    }
    bool read_map(uint32_t& mask) const {
        snd_ctl_elem_info i{};
        i.id = map_;
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &i) < 0 || i.type != SNDRV_CTL_ELEM_TYPE_BYTES ||
            i.count != 20 * sizeof(uint32_t) || !(i.access & SNDRV_CTL_ELEM_ACCESS_READ)) return false;
        snd_ctl_elem_value v{};
        uint32_t channels;
        if (!value(map_, v)) return false;
        memcpy(&channels, v.value.bytes.data, sizeof(channels));
        memcpy(&mask, v.value.bytes.data + sizeof(channels), sizeof(mask));
        return channels && channels == static_cast<unsigned>(__builtin_popcount(mask));
    }
    bool write_mask(long n, int* error = nullptr) const {
        snd_ctl_elem_value v{};
        v.id = mute_;
        v.value.integer.value[0] = n;
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) == 0) return true;
        const int code = errno;
        if (error) *error = code;
        fprintf(stderr, "[avcrtd:agm-tx] write DEC mute %#lx failed: %s\n", static_cast<unsigned long>(n), strerror(code));
        return false;
    }

    int fd_ = -1;
    unsigned aif_ = 0, device_ = 0, subdevices_ = 0;
    uint32_t mask_ = 0;
    bool writable_ = false, ready_ = false, lost_on_renewal_ = false;
    cellular::OneShotMute mute_attempt_;
    Id map_{}, power_{}, mute_{}, test_{};
    Mux dec_[kDecs]{}, dmic_[kDecs]{}, smic_[kDecs]{};
    std::vector<Edge> edges_;
    lpass::Probe<Id> probe_;
};

} // namespace avcrt::agm
