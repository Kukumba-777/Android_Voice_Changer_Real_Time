#include "avcrt_agm_stock_tx.h"
#include "avcrt_agm_uplink.h"
#include "avcrt_cellular_backend.h"
#include "avcrt_mic_state.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <string>

using namespace avcrt::cellular;

namespace {
struct Streams {
    std::string& log;
    ~Streams() { log += "tx"; }  // uplink then capture teardown
};

struct Lease {
    std::string& log;
    int error = 0;
    bool release_ok = true;
    bool muted = false;
    bool renew() {
        log += 'M';
        muted = true;  // model a partial hardware write even on failure
        return !error;
    }
    bool release() {
        log += '0';
        if (release_ok) muted = false;
        return release_ok;
    }
};

struct Input {
    uint64_t call = 1, route = 1;
    bool enabled = true, fresh = true, skip_late = false;
    Block capture = Block::Full, dsp = Block::Full, write = Block::Full;
    char invalidate_on = '\0';

    Step run(ReplacementSession<Lease>& session, std::string& log) {
        const auto begin = log.size();
        const auto event = [&](char c) {
            log += c;
            if (c == invalidate_on) fresh = false;
        };
        return session.step(
            [&] {
                if (invalidate_on == 'M' && log.size() > begin && log.back() == 'M') fresh = false;
                if (call != 1 || route != 1 || !enabled) return PeriodCheck::Invalid;
                return fresh ? PeriodCheck::Ready : skip_late ? PeriodCheck::SkipLate : PeriodCheck::Invalid;
            },
            [&] { event('R'); return capture; },
            [&] { event('D'); return dsp; },
            [&] { event('W'); return write; });
    }
};

template <class Test>
void check(Test test, const char* expected) {
    std::string log;
    Lease lease{log};
    {
        Streams streams{log};
        ReplacementSession session(lease);
        Input input;
        test(session, input, lease, log);
    }
    assert(log == expected);
    assert(!lease.muted || !lease.release_ok);
}

// Fake clock and DSP callback for the same ring admission and replacement
// transaction as production. Each W is an actual submission, not a retry.
struct QueuedUplink {
    static constexpr size_t kBytes = 1920;
    avcrt::agm::UplinkWindow window{4};
    uint64_t done = 0;
    unsigned waited = 0, complete_at = 0, wait_budget = 10;
    bool route_valid = true, reroute_on_completion = false;
    int rc = 0;
    size_t reported = kBytes;

    Step run(ReplacementSession<Lease>& session, std::string& log) {
        return session.step(
            [&] { return route_valid ? PeriodCheck::Ready : PeriodCheck::Invalid; },
            [] { return Block::Full; }, [] { return Block::Full; },
            [&] {
                if (!window.wait_writable([&] { return done; }, [&] { return waited >= wait_budget; }, [&] {
                        if (++waited == complete_at) {
                            ++done;
                            if (reroute_on_completion) route_valid = false;
                        }
                    }) || !route_valid) return Block::Failed;
                log += 'W';
                if (!window.accept(rc, reported, kBytes)) return Block::Failed;
                return window.draining(done) ? Block::Full : Block::Pending;
            });
    }

    void fill(ReplacementSession<Lease>& s, Lease& l, std::string& log) {
        for (unsigned i = 0; i < 4; ++i) assert(run(s, log) == Step::Priming && !l.muted);
        assert(window.sent() == 4 && done == 0 && waited == 0 && s.periods() == 0);
    }
};
} // namespace

