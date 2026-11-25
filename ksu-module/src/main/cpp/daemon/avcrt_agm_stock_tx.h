#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace avcrt::agm {

template <size_t N>
bool parse_tx_fields(std::string_view line, std::array<uint64_t, N>& fields) {
    for (auto& field : fields) {
        if (line.empty()) return false;
        const auto result = std::from_chars(line.data(), line.data() + line.size(), field);
        if (result.ec != std::errc{} || result.ptr == line.data() + line.size()) return false;
        if (*result.ptr != (&field == &fields.back() ? '\n' : ' ')) return false;
        line.remove_prefix(result.ptr - line.data() + 1);
    }
    return line.empty() && fields[0] == 1;
}

struct StockTx {
    uint64_t epoch = 0, leg = 0;
    uint32_t vsid = 0, codec = 0, index = 0, slots = 0;

    static StockTx parse(std::string_view line) {
        std::array<uint64_t, 7> fields{};
        if (!parse_tx_fields(line, fields) || fields[2] > UINT32_MAX ||
            fields[4] > UINT32_MAX || fields[5] > UINT32_MAX || fields[6] > 0xffff ||
            !fields[2] || !fields[3] || !fields[4] || !fields[5] || !fields[6]) return {};
        return {fields[1], fields[3], static_cast<uint32_t>(fields[2]), static_cast<uint32_t>(fields[4]),
                static_cast<uint32_t>(fields[5]), static_cast<uint32_t>(fields[6])};
    }

    bool operator==(const StockTx& other) const {
        return epoch == other.epoch && leg == other.leg && vsid == other.vsid &&
               codec == other.codec && index == other.index && slots == other.slots;
    }
    bool matches(unsigned dma_index, uint32_t local_slots) const {
        return codec && index == dma_index && slots == local_slots;
    }
    bool separate(uint32_t candidate_codec, uint32_t candidate_read, uint32_t reserved_read) const {
        return codec && candidate_codec && candidate_codec != codec && candidate_read &&
               candidate_read != reserved_read;
    }
};

// Observation is not permission to open or mute a USB stream. Only codec
// routes can use the existing LPASS replacement provider.
struct StockRoute {
    enum class Kind { Unknown, Codec, UsbCapture };
    Kind kind = Kind::Unknown;
    StockTx tx{};
    uint32_t usb_miid = 0, usb_token = 0, usb_interval = 0;

    bool operator==(const StockRoute& other) const {
        return kind == other.kind && tx == other.tx && usb_miid == other.usb_miid &&
               usb_token == other.usb_token && usb_interval == other.usb_interval;
    }

    static StockRoute parse(std::string_view line) {
        std::array<uint64_t, 8> f{};
        if (!parse_tx_fields(line, f) || !f[2] || f[2] > UINT32_MAX || !f[3] ||
            !f[5] || f[5] > UINT32_MAX || f[6] > UINT32_MAX || f[7] > UINT32_MAX) return {};
        StockTx tx{f[1], f[3], static_cast<uint32_t>(f[2])};
        if (f[4] == 1 && f[6] && f[7] && f[7] <= 0xffff) {
            tx.codec = static_cast<uint32_t>(f[5]);
            tx.index = static_cast<uint32_t>(f[6]);
            tx.slots = static_cast<uint32_t>(f[7]);
            return {Kind::Codec, tx};
        }
        if (f[4] == 2 && (f[6] & 0xff) == 1 && (!f[7] || (f[7] >= 125 && f[7] <= 3276800)))
            return {Kind::UsbCapture, tx, static_cast<uint32_t>(f[5]),
                    static_cast<uint32_t>(f[6]), static_cast<uint32_t>(f[7])};
        return {};
    }
};

struct UsbStartEvidence {
    StockRoute route{};
    std::array<uint32_t, 8> ids{};
    size_t count = 0;

    bool contains(uint32_t id) const {
        for (size_t i = 0; i < count; ++i) if (ids[i] == id) return true;
        return false;
    }
    bool operator==(const UsbStartEvidence& other) const {
        return route == other.route && count == other.count && ids == other.ids;
    }

    static std::optional<UsbStartEvidence> parse(std::string_view line) {
        std::array<uint64_t, 8> fields{};
        UsbStartEvidence evidence;
        const auto value = [&](auto& out, char separator) {
            if (line.empty()) return false;
            const auto result = std::from_chars(line.data(), line.data() + line.size(), out);
            if (result.ec != std::errc{} || result.ptr == line.data() + line.size() ||
                *result.ptr != separator) return false;
            line.remove_prefix(result.ptr - line.data() + 1);
            return true;
        };
        for (auto& f : fields) if (!value(f, ' ')) return {};
        const auto [version, epoch, vsid, leg, miid, token, interval, count] = fields;
        if (version != 1 || !vsid || vsid > UINT32_MAX || !leg || !miid || miid > UINT32_MAX ||
            token > UINT32_MAX || (token & 0xff) != 1 || (interval && (interval < 125 || interval > 3276800)) ||
            !count || count > evidence.ids.size()) return {};
        while (evidence.count < count) {
            uint32_t id;
            if (!value(id, evidence.count + 1 == count ? '\n' : ' ') || !id || evidence.contains(id)) return {};
            evidence.ids[evidence.count++] = id;
        }
        if (!line.empty()) return {};
        evidence.route = {StockRoute::Kind::UsbCapture, {epoch, leg, static_cast<uint32_t>(vsid)},
                          static_cast<uint32_t>(miid), static_cast<uint32_t>(token), static_cast<uint32_t>(interval)};
        return evidence;
    }
};

} // namespace avcrt::agm
