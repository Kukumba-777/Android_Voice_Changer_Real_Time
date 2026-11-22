#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace avcrt {

// ---- per-effect parameter blocks (Phase 6). Each has an `enabled` flag; when false the effect
//      is a no-op bypass (~0 CPU). Defaults = neutral, so a preset with no effects behaves exactly
//      like the pre-Phase-6 pitch/formant/gain engine. Shared verbatim by the daemon (JSON) and the
//      Zygisk hook (grouped CSV props) — both fill this same struct. ----
struct GateCfg      { bool enabled=false; double threshold_db=-45.0; double attack_ms=5.0;  double release_ms=80.0; };
struct EqCfg        { bool enabled=false; double low_db=0.0; double mid_db=0.0; double high_db=0.0; };
struct TelephoneCfg { bool enabled=false; };  // fixed 300–3400 Hz bandpass
struct RingModCfg   { bool enabled=false; double carrier_hz=120.0; double mix=1.0; };
struct TremoloCfg   { bool enabled=false; double rate_hz=5.0; double depth=0.6; int shape=0; };  // 0=sine,1=tri,2=square
struct DistortionCfg{ bool enabled=false; double drive=4.0; double mix=1.0; };
struct EchoCfg      { bool enabled=false; double time_ms=180.0; double feedback=0.35; double mix=0.30; };
struct ReverbCfg    { bool enabled=false; double room_size=0.5; double damping=0.5; double wet=0.3; double width=1.0; };
// ---- Tier-2 (member order MUST match the grouped-CSV field order in zygisk parse_fx_props) ----
struct FilterCfg    { bool enabled=false; double hp_hz=0.0; double lp_hz=0.0; double q=0.707; };  // hp/lp; 0 Hz = leg off
struct VibratoCfg   { bool enabled=false; double rate_hz=5.0; double depth=0.5; };
struct ChorusCfg    { bool enabled=false; double rate_hz=1.5; double depth=0.5; double mix=0.5; };
struct FlangerCfg   { bool enabled=false; double rate_hz=0.5; double depth=0.5; double feedback=0.5; double mix=0.5; };
struct PhaserCfg    { bool enabled=false; double rate_hz=0.5; double depth=0.7; double feedback=0.4; double mix=0.5; };
struct AutoWahCfg   { bool enabled=false; double sensitivity=0.5; double base_hz=300.0; double range_hz=2000.0; double mix=1.0; };
struct BitcrushCfg  { bool enabled=false; double bits=8.0; double downsample=4.0; double mix=1.0; };
struct CompressorCfg{ bool enabled=false; double threshold_db=-18.0; double ratio=3.0; double attack_ms=10.0; double release_ms=120.0; double makeup_db=0.0; };
// ---- Tier-3 (channel-vocoder family; member order MUST match the grouped-CSV field order) ----
struct FormantCfg   { bool enabled=false; double shift=1.0; };                                   // 0.5..2.0, 1=neutral
struct VocoderCfg   { bool enabled=false; double carrier_hz=110.0; int carrier_type=0; double mix=1.0; };  // 0=saw,1=pulse,2=noise
struct WhisperCfg   { bool enabled=false; double mix=1.0; };
// ---- Phase 7 EXPERIMENTAL (FFT spectral + phase-vocoder; adds latency — gated behind the toggle) ----
struct SpectralCfg  { bool robot=false; bool whisper=false; bool freeze=false; bool blur=false; double blur_amt=0.5; double mix=1.0; };  // mix = wet/dry strength of the spectral ops
struct AutotuneCfg  { bool enabled=false; double strength=1.0; };                              // 0..1 correction
struct HarmonizerCfg{ bool enabled=false; int interval1=4; int interval2=7; double mix=0.5; }; // semitones ±12

/**
 * Snapshot of the active preset's DSP parameters parsed from the JSON written
 * by the Kotlin app at [avcrt::paths::kActivePreset]. Plain-old-data so the
 * audio thread can copy it into local scope without lock contention.
 */
struct DspConfig {
    bool   enabled            = false;
    bool   bypass_voice       = false;
    double pitch_semitones    = 0.0;
    double formant_semitones  = 0.0;
    double time_ratio         = 1.0;
    double gain_db            = 0.0;
    int    crispness          = 5;
    bool   preserve_formants  = true;
    int64_t revision          = 0;

    // ---- Phase 6 effect chain (Tier-1) ----
    GateCfg       gate;
    EqCfg         eq;
    TelephoneCfg  telephone;
    RingModCfg    ringmod;
    TremoloCfg    tremolo;
    DistortionCfg distortion;
    EchoCfg       echo;
    ReverbCfg     reverb;

    // ---- Phase 6 effect chain (Tier-2) ----
    FilterCfg     filter;
    VibratoCfg    vibrato;
    ChorusCfg     chorus;
    FlangerCfg    flanger;
    PhaserCfg     phaser;
    AutoWahCfg    autowah;
    BitcrushCfg   bitcrush;
    CompressorCfg compressor;

    // ---- Phase 6 effect chain (Tier-3: channel-vocoder family) ----
    FormantCfg    formant;
    VocoderCfg    vocoder;
    WhisperCfg    whisper;

    // ---- Phase 7 EXPERIMENTAL (FFT spectral + phase-vocoder) ----
    SpectralCfg   spectral;
    AutotuneCfg   autotune;
    HarmonizerCfg harmonizer;
};

/**
 * Owns the current [DspConfig] and reloads it when the
 * `avcrt.config.revision` system property changes.
 *
 * The audio thread reads via [snapshot()] which performs a single atomic
 * compare-and-swap if a newer revision is available. Polling is cheap — a
 * single system property read every process() call.
 */
class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore();

    /** Atomically returns the latest known config, reloading from disk if the
     *  property has been bumped since the last call. */
    DspConfig snapshot();

    /** Forces a re-parse from disk regardless of revision. Used at effect
     *  init so we don't start with stale defaults. */
    void reload();

private:
    DspConfig parse_file();

    std::mutex            mu_;
    DspConfig             current_;
    std::atomic<int64_t>  last_seen_revision_{-1};
    int64_t               last_prop_revision_{-1};
};

}  // namespace avcrt