int main() {
    using avcrt::agm::StockTx;
    const auto handset = StockTx::parse("1 42 1234 7 901 4 12\n");
    const auto speaker = StockTx::parse("1 49 1234 8 902 4 12\n");
    assert(handset.matches(4, 12) && speaker.matches(4, 12) && !(handset == speaker));
    assert(!speaker.matches(5, 12) && !speaker.matches(4, 16));
    assert(handset.separate(902, 702, 701) && speaker.separate(901, 702, 701));
    assert(!speaker.separate(902, 702, 701) && !speaker.separate(901, 701, 701));
    assert(!StockTx::parse("1 49 1234 0 0 0 0\n").codec);
    assert(!StockTx::parse("1 49 1234 8 902 4 65536\n").codec);
    assert(!StockTx::parse("1 49 1234 8 4294967296 4 12\n").codec);
    assert(!StockTx::parse("1 49 1234 8 902 4 12\nextra").codec);
    assert(!StockTx::parse("2 49 1234 8 902 4 12\n").codec);
    assert(!StockTx::parse("1 49 1234 8 902 4 12").codec);
    using avcrt::agm::StockRoute;
    const auto codec_route = StockRoute::parse("1 49 1234 8 1 902 4 12\n");
    assert(codec_route.kind == StockRoute::Kind::Codec && codec_route.tx == speaker);
    assert(!codec_route.usb_miid);
    const auto usb_route = StockRoute::parse("1 34 299651072 7 2 17251 65537 0\n");
    assert(usb_route.kind == StockRoute::Kind::UsbCapture && usb_route.usb_miid == 0x4363 &&
           usb_route.usb_token == 0x10001 && !usb_route.usb_interval);
    assert(!usb_route.tx.codec && !usb_route.tx.matches(4, 12));
    assert(usb_route == StockRoute::parse("1 34 299651072 7 2 17251 65537 0\n"));
    for (const auto* line : {"1 35 299651072 7 2 17251 65537 0\n", "1 34 299651072 8 2 17251 65537 0\n",
                             "1 34 299651073 7 2 17251 65537 0\n", "1 34 299651072 7 2 17252 65537 0\n",
                             "1 34 299651072 7 2 17251 65793 0\n", "1 34 299651072 7 2 17251 65537 125\n",
                             "1 34 299651072 7 1 17251 4 12\n", "1 35 0 0 0 0 0 0\n"})
        assert(!(usb_route == StockRoute::parse(line)));
    for (const auto* line : {"1 50 1234 0 0 0 0 0\n", "1 50 1234 9 2 903 132864 0\n",
                             "1 50 1234 9 2 903 132865 124\n", "1 50 1234 9 3 903 132865 0\n",
                             "1 50 1234 9 2 0 132865 0\n", "1 50 1234 9 2 4294967296 132865 0\n"})
        assert(StockRoute::parse(line).kind == StockRoute::Kind::Unknown);
    using avcrt::agm::UsbStartEvidence;
    const auto evidence = UsbStartEvidence::parse("1 34 299651072 7 17251 65537 0 2 4321 4322\n");
    assert(evidence && evidence->route == usb_route && evidence->count == 2 &&
           evidence->contains(4322) && !evidence->contains(0));
    const auto stale = UsbStartEvidence::parse("1 34 299651072 8 17251 65537 0 2 4321 4322\n");
    assert(stale && !(stale->route == usb_route) && !(*stale == *evidence));
    for (const auto* line : {"1 34 299651072 7 17251 65537 0 0\n",
                             "1 34 299651072 7 17251 65537 0 9 1 2 3 4 5 6 7 8 9\n",
                             "1 34 299651072 7 17251 65537 0 2 4321 4321\n",
                             "1 34 299651072 7 17251 65537 0 2 4321 0\n",
                             "1 34 299651072 7 17251 65537 0 2 4321 4294967296\n",
                             "1 34 299651072 7 17251 65537 0 2 4321\n",
                             "1 34 299651072 7 17251 65537 0 2 4321 4322\nextra",
                             "1 34 299651072 7 17251 65536 0 2 4321 4322\n"})
        assert(!UsbStartEvidence::parse(line));
    using avcrt::agm::MicMuteReport;
    using avcrt::agm::MicPoll;
    using avcrt::agm::MicPrivacyReport;
    using avcrt::agm::MicStatus;
    {
        using Priming = avcrt::agm::UplinkPriming<4>;
        using namespace std::chrono_literals;
        const Priming::Clock::time_point start{};
        Priming p;
        assert(!p.no_progress(0, start + 100ms));
        p.ring_filled(start + 30ms);
        assert(!p.no_progress(0, start + 49ms));
        assert(p.no_progress(0, start + 50ms) && !p.no_progress(1, start + 50ms));
        for (unsigned i = 1; i <= 5; ++i) p.accepted(i, start + (25 + 10 * i) * 1ms, i == 1 ? 10 : 11);
        assert(!p.ready(0, start + 20ms, 11));
        assert(!p.ready(6, start + 20ms, 11));
        assert(p.ready(1, start + 34ms, 10)); // Still tracked with four more blocks queued.
        assert(!p.ready(1, start + 34ms, 11));
        assert(!p.ready(1, start + 35ms, 10));
        assert(p.ready(2, start + 44ms, 11));
        p.accepted(6, start + 85ms, 11);
        assert(!p.ready(1, start + 34ms, 10)); // Wrapped slot isn't the old block.
    }
    {
        using avcrt::agm::UplinkTiming;
        using namespace std::chrono_literals;
        UplinkTiming timing;
        const auto start = UplinkTiming::Clock::time_point{};
        assert(!timing.can_skip(start));
        timing.replaced(start);
        assert(!timing.can_skip(start + 120ms));
        assert(timing.can_skip(start + 119ms));
        for (unsigned i = 0; i < UplinkTiming::kMaxDrops; ++i) {
            assert(timing.can_skip(start + 40ms));
            timing.skipped();
        }
        assert(!timing.can_skip(start + 50ms));
        assert(!timing.grace_elapsed(start + 119ms));
        assert(timing.grace_elapsed(start + 120ms));
        timing.replaced(start + 55ms);
        assert(timing.can_skip(start + 90ms));
    }
    {
        MicMuteReport mute;
        assert(mute.status() == MicStatus::Unavailable);
        mute.line("mic mute FromSwitch=false FromRestrictions=false FromApi=false from system=false");
        assert(mute.status() == MicStatus::Clear);
        mute.line("mic mute FromSwitch=false FromRestrictions=false FromApi=true from system=false");
        assert(mute.status() == MicStatus::Blocked);
    }
    for (auto mic : {"state_type=1", "state_type=2", "state_type=3", ""}) {
        MicPrivacyReport privacy;
        for (auto row : {"SENSOR PRIVACY MANAGER STATE (dumpsys sensor_privacy)", "{", "user_id=0", "sensor=1",
                         "toggle_type=1", mic, "sensor=2", "state_type=1", "}"}) privacy.line(row);
        const auto state = std::string_view(mic);
        assert(privacy.status() == (state == "state_type=2" ? MicStatus::Clear :
                                    state == "state_type=1" ? MicStatus::Blocked : MicStatus::Unavailable));
        privacy.line("DUMP TIMEOUT");
        assert(privacy.status() != MicStatus::Clear);
    }
    assert(MicPoll::status(MicPoll::kBlocked, 1000) == MicStatus::Blocked);
    assert(MicPoll::status(MicPoll::kUnavailable, 1000) == MicStatus::Unavailable);
    assert(MicPoll::status(1000, 1000 + MicPoll::interval_ms + MicPoll::query_ms) == MicStatus::Clear);
    assert(MicPoll::status(1000, 1000 + MicPoll::max_age_ms) == MicStatus::Stale);

    assert(mute_readback(false, 0xC, 0, 0) == MuteReadback::Expected);
    assert(mute_readback(true, 0xC, 0xC, 0) == MuteReadback::Expected);
    assert(mute_readback(true, 0xC, 0, 0) == MuteReadback::Lost);
    assert(mute_readback(true, 0xC, 0x4, 0) == MuteReadback::Conflict);
    assert(mute_readback(true, 0xC, 0, 0x4) == MuteReadback::Conflict);
    assert(mute_readback(false, 0xC, 0xC, 0) == MuteReadback::Conflict);

    using avcrt::agm::empty_va_read;
    for (auto rc : {0, -ENOSPC, -EAGAIN, -ENODATA}) {
        assert(empty_va_read(rc, 0));
        assert(!empty_va_read(rc, 2));
    }
    assert(!empty_va_read(-EIO, 0));
    {
        std::array<int16_t, 4> pcm{};
        assert(!avcrt::agm::nonzero_pcm(pcm));
        pcm.back() = -1;
        assert(avcrt::agm::nonzero_pcm(pcm)); // even one least-significant bit is nonzero
    }
    {
        std::array<int16_t, 2> pcm{123, -456};
        avcrt::agm::UplinkSilence silence;
        assert(!silence.prepare(false, false, pcm));
        assert(silence.prepare(false, true, pcm) && pcm[0] == 123);
        for (unsigned i = 0; i < silence.kMaxPeriods; ++i) {
            pcm = {123, -456};
            assert(silence.prepare(true, false, pcm) && pcm[0] == 0 && pcm[1] == 0);
        }
        assert(!silence.prepare(true, false, pcm));
        assert(silence.prepare(true, true, pcm) && silence.prepare(true, false, pcm));
    }
    // Exercise the one-shot state used by the real ALSA lease wrapper.
    for (auto fault : {'P', 'W', 'H', 'U', '\0'}) {
        OneShotMute mute;
        std::string ops;
        auto prepare = [&] { ops += 'P'; return fault != 'P'; };
        auto write = [&] { ops += 'W'; return fault != 'W' && (fault != 'H' || ops == "PW"); };
        auto unmute = [&] { ops += '0'; return fault != 'U'; };
        const bool armed = fault != 'P' && fault != 'W';
        assert(mute.renew(prepare, write) == armed && mute.active() == armed);
        assert(mute.renew(prepare, write) == (armed && fault != 'H'));
        assert(mute.release(unmute) == (fault != 'U'));
        assert(!mute.active() && !mute.renew(prepare, write));
        assert(mute.release(unmute) == (fault != 'U'));
        assert(ops == (fault == 'P' ? "P" : fault == 'W' ? "PW0" : "PWW0"));
    }

    constexpr Capabilities none{};
    static_assert(select(true, none) == Backend::Legacy);
    static_assert(select(false, none) == Backend::None);

    Capabilities all{true, true, true, true, true, true};
    assert(select(true, all) == Backend::Legacy);  // legacy-first invariant
    assert(select(false, all) == Backend::CapabilityAdapter);
    for (unsigned missing = 0; missing != 6; ++missing) {
        Capabilities partial{
            missing != 0, missing != 1, missing != 2,
            missing != 3, missing != 4, missing != 5};
        assert(select(false, partial) == Backend::None);
    }

    {
        AgmAttempts attempts;
        assert(!attempts.may_discover(false, false) && !attempts.may_discover(true, true));
        assert(attempts.may_discover(true, false) && !attempts.begin({}, 1, 0));
        assert(attempts.begin("1:12", 1, 0) && attempts.current("1:12", 1));
        assert(!attempts.begin("1:12", 1, 1) && !attempts.current("2:12", 1));
        assert(!attempts.recover("2:12", 1, AgmRecovery::ColdUplink, 1));
        assert(attempts.recover("1:12", 1, AgmRecovery::ColdUplink, 100) == 400);
        assert(attempts.pending() && !attempts.begin("1:12", 1, 499));
        assert(!attempts.recover("1:12", 1, AgmRecovery::ColdUplink, 499));
        assert(attempts.begin("1:12", 1, 500) && !attempts.pending());
        assert(attempts.recover("1:12", 1, AgmRecovery::ColdUplink, 600) == 1200);
        assert(!attempts.begin("1:12", 1, 1799));
        assert(!attempts.begin("1:12", 2, 1799)); // A revision change is not another attempt while pending.
        assert(attempts.begin("1:12", 2, 1800) && attempts.current("1:12", 2));
        assert(!attempts.recover("1:12", 2, AgmRecovery::ColdUplink, 1900));
        // If the late worker starts successfully and later misses a PCM deadline,
        // it still has the live recovery budget; another live fault won't loop.
        assert(attempts.recover("1:12", 2, AgmRecovery::Transient, 4000) == 400);
        assert(!attempts.begin("1:12", 2, 4399) && attempts.begin("1:12", 2, 4400));
        assert(!attempts.recover("1:12", 2, AgmRecovery::Transient, 4500));
        assert(!attempts.begin("1:12", 2, 9000));
        assert(attempts.begin("2:12", 2, 9000)); // New call does not inherit the cooldown.
        assert(attempts.recover("2:12", 2, AgmRecovery::ColdUplink, 9100) == 400);
    }
    {
        AgmAttempts attempts;
        assert(attempts.begin("1:12", 1, 0));
        // A known reroute or explicit privacy/mute veto is not recoverable.
        assert(!attempts.recover("1:12", 1, AgmRecovery::None, 100));
        assert(!attempts.pending() && !attempts.begin("1:12", 1, 500));
        assert(attempts.recover("1:12", 1, AgmRecovery::ColdUplink, 600) == 400);
        attempts.cancel_recovery(); // No longer the same sole call, or legacy took precedence.
        assert(!attempts.pending() && !attempts.begin("1:12", 1, 1000));
        assert(attempts.begin("2:12", 1, 1000));
        assert(attempts.recover("2:12", 1, AgmRecovery::ColdUplink, 1100) == 400);
        attempts.disable();
        assert(!attempts.pending() && !attempts.same_call("2:12"));
        assert(attempts.begin("2:12", 1, 1200)); // Explicit enable starts a fresh sequence.
        assert(attempts.recover("2:12", 1, AgmRecovery::ColdUplink, 1300) == 400);
        attempts.quarantine();
        assert(!attempts.pending() && !attempts.begin("2:12", 1, 1700));
        attempts.disable();
        assert(!attempts.may_discover(true, false) && !attempts.begin("3:12", 3, 1800));
    }
    {
        AgmAttempts attempts;
        constexpr auto loss = AgmRecovery::LeaseLost;
        assert(attempts.begin("1:12", 4, 0));
        // Entering and leaving the dialer may each clear an established lease
        // without ending the call or touching the active preset.
        assert(attempts.recover("1:12", 4, loss, 1000) == 100);
        assert(!attempts.begin("1:12", 4, 1099));
        assert(attempts.begin("1:12", 4, 1100));
        assert(attempts.recover("1:12", 4, loss, 2000) == 200);
        assert(attempts.begin("1:12", 4, 2200));
        assert(attempts.recover("1:12", 4, loss, 3000) == 400);
        assert(attempts.begin("1:12", 4, 3400));
        assert(attempts.recover("1:12", 4, loss, 4000) == 600);
        assert(attempts.begin("1:12", 4, 4600));
        assert(!attempts.recover("1:12", 4, loss, 5999));
        assert(!attempts.pending() && !attempts.begin("1:12", 4, 5999));
        assert(attempts.recover("1:12", 4, loss, 6000) == 100);
        assert(attempts.begin("1:12", 4, 6100));
        assert(attempts.recover("1:12", 4, AgmRecovery::ColdUplink, 6200) == 400);
        assert(attempts.begin("1:12", 4, 6600));
        assert(attempts.recover("1:12", 4, AgmRecovery::ColdUplink, 6700) == 1200);
        assert(attempts.begin("1:12", 4, 7900));
        assert(!attempts.recover("1:12", 4, AgmRecovery::ColdUplink, 8000));
        assert(attempts.recover("1:12", 4, loss, 8100) == 200);
        assert(attempts.begin("1:12", 4, 8300));
        assert(attempts.recover("1:12", 4, AgmRecovery::ColdUplink, 8400) == 400);
        attempts.cancel_recovery(); // Call ended, or legacy took over before launch.
        assert(!attempts.begin("1:12", 4, 8800));
        assert(!attempts.recover("2:12", 4, loss, 8800));
        assert(attempts.begin("2:12", 4, 8800));
        assert(attempts.recover("2:12", 4, loss, 8900) == 100);
        attempts.quarantine(); // Cleanup failure must never let a retry through.
        assert(!attempts.begin("2:12", 4, 9000));
    }
    {
        // USB speaker/mute transitions can withdraw the DSP lease while the
        // last mic poll still says clear. Require a new poll and keep the preset.
        constexpr auto loss = AgmRecovery::LeaseLost;
        constexpr auto transient = AgmRecovery::Transient;
        assert(usb_cut_recovery(true, ESTALE, 2) == loss);
        assert(usb_cut_recovery(true, ESTALE, 1) == loss);
        assert(usb_cut_recovery(true, ETIME, 4) == loss);
        assert(usb_cut_recovery(true, ESTALE, 3) == AgmRecovery::None);
        assert(usb_cut_recovery(true, 0, 4) == AgmRecovery::None);
        assert(usb_cut_recovery(false, ESTALE, 2) == AgmRecovery::None);
        assert(usb_cut_recovery(false, ETIME, 0) == transient);
        assert(usb_cut_recovery(false, EBUSY, 0) == transient);

        AgmAttempts attempts;
        assert(attempts.begin("usb:1", 7, 0));
        assert(attempts.recover("usb:1", 7, loss, 1000, true) == 100);
        assert(attempts.pending() && attempts.waiting_for_mic("usb:1"));
        assert(!attempts.mic_check_due("usb:1", 1099));
        assert(!attempts.begin("usb:1", 7, 1099, MicStatus::Clear));
        assert(attempts.mic_check_due("usb:1", 1100));
        assert(!attempts.begin("usb:1", 7, 1100, MicStatus::Blocked));
        assert(!attempts.begin("usb:1", 8, 1100, MicStatus::Unavailable));
        assert(attempts.mic_check_due("usb:1", 1225));
        assert(attempts.begin("usb:1", 7, 1225, MicStatus::Clear));
        assert(!attempts.pending() && !attempts.waiting_for_mic("usb:1"));
        assert(attempts.recover("usb:1", 7, loss, 1500, true) == 200);
        assert(attempts.mic_check_due("usb:1", 1700));
        assert(attempts.begin("usb:1", 7, 1700, MicStatus::Clear)); // Same-route speaker switch.
        assert(attempts.recover("usb:1", 7, loss, 1800, true) == 400);
        attempts.quarantine();
        assert(!attempts.begin("usb:1", 7, 2200, MicStatus::Clear));
    }
    {
        AgmAttempts attempts;
        constexpr auto ready = MicStatus::Clear;
        assert(attempts.begin("1:12", 4, 0));
        assert(!attempts.wait_for_mic("2:12", 1));
        // Starts muted, or a live worker exits cleanly after mute/privacy.
        assert(attempts.wait_for_mic("1:12", 100));
        assert(!attempts.mic_check_due("2:12", 100) && attempts.mic_check_due("1:12", 100));
        assert(!attempts.mic_check_due("1:12", 224) && attempts.mic_check_due("1:12", 225));
        for (const auto status : {MicStatus::Blocked, MicStatus::Unavailable, MicStatus::Stale}) {
            assert(!attempts.begin("1:12", 4, 500, status));
            assert(!attempts.begin("1:12", 5, 500, status));
        }
        assert(attempts.begin("1:12", 4, 501, ready)); // Same preset, no app toggle.
        assert(!attempts.waiting_for_mic("1:12") && !attempts.begin("1:12", 4, 502, ready));
        assert(attempts.recover("1:12", 4, AgmRecovery::ColdUplink, 600) == 400);
        assert(attempts.begin("1:12", 4, 1000));
        assert(attempts.wait_for_mic("1:12", 1100));
        assert(attempts.begin("1:12", 5, 1500, ready) && attempts.current("1:12", 5));
        assert(attempts.recover("1:12", 5, AgmRecovery::ColdUplink, 1600) == 1200);
        assert(attempts.begin("1:12", 5, 2800));
        assert(attempts.wait_for_mic("1:12", 2900));
        attempts.cancel_recovery(); // Call ended, binding changed or legacy took priority.
        assert(!attempts.waiting_for_mic("1:12") && !attempts.begin("1:12", 5, 3300, ready));
        assert(attempts.begin("2:12", 5, 3300));
        assert(attempts.wait_for_mic("2:12", 3400));
        attempts.disable();
        assert(!attempts.waiting_for_mic("2:12") && !attempts.may_discover(false, false));
        assert(attempts.begin("2:12", 5, 3800));
        assert(attempts.wait_for_mic("2:12", 3900));
        attempts.quarantine();
        assert(!attempts.wait_for_mic("2:12", 4000) && !attempts.begin("2:12", 5, 4300, ready));
    }
    {
        AgmAttempts attempts;
        assert(attempts.begin("1:12", 4, 0));
        assert(attempts.wait_for_mic("1:12", 10) && attempts.mic_check_due("1:12", 10));
        assert(attempts.begin("1:12", 4, 20, MicStatus::Clear));
        // A new worker can disagree with the supervisor's observation. No
        // repeated tight worker loop, even when the preset changes in between.
        assert(attempts.wait_for_mic("1:12", 25));
        assert(!attempts.mic_check_due("1:12", 419));
        assert(!attempts.begin("1:12", 5, 419, MicStatus::Clear));
        assert(attempts.mic_check_due("1:12", 420));
        assert(attempts.begin("1:12", 5, 430, MicStatus::Clear));
    }

    check([](auto& s, auto& io, auto& l, auto& log) {
        io.capture = Block::Pending;
        assert(io.run(s, log) == Step::Priming && !l.muted);
        io.capture = Block::Full;
        io.dsp = Block::Pending;
        assert(io.run(s, log) == Step::Priming && !l.muted);
        io.dsp = Block::Full;
        assert(io.run(s, log) == Step::Replaced && l.muted);
        assert(io.run(s, log) == Step::Replaced && s.periods() == 2);
        assert(s.finish() && !l.muted);
        assert(s.finish());
    }, "RRDRDWMRDWM0tx");

    check([](auto& s, auto& io, auto&, auto& log) {
        io.capture = Block::Failed;
        assert(io.run(s, log) == Step::CaptureFailed);
    }, "Rtx");

    check([](auto& s, auto& io, auto&, auto& log) {
        io.write = Block::Failed;
        assert(io.run(s, log) == Step::UplinkFailed);
    }, "RDWtx");

    for (const unsigned completion : {0u, 3u}) {
        check([&](auto& s, auto&, auto& l, auto& log) {
            QueuedUplink q;
            q.fill(s, l, log);
            q.complete_at = completion;
            q.wait_budget = 3;  // only 3 ms of freshness left
            // No completion, or one arriving at the deadline: do not submit
            // a fifth period or request TX mute on the four accepted writes.
            assert(q.run(s, log) == Step::UplinkFailed && !l.muted);
            assert(q.waited == 3 && q.window.sent() == 4);
            assert(q.window.stalled_at_start(q.done) == (completion == 0));
        }, "WWWWtx");
    }

    check([](auto& s, auto&, auto& l, auto& log) {
        QueuedUplink q;
        q.fill(s, l, log);
        q.complete_at = 2;
        q.reroute_on_completion = true;
        assert(q.run(s, log) == Step::UplinkFailed && !l.muted);
        assert(q.waited == 2 && q.window.sent() == 4 && q.done == 1);
    }, "WWWWtx");

    for (const auto failure : {0, -ENOSPC, -EMSGSIZE}) {
        check([&](auto& s, auto&, auto& l, auto& log) {
            QueuedUplink q;
            q.fill(s, l, log);
            q.complete_at = 2;
            assert(q.run(s, log) == Step::Replaced && l.muted && s.periods() == 1);
            assert(q.waited == 2 && q.window.sent() == 5 && q.done == 1);
            if (failure) {
                ++q.done; // credit exists, but submission returns an error/short count
                if (failure == -ENOSPC) q.rc = -ENOSPC; // leaves byte_count unchanged
                else --q.reported;                      // rc=0, short write
            }
            assert(q.run(s, log) == Step::UplinkFailed && !l.muted);
            assert(q.waited == (failure ? 2u : 10u) && q.window.sent() == 5);
            assert(q.run(s, log) == Step::Closed); // no retry; unmute before teardown
        }, failure ? "WWWWWMW0tx" : "WWWWWM0tx");
    }

    for (auto fault : {'R', 'D', 'W', 'M'}) {
        for (const bool active : {false, true}) {
            auto expected = active ? std::string("RDWM") : std::string();
            expected += fault == 'R' ? "R" : fault == 'D' ? "RD" : fault == 'W' ? "RDW" : "RDWM";
            if (active || fault == 'M') expected += '0';
            expected += "tx";
            check([&](auto& s, auto& io, auto& l, auto& log) {
                if (active) assert(io.run(s, log) == Step::Replaced);
                io.invalidate_on = fault;
                assert(io.run(s, log) == Step::Invalidated && !l.muted);
            }, expected.c_str());
        }
    }

    for (auto fault : {'\0', 'R', 'D', 'W', 'M'}) {
        std::string expected = "RDWM";
        if (fault) expected += fault == 'R' ? "R" : fault == 'D' ? "RD" : fault == 'W' ? "RDW" : "RDWM";
        expected += "RDWM0tx";
        check([&](auto& s, auto& io, auto& l, auto& log) {
            assert(io.run(s, log) == Step::Replaced);
            io.skip_late = true;
            io.invalidate_on = fault;
            io.fresh = fault != '\0';
            assert(io.run(s, log) == Step::SkippedLate && l.muted && s.periods() == 1);
            io = {};
            assert(io.run(s, log) == Step::Replaced && l.muted && s.periods() == 2);
        }, expected.c_str());
    }
    check([](auto& s, auto& io, auto& l, auto& log) {
        io.skip_late = true;
        io.fresh = false;
        assert(io.run(s, log) == Step::Invalidated && !l.muted);
    }, "tx");
    check([](auto& s, auto& io, auto& l, auto& log) {
        assert(io.run(s, log) == Step::Replaced);
        io.skip_late = true;
        io.invalidate_on = 'R';
        io.capture = Block::Failed;
        assert(io.run(s, log) == Step::CaptureFailed && !l.muted);
    }, "RDWMR0tx");
    check([](auto& s, auto& io, auto& l, auto& log) {
        assert(io.run(s, log) == Step::Replaced);
        bool late = false;
        assert(s.step([&] { return late ? PeriodCheck::SkipLate : PeriodCheck::Ready; },
                      [] { return Block::Full; }, [] { return Block::Full; },
                      [&] { late = true; return Block::Late; }) == Step::SkippedLate);
        assert(l.muted && s.periods() == 1);
        assert(io.run(s, log) == Step::Replaced);
    }, "RDWMRDWM0tx");
    for (auto fault : {'C', 'R', 'E'}) {
        check([&](auto& s, auto& io, auto&, auto& log) {
            assert(io.run(s, log) == Step::Replaced);
            if (fault == 'C') io.call = 0;  // ended
            if (fault == 'R') io.route = 2; // changed with same call
            if (fault == 'E') io.enabled = false;
            io.fresh = false;
            io.skip_late = true; // Timing grace must not mask any invalid binding.
            assert(io.run(s, log) == Step::Invalidated);
            io = {};
            assert(io.run(s, log) == Step::Closed);
        }, "RDWM0tx");
    }
    check([](auto& s, auto& io, auto& l, auto& log) {
        assert(io.run(s, log) == Step::Replaced);
        io.dsp = Block::Pending;
        assert(io.run(s, log) == Step::DspFailed && !l.muted);
    }, "RDWMRD0tx");
    for (const bool active : {false, true}) {
        check([&](auto& s, auto& io, auto& l, auto& log) {
            if (active) assert(io.run(s, log) == Step::Replaced);
            l.error = active ? -ESTALE : -EIO;
            assert(io.run(s, log) == Step::LeaseFailed && !l.muted);
            l.error = 0;
            assert(io.run(s, log) == Step::Closed);  // no re-arm
        }, active ? "RDWMRDWM0tx" : "RDWM0tx");
    }
    check([](auto& s, auto& io, auto& l, auto& log) {
        assert(io.run(s, log) == Step::Replaced);
        l.release_ok = false;
        assert(!s.finish() && !s.finish());  // report unverified release
    }, "RDWM0tx");
    check([](auto& s, auto& io, auto&, auto& log) {
        assert(io.run(s, log) == Step::Replaced);
        // Implicit scope exit also releases before either stream closes.
    }, "RDWM0tx");
}
