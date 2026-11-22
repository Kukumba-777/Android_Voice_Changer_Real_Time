#pragma once

#include <string_view>

namespace avcrt::capture_probe {

inline constexpr std::string_view kPerTrackV1 = "AVCRT_CAPTURE_CONTRACT=per_track_pcm_v1\n";

// A successful reply advertises the source-ROM integration, not ownership of
// any current buffer. This probe never authorizes loading a legacy payload.
constexpr bool source_contract_observed(std::string_view reply, bool dumpOk,
                                        bool sameAudioserver) {
    return dumpOk && sameAudioserver && reply == kPerTrackV1;
}

constexpr const char* audioflinger_advice(bool observed, bool optedIn, bool legacyMapped) {
    if (legacyMapped) return "legacy_mapping_not_a_per_track_backend";
    if (!observed) return "no_verified_per_track_contract";
    return optedIn ? "separate_source_rom_backend" : "source_rom_opt_in_off";
}

} // namespace avcrt::capture_probe
