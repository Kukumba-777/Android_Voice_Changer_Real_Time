#pragma once

#include <cerrno>
#include <optional>

#include <cstdint>
#include <string>
#include <string_view>

#include "avcrt_mic_state.h"

namespace avcrt::cellular {

// Full binding contract for future adapters. The compatibility VA/AGM path
// below is best effort and does not claim this contract.
struct Capabilities {
    bool capture_exclusive = false;
    bool format_negotiated = false;
    bool uplink_bound_to_call = false;
    bool tx_mute_leased = false;
    bool route_observable = false;
    bool rollback_before_teardown = false;

    constexpr bool complete() const {
        return capture_exclusive && format_negotiated && uplink_bound_to_call &&
               tx_mute_leased && route_observable && rollback_before_teardown;
    }
};

enum class Backend { None, Legacy, CapabilityAdapter };

// Preserve an already-qualified legacy route. AGM never shadows an eligible
// legacy route, and partial capability reports always fail closed.
constexpr Backend select(bool legacy_eligible, const Capabilities& adapter) {
    if (legacy_eligible) return Backend::Legacy;
    return adapter.complete() ? Backend::CapabilityAdapter : Backend::None;
}

enum class AgmRecovery { None, ColdUplink, Transient, LeaseLost };

enum class MuteReadback { Expected, Lost, Conflict };

constexpr MuteReadback mute_readback(bool active, uint32_t mask, long mute, long test_mute) {
    if (test_mute) return MuteReadback::Conflict;
    if (mute == (active ? static_cast<long>(mask) : 0)) return MuteReadback::Expected;
    return active && mute == 0 ? MuteReadback::Lost : MuteReadback::Conflict;
}

// One fast and one later retry for a cold graph that never drained. An
// established path gets separate recovery for transient faults or a verified
// lease loss. Lease losses can recur on the same call, but cap each burst.
class AgmAttempts {
public:
    void disable() { generation_.clear(); reset_recovery(); lease_retries_ = 0; }
    void quarantine() { blocked_ = true; cancel_recovery(); }
    bool may_discover(bool enabled, bool legacy) const { return enabled && !legacy && !blocked_; }
    bool begin(std::string_view generation, int64_t revision, int64_t now_ms,
               agm::MicStatus mic = agm::MicStatus::Unavailable) {
        if (blocked_ || generation.empty()) return false;
        if (same_call(generation) && pending_ && now_ms < ready_ms_) return false;
        if (waiting_for_mic(generation)) {
            if (mic != agm::MicStatus::Clear || now_ms < earliest_mic_resume()) return false;
            cancel_recovery(); // A microphone pause must not refill the fault retry budgets.
            have_mic_resume_ = true;
            last_mic_resume_ms_ = now_ms;
            revision_ = revision;
            return true;
        }
        if (same_call(generation) && pending_) {
            cancel_recovery();
            revision_ = revision; // Use the latest preset without resetting the retry budget.
            return true;
        }
        if (current(generation, revision)) return false;
        if (!same_call(generation)) lease_retries_ = 0;
        generation_.assign(generation);
        revision_ = revision;
        reset_recovery();
        return true;
    }
    // Call only for an eligible exit after the worker has verified rollback.
    int recover(std::string_view generation, int64_t revision, AgmRecovery reason, int64_t now_ms,
                bool require_mic_clear = false) {
        if (blocked_ || !same_call(generation) || pending_ || mic_wait_) return 0;
        int delay;
        if (reason == AgmRecovery::ColdUplink && cold_retries_ < 2) {
            delay = cold_retries_++ == 0 ? 400 : 1200;
        } else if (reason == AgmRecovery::Transient && !live_retry_used_) {
            live_retry_used_ = true;
            delay = 400;
        } else if (reason == AgmRecovery::LeaseLost) {
            constexpr int64_t burst_ms = 5000;
            constexpr int delays[] = {100, 200, 400, 600};
            if (!lease_retries_ || now_ms - lease_burst_ms_ >= burst_ms) {
                lease_burst_ms_ = now_ms;
                lease_retries_ = 0;
            }
            if (lease_retries_ == sizeof(delays) / sizeof(delays[0])) return 0;
            delay = delays[lease_retries_++];
            // The last worker had already established a working uplink.
            // Its replacement may need the usual cold-start retries again.
            cold_retries_ = 0;
        } else {
            return 0;
        }
        revision_ = revision;
        ready_ms_ = now_ms + delay;
        pending_ = true;
        // A USB lease can be revoked before the microphone poll observes a
        // mute. Keep the retry pending, but do not reopen graphs until clear.
        mic_wait_ = require_mic_clear;
        mic_poll_ms_ = ready_ms_;
        return delay;
    }
    // MicWait is sent only once the previous worker has rolled back. While
    // waiting, neither a preset change nor an unknown report can start a worker.
    bool wait_for_mic(std::string_view generation, int64_t now_ms) {
        if (blocked_ || !same_call(generation)) return false;
        cancel_recovery();
        mic_wait_ = true;
        // Recheck promptly on first entry. If the previous clear observation
        // just launched a worker, cap the rate of repeated spawn/exit cycles.
        const auto earliest = earliest_mic_resume();
        mic_poll_ms_ = now_ms < earliest ? earliest : now_ms;
        return true;
    }
    bool waiting_for_mic(std::string_view generation) const { return mic_wait_ && same_call(generation); }
    bool mic_check_due(std::string_view generation, int64_t now_ms) {
        if (!waiting_for_mic(generation) || now_ms < mic_poll_ms_) return false;
        mic_poll_ms_ = now_ms + agm::MicPoll::interval_ms;
        return true;
    }
    bool pending() const { return pending_; }
    void cancel_recovery() { pending_ = mic_wait_ = false; }
    bool same_call(std::string_view generation) const {
        return !generation.empty() && generation == generation_;
    }
    bool current(std::string_view generation, int64_t revision) const {
        return same_call(generation) && revision == revision_;
    }

private:
    int64_t earliest_mic_resume() const {
        return have_mic_resume_ ? last_mic_resume_ms_ + agm::MicPoll::max_age_ms : 0;
    }
    void reset_recovery() {
        cancel_recovery();
        cold_retries_ = 0;
        live_retry_used_ = false;
        have_mic_resume_ = false;
    }
    std::string generation_;
    int64_t revision_ = -1, ready_ms_ = 0, lease_burst_ms_ = 0, mic_poll_ms_ = 0;
    int64_t last_mic_resume_ms_ = 0;
    unsigned cold_retries_ = 0, lease_retries_ = 0;
    bool blocked_ = false, pending_ = false, live_retry_used_ = false, mic_wait_ = false, have_mic_resume_ = false;
};

// SPF 0017 status reasons. Only use after a matching owner's confirmed GET 0
// and private graph cleanup. Do not reuse the old lease or its PCM evidence.
constexpr AgmRecovery usb_cut_recovery(bool previously_confirmed, int error, unsigned restore_reason) {
    if (!previously_confirmed)
        return error == EBUSY || error == ETIME ? AgmRecovery::Transient : AgmRecovery::None;
    constexpr unsigned expired = 1, graph_change = 2;
    return error == ETIME || (error == ESTALE && (restore_reason == expired || restore_reason == graph_change))
        ? AgmRecovery::LeaseLost : AgmRecovery::None;
}

enum class Block { Full, Pending, Failed, Late };
enum class PeriodCheck { Ready, Invalid, SkipLate };
enum class Step { Replaced, Priming, SkippedLate, Invalidated, CaptureFailed, DspFailed, UplinkFailed, LeaseFailed, Closed };

// One userspace lease attempt per binding. A failed write or readback may
// leave hardware muted, so release is still required. This guards retries;
// it does not replace the provider's atomic route check or expiry timer.
class OneShotMute {
public:
    template <class Prepare, class Heartbeat>
    bool renew(Prepare prepare, Heartbeat heartbeat) {
        if (closed_ || failed_) return false;
        if (!attempted_ && !prepare()) {
            failed_ = true;
            return false;
        }
        attempted_ = true; // Mark before hardware I/O, including partial failure.
        active_ = heartbeat();
        failed_ = !active_;
        return active_;
    }

