#pragma once
//
// On-device paths shared between the Kotlin app and this native effect.
// MUST stay in lockstep with dev/avcrt/core/common/AvcrtPaths.kt — when one
// side changes a constant, change the other side or the engine cannot reload.
//
namespace avcrt::paths {
constexpr const char* kModuleDataDir   = "/data/local/tmp/avcrt";
constexpr const char* kActivePreset    = "/data/local/tmp/avcrt/active_preset.json";
constexpr const char* kEngineState     = "/data/local/tmp/avcrt/engine_state";
constexpr const char* kReloadProperty  = "avcrt.config.revision";
constexpr const char* kLogTag          = "avcrtfx";
}  // namespace avcrt::paths
