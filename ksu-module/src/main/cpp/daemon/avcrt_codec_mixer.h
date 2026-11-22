#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace avcrt::codec_mixer {

// ALSA's boolean/integer payload can hold up to 128 values. The codec flow
// expects switches or scalar muxes, never a multi-value integer/enum control.
inline constexpr unsigned kMaxValues = 128;
enum class Kind { Unsupported, Switch, Enum };

struct Edit {
    std::string name;
    Kind kind;
    unsigned count;
    unsigned index;
    int before;
    int written;
    bool spare;
};

inline bool supported(Kind kind, unsigned count) {
    return count > 0 && (kind == Kind::Switch ? count <= kMaxValues :
                        kind == Kind::Enum && count == 1);
}

template <typename Control>
bool off(const Control& ctl) {
    if (ctl.kind() != Kind::Switch || !supported(ctl.kind(), ctl.count())) return false;
    for (unsigned i = 0; i < ctl.count(); ++i) {
        if (ctl.get(i) != 0) return false;
    }
    return true;
}

struct DecActivity {
    unsigned capture = 0;
    unsigned elsewhere = 0;

    template <typename Control>
    void inspect(const char* name, int cap_aif, const Control& ctl) {
        int aif, dec, end = 0;
        if (!name || std::sscanf(name, "TX_AIF%d_CAP Mixer DEC%d%n", &aif, &dec, &end) != 2 ||
            !end || name[end] || dec < 0 || dec >= 8 || off(ctl)) return;
        (aif == cap_aif ? capture : elsewhere) |= 1u << dec;
    }
};

template <typename Control>
int enum_value(const Control& ctl, const char* value) {
    if (ctl.kind() != Kind::Enum || !supported(ctl.kind(), ctl.count())) return -1;
    const unsigned count = ctl.enums();
    const int live = ctl.get(0);
    if (live < 0 || static_cast<unsigned>(live) >= count) return -1;
    for (unsigned i = 0; i < count; ++i) {
        const char* item = ctl.enum_name(i);
        if (item && std::strcmp(item, value) == 0) return static_cast<int>(i);
    }
    return -1;
}

struct SpareDec {
    int index = -1;
    char modem_switch[40]{};
    char capture_switch[40]{};
    char dec_mux[24]{};
    char source_mux[24]{};
};

template <typename Lookup>
SpareDec pick_spare_dec(unsigned unavailable, int modem_aif, int cap_aif,
                        const char* dec_source, const char* mic_source, Lookup lookup) {
    // Keep the preference for higher indices, but require a complete, idle tap.
    // A missing route on a codec with fewer decimators is not an idle route.
    for (int i = 3; i < 11; ++i) {
        const int dec = i % 8;
        if (unavailable & (1u << dec)) continue;
        SpareDec spare;
        std::snprintf(spare.modem_switch, sizeof(spare.modem_switch), "TX_AIF%d_CAP Mixer DEC%d", modem_aif, dec);
        std::snprintf(spare.capture_switch, sizeof(spare.capture_switch), "TX_AIF%d_CAP Mixer DEC%d", cap_aif, dec);
        if (!off(lookup(spare.modem_switch)) || !off(lookup(spare.capture_switch))) continue;

        std::snprintf(spare.dec_mux, sizeof(spare.dec_mux), "TX DEC%d MUX", dec);
        std::snprintf(spare.source_mux, sizeof(spare.source_mux), "TX %s MUX%d",
                      std::strcmp(dec_source, "SWR_MIC") == 0 ? "SMIC" : "DMIC", dec);
        if (enum_value(lookup(spare.dec_mux), dec_source) < 0 ||
            enum_value(lookup(spare.source_mux), mic_source) < 0) continue;

        spare.index = dec;
        return spare;
    }
    return {};
}

template <typename Control>
bool write(const Control& ctl, std::vector<Edit>& edits, const char* name, int value, bool spare) {
    const Kind kind = ctl.kind();
    const unsigned count = ctl.count();
    if (!supported(kind, count) || value < 0 ||
        (kind == Kind::Switch ? value > 1 : static_cast<unsigned>(value) >= ctl.enums())) return false;

    std::array<int, kMaxValues> before{};
    for (unsigned i = 0; i < count; ++i) {
        before[i] = ctl.get(i);
        // An active switch is owned by someone else; don't take over any slot.
        if (before[i] < 0 || (kind == Kind::Switch && before[i] != 0) ||
            (kind == Kind::Enum && static_cast<unsigned>(before[i]) >= ctl.enums())) return false;
    }
    const auto first = edits.size();
    for (unsigned i = 0; i < count; ++i) {
        if (ctl.get(i) != before[i]) return false;
        edits.push_back({name, kind, count, i, before[i], value, spare});
        if (before[i] == value) continue;
        // An error can still leave a driver-side edit. Journal before the write.
        if (!ctl.set(i, value)) return false;
        const int live = ctl.get(i);
        if (live == value) continue;
        // Some routing controls advertise secondary boolean slots that the
        // driver ignores. Only a verified primary route bit can enable a tap.
        if (live != before[i] || i == 0) return false;
        edits.back().written = before[i]; // Assert the slot stayed off; don't own it.
    }
    for (unsigned i = 0; i < count; ++i) {
        if (ctl.get(i) != edits[first + i].written) return false;
    }
    return true;
}

enum class Restore { Restored, NotOurs, Failed };

template <typename Control>
bool matches(const Control& ctl, const Edit& edit) {
    return ctl.kind() == edit.kind && ctl.count() == edit.count && edit.index < edit.count &&
        ctl.get(edit.index) == edit.written;
}

template <typename Control>
Restore restore(const Control& ctl, const Edit& edit) {
    if (edit.before == edit.written) return Restore::NotOurs;
    if (ctl.kind() != edit.kind || ctl.count() != edit.count || edit.index >= edit.count) {
        return Restore::Failed;
    }
    const int live = ctl.get(edit.index);
    if (live < 0) return Restore::Failed;
    if (live != edit.written) return Restore::NotOurs;
    if (!ctl.set(edit.index, edit.before) || ctl.get(edit.index) != edit.before) return Restore::Failed;
    return Restore::Restored;
}

} // namespace avcrt::codec_mixer