    template <class Unmute>
    bool release(Unmute unmute) {
        if (closed_) return release_ok_;
        closed_ = true;
        active_ = false;
        release_ok_ = !attempted_ || unmute();
        return release_ok_;
    }

    bool active() const { return active_; }

private:
    bool attempted_ = false, active_ = false, failed_ = false;
    bool closed_ = false, release_ok_ = true;
};

// Live replacement period. Check the original call/route token and control
// state before offering SkipLate for timing alone. A production lease must also
// check the token atomically in the provider; polling here does not close that race.
// Construct after the stream owners so exceptions release before teardown.
template <class Lease>
class ReplacementSession {
public:
    explicit ReplacementSession(Lease& lease) : lease_(lease) {}
    ReplacementSession(const ReplacementSession&) = delete;
    ReplacementSession& operator=(const ReplacementSession&) = delete;
    ~ReplacementSession() { finish(); }

    template <class Valid, class Capture, class Process, class Write>
    Step step(Valid valid, Capture capture, Process process, Write write) {
        if (closed_) return Step::Closed;
        const auto check = [&](bool allow_skip = true) -> std::optional<Step> {
            switch (valid()) {
            case PeriodCheck::Invalid: return stop(Step::Invalidated);
            case PeriodCheck::SkipLate:
                if (!periods_) return stop(Step::Invalidated);
                if (allow_skip) return Step::SkippedLate;
                break;  // Don't mask a hard I/O or DSP error.
            case PeriodCheck::Ready: break;
            }
            return std::nullopt;
        };
        if (auto halt = check()) return *halt;
        const auto input = capture();
        if (auto halt = check(input != Block::Failed)) return *halt;
        if (input != Block::Full)
            return input == Block::Pending && !periods_ ? Step::Priming : stop(Step::CaptureFailed);
        const auto output = process();
        if (auto halt = check(output != Block::Failed)) return *halt;
        if (output != Block::Full)
            return output == Block::Pending && !periods_ ? Step::Priming : stop(Step::DspFailed);
        const auto submitted = write();
        if (submitted == Block::Failed) return stop(Step::UplinkFailed);
        if (auto halt = check()) return *halt;
        if (submitted == Block::Late) return stop(Step::Invalidated);
        // A full write may only queue a block. An adapter can keep stock TX
        // on while it waits for evidence that the uplink is draining.
        if (submitted == Block::Pending)
            return !periods_ ? Step::Priming : stop(Step::UplinkFailed);
        // Record the call before making it: an error can follow a partial write.
        lease_attempted_ = true;
        if (!lease_.renew()) return stop(Step::LeaseFailed);
        if (auto halt = check()) return *halt;
        ++periods_;
        return Step::Replaced;
    }

    bool finish() {
        if (closed_) return release_ok_;
        closed_ = true;
        release_ok_ = !lease_attempted_ || lease_.release();
        return release_ok_;
    }

    uint64_t periods() const { return periods_; }

private:
    Step stop(Step reason) { finish(); return reason; }
    Lease& lease_;
    uint64_t periods_ = 0;
    bool closed_ = false;
    bool lease_attempted_ = false;
    bool release_ok_ = true;
};

}  // namespace avcrt::cellular
