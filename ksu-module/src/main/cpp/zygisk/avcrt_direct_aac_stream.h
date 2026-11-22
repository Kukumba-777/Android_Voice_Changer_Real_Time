#pragma once

#include "avcrt_adts.h"

#include <array>
#include <cstring>

namespace avcrt::direct_aac {

// Byte streams may split anywhere, including inside the seven-byte header.
// Never scan for sync: an apparent syncword inside a payload is not evidence
// of framing. The camera consumes exactly one raw AAC-LC block per ADTS AU.
enum class Parse { NeedBytes, Complete, Unsupported };

inline Parse frame_size(const uint8_t* data, size_t bytes, int rate, int channels, size_t& frame) {
    frame = 0;
    if (bytes < adts::kHeader) return Parse::NeedBytes;
    const int index = adts::rate_index(rate);
    const int ch = ((data[2] & 1) << 2) | (data[3] >> 6);
    if (index < 0 || (channels != 1 && channels != 2) || data[0] != 0xff ||
        (data[1] & 0xf7) != 0xf1 || (data[2] >> 6) != 1 || ((data[2] >> 2) & 15) != index ||
        ch != channels || (data[6] & 3)) return Parse::Unsupported;
    frame = ((data[3] & 3) << 11) | (data[4] << 3) | (data[5] >> 5);
    if (frame <= adts::kHeader || frame > adts::kMaxFrame) return Parse::Unsupported;
    return bytes < frame ? Parse::NeedBytes : Parse::Complete;
}

struct Prefix { size_t bytes = 0, frames = 0; };

inline Prefix whole_frames(const uint8_t* data, size_t bytes, size_t capacity, int rate, int channels) {
    Prefix prefix;
    while (prefix.bytes < bytes) {
        size_t frame = 0;
        if (frame_size(data + prefix.bytes, bytes - prefix.bytes, rate, channels, frame) != Parse::Complete ||
            frame > capacity - prefix.bytes) break;
        prefix.bytes += frame;
        ++prefix.frames;
    }
    return prefix;
}

template <size_t Capacity>
class Bytes {
public:
    const uint8_t* data() const { return bytes_.data() + start_; }
    size_t size() const { return end_ - start_; }
    size_t room() const { return Capacity - size(); }
    void clear() { start_ = end_ = 0; }

    bool append(const uint8_t* data, size_t len) {
        if (len > room()) return false;
        if (!len) return true;
        if (len > Capacity - end_) {
            const size_t held = size();
            std::memmove(bytes_.data(), this->data(), held);
            start_ = 0;
            end_ = held;
        }
        std::memcpy(bytes_.data() + end_, data, len);
        end_ += len;
        return true;
    }

    bool discard(size_t count) {
        if (count > size()) return false;
        start_ += count;
        if (start_ == end_) clear();
        return true;
    }

private:
    std::array<uint8_t, Capacity> bytes_{};
    size_t start_ = 0, end_ = 0;
};

// Keep original reads in order until the first replacement AU is published.
// A declined probe can replay every byte verbatim, even after codec parsing
// consumed a prefix. After commit those bytes can never be published.
class Journal {
public:
    enum class Mode { Probe, Original, Transcoded, Failed };
    Mode mode() const { return mode_; }
    size_t room() const { return original_.room(); }
    bool pending() const { return original_.size() != 0; }
    bool note(const uint8_t* data, size_t size) {
        return mode_ == Mode::Probe && original_.append(data, size);
    }
    void decline() { if (mode_ == Mode::Probe) mode_ = Mode::Original; }
    size_t replay(uint8_t* data, size_t capacity) {
        if (mode_ != Mode::Original) return 0;
        const size_t count = capacity < original_.size() ? capacity : original_.size();
        if (count) std::memcpy(data, original_.data(), count);
        original_.discard(count);
        return count;
    }
    void commit() {
        if (mode_ != Mode::Probe) return;
        mode_ = Mode::Transcoded;
        original_.clear();
    }
    void fail() { mode_ = Mode::Failed; original_.clear(); }

private:
    Mode mode_ = Mode::Probe;
    Bytes<2 * adts::kMaxFrame> original_;
};

} // namespace avcrt::direct_aac
