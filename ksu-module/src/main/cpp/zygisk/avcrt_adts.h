#pragma once

#include <cstddef>
#include <cstdint>

namespace avcrt::adts {

constexpr size_t kHeader = 7;
constexpr size_t kMaxFrame = 8191;
constexpr int kSamples = 1024;

inline int rate_index(int rate) {
    constexpr int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                             22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; ++i) if (rate == rates[i]) return i;
    return -1;
}

// This writer only accepts AAC-LC with a standard rate, 1024 samples per AU,
// mono/stereo channel configuration and no extensions (SBR/PS, PCE, etc.).
inline bool matches_asc(const uint8_t* data, size_t size, int rate, int channels) {
    const int index = rate_index(rate);
    if (!data || size != 2 || index < 0 || (channels != 1 && channels != 2)) return false;
    return data[0] == ((2 << 3) | (index >> 1)) &&
           data[1] == (((index & 1) << 7) | (channels << 3));
}

inline bool header(uint8_t* out, size_t payload, int rate, int channels) {
    const int index = rate_index(rate);
    if (!out || payload == 0 || payload > kMaxFrame - kHeader || index < 0 ||
        (channels != 1 && channels != 2)) return false;
    const size_t length = payload + kHeader;
    out[0] = 0xff;
    out[1] = 0xf1; // MPEG-4, no CRC.
    out[2] = (1 << 6) | (index << 2); // AAC-LC profile (object type minus one).
    out[3] = (channels << 6) | (length >> 11);
    out[4] = (length >> 3) & 0xff;
    out[5] = ((length & 7) << 5) | 0x1f;
    out[6] = 0xfc; // VBR fullness, one raw data block.
    return true;
}

struct Request {
    int source = -1;
    int format = -1;
    int encoder = -1;
    int rate = 0;
    int channels = 0;
    int bitrate = 0;
    int64_t max_ms = 0;
    int64_t max_bytes = 0;
    bool excluded = false;

    bool supported() const {
        return !excluded && source == 1 && format == 6 && encoder == 3 &&
            rate >= 8000 && rate <= 48000 && rate_index(rate) >= 0 &&
            (channels == 1 || channels == 2) && bitrate >= 8000 && bitrate <= 320000;
    }
};

// Check every complete AU before writing. Limits are independently optional;
// compare without adding untrusted lengths or multiplying a large time limit.
inline int limit(const Request& request, uint64_t bytes, size_t next, uint64_t frames) {
    if (request.max_bytes > 0 && (bytes >= uint64_t(request.max_bytes) ||
        next >= uint64_t(request.max_bytes) - bytes)) return 801;
    if (request.max_ms > 0 && request.rate > 0) {
        const auto ms = uint64_t(request.max_ms);
        const uint64_t seconds = frames / request.rate;
        if (seconds > ms / 1000 || (seconds == ms / 1000 &&
            (frames % request.rate) * 1000 / request.rate >= ms % 1000)) return 800;
    }
    return 0;
}

} // namespace avcrt::adts
