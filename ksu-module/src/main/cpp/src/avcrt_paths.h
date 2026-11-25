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
constexpr const char* kBypassVoiceProperty = "avcrt.dsp.bypass_voice";
#if !defined(__ANDROID__) && defined(AVCRT_HOST_LOCAL_SOUNDBOARD)
// The host integration test runs in its own temporary working directory.
constexpr const char* kSoundboardClip = "clip.pcm";
constexpr const char* kSoundboardPlayback = "playback";
#else
constexpr const char* kSoundboardClip = "/data/avcrt_sound/clip.pcm";
constexpr const char* kSoundboardPlayback = "/data/avcrt_sound/playback";
#endif
constexpr const char* kLogTag          = "avcrtfx";
}  // namespace avcrt::paths
