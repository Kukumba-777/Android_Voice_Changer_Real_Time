#pragma once

#include <cstddef>
#include <cstdint>

namespace avcrt {

// Convert Android ENCODING_PCM_24BIT_PACKED (signed little-endian) to the
// engine's S16 domain and back. The low eight bits are intentionally discarded
// on input and zeroed on output; this keeps sign and frame layout deterministic.
inline int16_t pcm24_to_s16(const uint8_t* sample) {
    const uint32_t value = static_cast<uint32_t>(sample[0]) |
        (static_cast<uint32_t>(sample[1]) << 8) |
        (static_cast<uint32_t>(sample[2]) << 16);
    const int32_t signed24 = (value & 0x800000u)
        ? static_cast<int32_t>(value | 0xff000000u)
        : static_cast<int32_t>(value);
    return static_cast<int16_t>(signed24 >> 8);
}

inline void s16_to_pcm24(int16_t sample, uint8_t* packed) {
    const uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(sample) * 256);
    packed[0] = static_cast<uint8_t>(value);
    packed[1] = static_cast<uint8_t>(value >> 8);
    packed[2] = static_cast<uint8_t>(value >> 16);
}

inline bool pcm24_bytes_are_whole_samples(size_t bytes) { return bytes % 3 == 0; }

} // namespace avcrt
