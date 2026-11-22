#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "avcrt_paths.h"

namespace avcrt::soundboard {

// Wire contract with HalSoundboard: two little-endian uint64 values followed by mono PCM16LE.
// Android ABIs in the module are little endian. Keep these values in sync with the Android writer.
inline constexpr uint64_t kMagic = 0x314D435054524356ULL;
inline constexpr size_t kRate = 48000;
inline constexpr size_t kMaxFrames = kRate * 30;
inline constexpr float kClipGain = 0.65f;
struct Header { uint64_t magic, start_ms; };
static_assert(sizeof(Header) == 16);

struct Clip {
    uint64_t start_ms;
    std::vector<int16_t> pcm;
};

struct Cursor {
    uint64_t start_ms = 0;
    double position = 0;
};

inline uint64_t boot_ms() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

inline bool is_expired(const Clip& clip, uint64_t now) {
    return now >= clip.start_ms + (clip.pcm.size() * 1000 + kRate - 1) / kRate;
}

// Each consumer follows the same boot-clock timeline; the cursor keeps adjacent buffers
// sample-contiguous instead of seeking on every small scheduling jitter.
template <typename Add>
void mix(const Clip& clip, Cursor& cursor, uint64_t now, size_t frames, uint32_t rate, Add add) {
    if (!rate || now < clip.start_ms || is_expired(clip, now) || clip.pcm.empty()) return;
    const double desired = (now - clip.start_ms) * (kRate / 1000.0);
    const double step = static_cast<double>(kRate) / rate;
    if (cursor.start_ms != clip.start_ms || std::abs(cursor.position - desired) > kRate / 10) {
        cursor.start_ms = clip.start_ms;
        cursor.position = desired;
    }
    for (size_t i = 0; i < frames; ++i, cursor.position += step) {
        const auto index = static_cast<size_t>(cursor.position);
        if (index >= clip.pcm.size()) break;
        const float a = clip.pcm[index];
        const float b = clip.pcm[std::min(index + 1, clip.pcm.size() - 1)];
        const float value = (a + (b - a) * static_cast<float>(cursor.position - index)) * kClipGain;
        if (value != 0) add(i, value);
    }
}

class Source {
public:
    static Source* instance() {
        // Process-lifetime worker. Forked app processes first use this after specialization.
        // Failure to start the soundboard must not prevent normal voice processing.
        static Source* source = []() noexcept -> Source* {
            try { return new Source; } catch (...) { return nullptr; }
        }();
        return source;
    }

    template <class Add>
    void render(Cursor& cursor, size_t frames, uint32_t rate, Add add) {
        const auto now = boot_ms();
        last_capture_ms_.store(now, std::memory_order_relaxed);
        const auto clip = std::atomic_load_explicit(&current_, std::memory_order_acquire);
        if (clip) mix(*clip, cursor, now, frames, rate, add);
    }

private:
    Source() { std::thread([this] { watch(); }).detach(); }

    static std::shared_ptr<const Clip> read_clip(int fd, const struct stat& st, uint64_t now) {
        if (!S_ISREG(st.st_mode) || st.st_size <= static_cast<off_t>(sizeof(Header))) return {};
        const auto bytes = st.st_size - sizeof(Header);
        if (bytes % sizeof(int16_t) || bytes / sizeof(int16_t) > kMaxFrames) return {};
        Header header{};
        if (pread(fd, &header, sizeof(header), 0) != sizeof(header) || header.magic != kMagic ||
            header.start_ms > now + 1000 || now >= header.start_ms + (bytes / sizeof(int16_t) * 1000 + kRate - 1) / kRate) return {};
        auto clip = std::make_shared<Clip>();
        clip->start_ms = header.start_ms;
        clip->pcm.resize(bytes / sizeof(int16_t));
        if (pread(fd, clip->pcm.data(), bytes, sizeof(Header)) != static_cast<ssize_t>(bytes)) return {};
        return clip;
    }

    void watch() {
        struct stat previous{};
        bool known = false;
        uint64_t end = 0;
        for (;;) {
            const auto now = boot_ms();
            struct stat st{};
            if (lstat(paths::kSoundboardClip, &st) != 0) {
                if (known) publish({});
                known = false;
                end = 0;
            } else if (!known || st.st_ino != previous.st_ino || st.st_dev != previous.st_dev ||
                       st.st_ctim.tv_sec != previous.st_ctim.tv_sec || st.st_ctim.tv_nsec != previous.st_ctim.tv_nsec) {
                const int fd = open(paths::kSoundboardClip, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                std::shared_ptr<const Clip> clip;
                if (fd >= 0) {
                    if (fstat(fd, &st) == 0) {
                        try { clip = read_clip(fd, st, now); } catch (...) {}
                    }
                    close(fd);
                }
                end = clip ? clip->start_ms + (clip->pcm.size() * 1000 + kRate - 1) / kRate : 0;
                publish(std::move(clip));
                previous = st;
                known = true;
            } else if (end && now >= end) {
                publish({});
                end = 0;
            }
            retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                          [](const auto& old) { return old.use_count() == 1; }), retired_.end());
            const auto last = last_capture_ms_.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(now < last + 1000 ? 30 : 250));
        }
    }

    void publish(std::shared_ptr<const Clip> clip) {
        // Retain the previous allocation on the worker until readers release it.
        auto previous = std::atomic_exchange_explicit(&current_, std::move(clip), std::memory_order_acq_rel);
        if (previous) {
            try { retired_.push_back(std::move(previous)); } catch (...) {}
        }
    }

    std::atomic<uint64_t> last_capture_ms_{boot_ms()};
    std::shared_ptr<const Clip> current_;
    std::vector<std::shared_ptr<const Clip>> retired_;
};

} // namespace avcrt::soundboard
