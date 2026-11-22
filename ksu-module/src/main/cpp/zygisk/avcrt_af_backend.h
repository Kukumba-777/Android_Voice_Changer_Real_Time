#pragma once

#include <string_view>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace avcrt::af_backend {

// Use Android's audio_prop context rather than giving audioserver write access
// to default_prop (all unlabelled system properties).
inline constexpr char kProperty[] = "sys.audio.avcrt.afhook.active";
inline constexpr char kServerPid[] = "init.svc_debug_pid.audioserver";

// A missing claim, the old probing sentinel, and a claim from a previous
// audioserver must all leave app DSP enabled. Use init's service PID rather
// than relying on app access to another domain's /proc entries.
constexpr bool claim_matches(std::string_view claim, std::string_view server_pid) {
    if (claim.empty() || claim == "1" || claim == "2" || claim.front() == '0' ||
        claim != server_pid) return false;
    return claim.find_first_not_of("0123456789") == std::string_view::npos;
}

#ifdef __ANDROID__
inline bool claimed() {
    char claim[PROP_VALUE_MAX]{}, server_pid[PROP_VALUE_MAX]{};
    if (__system_property_get(kProperty, claim) <= 0 || claim[0] == '0') return false;
    __system_property_get(kServerPid, server_pid);
    return claim_matches(claim, server_pid);
}
#endif

} // namespace avcrt::af_backend
