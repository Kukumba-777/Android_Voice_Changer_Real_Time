#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace avcrt::capture_contract {

enum class Sample { Unsupported, S16, Float };

// AOSP audio_format_t. Four bytes per sample do not establish float PCM.
constexpr Sample audio_record_sample(int32_t format) {
    switch (format) {
        case 0x1: return Sample::S16;   // AUDIO_FORMAT_PCM_16_BIT
        case 0x5: return Sample::Float; // AUDIO_FORMAT_PCM_FLOAT
        default:  return Sample::Unsupported;
    }
}

constexpr size_t sample_bytes(Sample sample) {
    switch (sample) {
        case Sample::S16: return sizeof(int16_t);
        case Sample::Float: return sizeof(float);
        default: return 0;
    }
}

constexpr bool supported_dimensions(int64_t rate, int64_t channels, int64_t channel_limit) {
    // DSP support range, not a default or a hardware selector.
    return rate >= 8000 && rate <= 192000 && channels > 0 && channels <= channel_limit;
}

struct PcmSpan {
    size_t bytes = 0;
    size_t frame_bytes = 0;
};

inline PcmSpan pcm_span(const void* data, size_t frames, int64_t rate, int64_t channels,
                        Sample sample, int64_t channel_limit) {
    const size_t bps = sample_bytes(sample);
    if (!data || !frames || !bps || !supported_dimensions(rate, channels, channel_limit)) return {};
    const auto addr = reinterpret_cast<uintptr_t>(data);
    const auto ch = static_cast<size_t>(channels);
    const size_t frame_bytes = bps * ch;
    if (addr % bps || frames > std::numeric_limits<unsigned int>::max() / ch ||
        frames > (std::numeric_limits<uintptr_t>::max() - addr) / frame_bytes) return {};
    return {frames * frame_bytes, frame_bytes};
}

inline PcmSpan audio_record_span(const void* data, size_t frames, size_t bytes, int64_t rate,
                                 int64_t channels, Sample sample, int64_t channel_limit) {
    const auto span = pcm_span(data, frames, rate, channels, sample, channel_limit);
    // Buffer size is a consistency check, never the source of the PCM format.
    return span.bytes == bytes ? span : PcmSpan{};
}

inline bool valid_read_result(int32_t got, int32_t requested) {
    return got > 0 && got <= requested;
}

} // namespace avcrt::capture_contract
