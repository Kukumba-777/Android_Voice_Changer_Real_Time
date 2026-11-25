#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace avcrt::agm {

// No heartbeat for a skipped period. Give a live graph a short chance to
// deliver fresh PCM before userspace rolls back (kernel lease is 250 ms).
class UplinkTiming {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kGrace = std::chrono::milliseconds{120};
    static constexpr unsigned kMaxDrops = 3;

    bool can_skip(Clock::time_point now) const {
        return have_renewal_ && consecutive_ < kMaxDrops && now - last_renewal_ < kGrace;
    }
    bool grace_elapsed(Clock::time_point now) const {
        return consecutive_ && now - last_renewal_ >= kGrace;
    }
    void skipped() { ++consecutive_; }
    void replaced(Clock::time_point renewal_started) {
        last_renewal_ = renewal_started;
        have_renewal_ = true;
        consecutive_ = 0;
    }

private:
    Clock::time_point last_renewal_{};
    unsigned consecutive_ = 0;
    bool have_renewal_ = false;
};

// Unlike a write, a nonblocking read that returned no bytes may simply be
// early. A partial block or other error is not safe to consume or retry.
inline bool empty_va_read(int rc, size_t bytes) {
    return bytes == 0 && (rc == 0 || rc == -EAGAIN || rc == -ENODATA || rc == -ENOSPC);
}

// An amplitude floor is not an uplink readiness check. Accept quiet, real
// Engine output after warm-up, but don't prime with an all-zero PCM block.
template <class Period>
bool nonzero_pcm(const Period& pcm) {
    for (const auto sample : pcm)
        if (sample) return true;
    return false;
}

// Never arm TX with silence. Once active allow a bounded run of explicit
// zero PCM while a new preset warms up or capture is briefly empty. Only a
// complete voice block resets the budget, not repeated preset changes.
class UplinkSilence {
public:
    static constexpr unsigned kMaxPeriods = 20;  // 200 ms at 10 ms per period

    template <class Period>
    bool prepare(bool active, bool full_voice, Period& pcm) {
        if (full_voice) { consecutive_ = 0; return true; }
        if (!active || consecutive_ >= kMaxPeriods) return false;
        ++consecutive_;
        pcm.fill(0);
        return true;
    }

private:
    unsigned consecutive_ = 0;
};

// One nonblocking write of exactly one configured AGM buffer. Do not infer
// acceptance from byte_count on error: vendor graph_write leaves it unchanged.
class UplinkWindow {
public:
    enum class Credit { Ready, Full, Invalid };
    explicit UplinkWindow(unsigned capacity) : capacity_(capacity) {}

    Credit credit(uint64_t done) const {
        if (done > sent_) return Credit::Invalid;
        return sent_ - done < capacity_ ? Credit::Ready : Credit::Full;
    }

    bool stalled_at_start(uint64_t done) const { return !done && credit(done) == Credit::Full; }

    bool accept(int rc, size_t reported, size_t expected) {
        if (rc || reported != expected) return false;
        ++sent_;
        return true;
    }

    bool draining(uint64_t done) const { return done != 0 && done <= sent_; }
    uint64_t sent() const { return sent_; }

    // Only wait on a full window, never retry an ambiguous or partial write.
    // The caller sets a deadline inside the current PCM period's freshness
    // budget and revalidates the route before submitting the held block.
    template <class Done, class Expired, class Pause>
    bool wait_writable(Done done, Expired expired, Pause pause) const {
        while (!expired()) {
            switch (credit(done())) {
            case Credit::Ready: return true;
            case Credit::Invalid: return false;
            case Credit::Full: pause(); break;
            }
        }
        return false;
    }

private:
    unsigned capacity_;
    uint64_t sent_ = 0;
};

// First lease only: attribute progress to the oldest accepted block(s).
// AGM exposes cumulative WRITE_DONE, not timestamps or PCM. Track the
// capture period's deadline, not when we first noticed the callback.
template <size_t Capacity>
class UplinkPriming {
public:
    using Clock = std::chrono::steady_clock;

    void accepted(uint64_t seq, Clock::time_point deadline, int64_t revision) {
        blocks_[seq % blocks_.size()] = {seq, deadline, revision};
    }

    void ring_filled(Clock::time_point now) { no_progress_by_ = now + std::chrono::milliseconds{20}; }

    bool no_progress(uint64_t done, Clock::time_point now) const {
        return !done && no_progress_by_ != Clock::time_point{} && now >= no_progress_by_;
    }

    bool ready(uint64_t done, Clock::time_point now, int64_t revision) const {
        return now < deadline(done, revision);
    }

    Clock::time_point deadline(uint64_t done, int64_t revision) const {
        const auto& block = blocks_[done % blocks_.size()];
        return done && block.seq == done && block.revision == revision ? block.deadline : Clock::time_point::min();
    }

private:
    struct Block {
        uint64_t seq = 0;
        Clock::time_point deadline{};
        int64_t revision = 0;
    };
    // Need the latest completed entry even while Capacity newer blocks are queued.
    std::array<Block, Capacity + 1> blocks_{};
    Clock::time_point no_progress_by_{};
};

} // namespace avcrt::agm
