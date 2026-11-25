#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace avcrt::agm {

enum class MicStatus { Clear, Blocked, Unavailable, Stale };

inline const char* mic_status_name(MicStatus status) {
    switch (status) {
    case MicStatus::Clear: return "clear";
    case MicStatus::Blocked: return "muted or privacy blocked";
    case MicStatus::Unavailable: return "unavailable (failed, timed out or unrecognized report)";
    case MicStatus::Stale: return "stale";
    }
    return "unknown";
}

// A slow query must not slide the start of the next poll or extend the
// lifetime of the last positive observation. Unknown and blocked both veto VA.
struct MicPoll {
    static constexpr int64_t kUnavailable = -1, kBlocked = -2;
    static constexpr int64_t interval_ms = 125, query_ms = 250, max_age_ms = 400;
    static_assert(interval_ms + query_ms < max_age_ms);
    static MicStatus status(int64_t sample, int64_t now) {
        if (sample == kBlocked) return MicStatus::Blocked;
        if (sample < 0) return MicStatus::Unavailable;
        return now - sample < max_age_ms ? MicStatus::Clear : MicStatus::Stale;
    }
};

// Read-only vetoes for the observed Android dump formats. The raw VA PCM
// does not inherit Android mute; absent or unfamiliar state is not permission.
struct MicMuteReport {
    void line(std::string_view text) {
        if (text.substr(0, 9) != "mic mute ") return;
        ++rows;
        clear &= text == "mic mute FromSwitch=false FromRestrictions=false FromApi=false from system=false";
        for (auto flag : {" FromSwitch=true", " FromRestrictions=true", " FromApi=true", " from system=true"})
            blocked |= text.find(flag) != text.npos;
    }
    MicStatus status() const {
        if (blocked) return MicStatus::Blocked;
        return rows == 1 && clear ? MicStatus::Clear : MicStatus::Unavailable;
    }
private:
    unsigned rows = 0;
    bool clear = true, blocked = false;
};

struct MicPrivacyReport {
    void line(std::string_view text) {
        if (text.empty()) return;
        last_closed = text == "}";
        if (text == "SENSOR PRIVACY MANAGER STATE (dumpsys sensor_privacy)") ++headers;
        if (text.substr(0, 8) == "user_id=" || text.substr(0, 7) == "sensor=") {
            valid &= !mic || states != 0;
            mic = text == "sensor=1";
            states = 0;
            if (mic) ++microphones;
        } else if (mic && text.substr(0, 11) == "state_type=") {
            // AOSP SensorPrivacyIndividualEnabledSensorProto.DISABLED.
            ++states;
            valid &= text == "state_type=2";
            blocked |= text == "state_type=1";
        }
    }
    // Be conservative with multiple Android users: veto a blocked/unknown
    // microphone in any reported user rather than guessing the current user.
    MicStatus status() const {
        if (blocked) return MicStatus::Blocked;
        return valid && headers == 1 && microphones && last_closed && (!mic || states)
            ? MicStatus::Clear : MicStatus::Unavailable;
    }
private:
    unsigned headers = 0, microphones = 0, states = 0;
    bool valid = true, mic = false, last_closed = false, blocked = false;
};

}  // namespace avcrt::agm
