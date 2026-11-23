#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
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

// Wire contract with HalSoundboard. The PCM16LE file has no header and is immutable.
// Replacing the small marker starts playback; the inode binds it to the published PCM link.
inline constexpr uint64_t kMagic = 0x324D435054524356ULL;
inline constexpr uint64_t kRate = 48000;
inline constexpr uint64_t kFramesPerMs = kRate / 1000;
inline constexpr float kClipGain = 0.65f;
struct Header { uint64_t magic, start_ms, volume_percent, pcm_ino; };
static_assert(sizeof(Header) == 32);

struct ClipInfo {
    uint64_t start_ms;
    uint64_t frames;
    float gain = kClipGain;
};

// Immutable snapshot of one part of the file; shared by all capture streams in a process.
struct Window {
    ClipInfo clip;
    uint64_t first;
    std::vector<int16_t> pcm;
};

inline constexpr uint64_t kWindowFrames = 2 * kRate;
inline constexpr uint64_t kWindowHop = kRate;
// Leave room behind the next window's boundary for cursors that are slightly behind the clock.
inline constexpr uint64_t kLookBehind = kRate / 4;

struct Cursor {
    uint64_t start_ms = 0;
    double position = 0;
};

inline uint64_t boot_ms() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

inline bool is_expired(const ClipInfo& clip, uint64_t now) {
    const auto duration = clip.frames / kFramesPerMs + (clip.frames % kFramesPerMs != 0);
    return now >= clip.start_ms && now - clip.start_ms >= duration;
}

inline uint64_t frame_at(const ClipInfo& clip, uint64_t now) {
    // Only called before expiration: the multiplication stays within clip.frames.
    return now > clip.start_ms ? (now - clip.start_ms) * kFramesPerMs : 0;
}

inline uint64_t window_start(uint64_t frame) {
    const auto boundary = frame / kWindowHop * kWindowHop;
    return boundary > kLookBehind ? boundary - kLookBehind : 0;
}

// Each consumer follows the same boot-clock timeline; the cursor keeps adjacent buffers
// sample-contiguous instead of seeking on every small scheduling jitter.
template <typename Add>
void mix(const Window& window, Cursor& cursor, uint64_t now, size_t frames, uint32_t rate, Add add) {
    const auto& clip = window.clip;
    if (!rate || now < clip.start_ms || is_expired(clip, now)) return;
    const double desired = static_cast<double>(frame_at(clip, now));
    const double step = static_cast<double>(kRate) / rate;
    if (!clip.gain) return;
    if (cursor.start_ms != clip.start_ms || std::abs(cursor.position - desired) > kRate / 10) {
        cursor.start_ms = clip.start_ms;
        cursor.position = desired;
    }
    for (size_t i = 0; i < frames; ++i, cursor.position += step) {
        const auto index = static_cast<uint64_t>(cursor.position);
        if (index >= clip.frames) break;
        if (index < window.first) continue;
        const auto local = index - window.first;
        const auto next = local + (index + 1 < clip.frames);
        // If the worker hasn't loaded this part yet, pass the mic through unchanged.
        if (next >= window.pcm.size()) break;
        const float a = window.pcm[local];
        const float b = window.pcm[next];
        const float value = (a + (b - a) * static_cast<float>(cursor.position - index)) * clip.gain;
        if (value != 0) add(i, value);
    }
}

inline bool read_exact(int fd, void* buffer, size_t bytes, off64_t offset) {
    auto* out = static_cast<char*>(buffer);
    while (bytes) {
        const auto count = pread64(fd, out, bytes, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        out += count;
        offset += count;
        bytes -= count;
    }
    return true;
}

inline bool read_info(const Header& header, const struct stat64& st, uint64_t now, ClipInfo& clip) {
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || header.pcm_ino != st.st_ino) return false;
    const uint64_t bytes = st.st_size;
    if (bytes % sizeof(int16_t)) return false;
    if (header.magic != kMagic || header.volume_percent > 100 ||
        (header.start_ms > now && header.start_ms - now > 1000)) return false;
    clip = {header.start_ms, bytes / sizeof(int16_t), kClipGain * (header.volume_percent / 100.0f)};
    return !is_expired(clip, now);
}

inline std::shared_ptr<const Window> read_window(int fd, const ClipInfo& clip, uint64_t first) {
    if (first >= clip.frames) return {};
    auto window = std::make_shared<Window>();
    window->clip = clip;
    window->first = first;
    window->pcm.resize(std::min(kWindowFrames, clip.frames - first));
    const auto offset = static_cast<off64_t>(first * sizeof(int16_t));
    if (!read_exact(fd, window->pcm.data(), window->pcm.size() * sizeof(int16_t), offset)) return {};
    return window;
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
        const auto window = std::atomic_load_explicit(&current_, std::memory_order_acquire);
        if (window) mix(*window, cursor, now, frames, rate, add);
    }

private:
    Source() { std::thread([this] { watch(); }).detach(); }

    static bool same_file(const struct stat64& a, const struct stat64& b) {
        return a.st_ino == b.st_ino && a.st_dev == b.st_dev &&
               a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }

    void watch() {
        struct stat64 previous{};
        bool known = false;
        int fd = -1;
        ClipInfo clip{};
        uint64_t loaded_first = UINT64_MAX;
        const auto reset = [&] {
            if (fd >= 0) close(fd);
            fd = -1;
            loaded_first = UINT64_MAX;
            publish({});
        };
        for (;;) {
            const auto now = boot_ms();
            const auto last = last_capture_ms_.load(std::memory_order_relaxed);
            const bool active = now < last || now - last < 1000;
            struct stat64 st{};
            if (lstat64(paths::kSoundboardPlayback, &st) != 0) {
                if (known) reset();
                known = false;
            } else if (!known || !same_file(st, previous)) {
                reset();
                previous = st;
                known = true;
                const int marker = open64(paths::kSoundboardPlayback, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                Header header{};
                if (marker >= 0) {
                    if (fstat64(marker, &st) == 0 && same_file(st, previous) &&
                        S_ISREG(st.st_mode) && st.st_size == static_cast<off64_t>(sizeof(Header)) &&
                        read_exact(marker, &header, sizeof(header), 0)) {
                        fd = open64(paths::kSoundboardClip, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                        if (fd >= 0 && (fstat64(fd, &st) != 0 || !read_info(header, st, now, clip))) reset();
                    }
                    close(marker);
                }
            }
            if (fd >= 0 && is_expired(clip, now)) reset();
            if (fd >= 0 && active) {
                const auto first = window_start(frame_at(clip, now));
                if (first != loaded_first) {
                    try {
                        if (auto window = read_window(fd, clip, first)) {
                            publish(std::move(window));
                            loaded_first = first;
                        }
                    } catch (...) {} // Let capture continue even if allocation or disk access fails.
                }
            }
            retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                          [](const auto& old) { return old.use_count() == 1; }), retired_.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(active ? 20 : 100));
        }
    }

    void publish(std::shared_ptr<const Window> window) {
        // Retain the previous allocation on the worker until readers release it.
        auto previous = std::atomic_exchange_explicit(&current_, std::move(window), std::memory_order_acq_rel);
        if (previous) {
            try { retired_.push_back(std::move(previous)); } catch (...) {}
        }
    }

    std::atomic<uint64_t> last_capture_ms_{boot_ms()};
    std::shared_ptr<const Window> current_;
    std::vector<std::shared_ptr<const Window>> retired_;
};

} // namespace avcrt::soundboard
