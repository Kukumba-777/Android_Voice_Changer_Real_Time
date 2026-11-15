package dev.avcrt.domain.preset

import kotlinx.serialization.Serializable

/**
 * DSP parameters the native engine actually applies (avcrt::Engine, shared by the cellular daemon
 * and the Zygisk app hook). Core voice params + the Phase-6 [PresetEffects] chain.
 *
 * Keep DSP-engine-agnostic. The native side reads the SAME JSON (daemon, via nlohmann) AND a set of
 * grouped CSV props (zygisk) — both fill the C++ `DspConfig`. Ranges in [PresetDspRange] / per-effect
 * ranges in [EffectRange]; the editor clamps to these.
 *
 * `fx` defaults to all-disabled, so a preset with no effects behaves exactly like the pre-Phase-6
 * pitch/formant/gain engine, and OLD custom presets (serialized without `fx`) deserialize fine.
 */
@Serializable
data class PresetDsp(
    val pitchSemitones: Double,
    val formantSemitones: Double,
    val timeRatio: Double = 1.0,
    val gainDb: Double = 0.0,
    val crispness: Int = 5,
    val preserveFormants: Boolean = true,
    val fx: PresetEffects = PresetEffects(),
)

// ---- Phase 6 effect chain (Tier-1). Each block has an `enabled` flag (default false = bypass). ----
@Serializable
data class PresetEffects(
    val gate: GateFx = GateFx(),
    val eq: EqFx = EqFx(),
    val telephone: TelephoneFx = TelephoneFx(),
    val ringmod: RingModFx = RingModFx(),
    val tremolo: TremoloFx = TremoloFx(),
    val distortion: DistortionFx = DistortionFx(),
    val echo: EchoFx = EchoFx(),
    val reverb: ReverbFx = ReverbFx(),
    // ---- Tier-2 ----
    val filter: FilterFx = FilterFx(),
    val vibrato: VibratoFx = VibratoFx(),
    val chorus: ChorusFx = ChorusFx(),
    val flanger: FlangerFx = FlangerFx(),
    val phaser: PhaserFx = PhaserFx(),
    val autowah: AutoWahFx = AutoWahFx(),
    val bitcrush: BitcrushFx = BitcrushFx(),
    val compressor: CompressorFx = CompressorFx(),
    // ---- Tier-3 (channel-vocoder family) ----
    val formant: FormantFx = FormantFx(),
    val vocoder: VocoderFx = VocoderFx(),
    val whisper: WhisperFx = WhisperFx(),
    // ---- Phase 7 experimental (FFT spectral + phase-vocoder; behind the experimental toggle) ----
    val spectral: SpectralFx = SpectralFx(),
    val autotune: AutotuneFx = AutotuneFx(),
    val harmonizer: HarmonizerFx = HarmonizerFx(),
)

@Serializable
data class GateFx(
    val enabled: Boolean = false,
    val thresholdDb: Double = -45.0,
    val attackMs: Double = 5.0,
    val releaseMs: Double = 80.0,
)

@Serializable
data class EqFx(
    val enabled: Boolean = false,
    val lowDb: Double = 0.0,
    val midDb: Double = 0.0,
    val highDb: Double = 0.0,
)

@Serializable
data class TelephoneFx(val enabled: Boolean = false)

@Serializable
data class RingModFx(
    val enabled: Boolean = false,
    val carrierHz: Double = 120.0,
    val mix: Double = 1.0,
)

@Serializable
data class TremoloFx(
    val enabled: Boolean = false,
    val rateHz: Double = 5.0,
    val depth: Double = 0.6,
    /** 0=sine, 1=triangle, 2=square. */
    val shape: Int = 0,
)

@Serializable
data class DistortionFx(
    val enabled: Boolean = false,
    val drive: Double = 4.0,
    val mix: Double = 1.0,
)

@Serializable
data class EchoFx(
    val enabled: Boolean = false,
    val timeMs: Double = 180.0,
    val feedback: Double = 0.35,
    val mix: Double = 0.30,
)

@Serializable
data class ReverbFx(
    val enabled: Boolean = false,
    val roomSize: Double = 0.5,
    val damping: Double = 0.5,
    val wet: Double = 0.3,
    val width: Double = 1.0,
)

// ---- Tier-2. Field names == the C++ daemon JSON keys (dsp.fx.<name>.<field>); defaults match the
//      native struct defaults so old presets (serialized without these) deserialize identically. ----

/** Sweepable high-pass + low-pass. A cutoff of 0 Hz disables that leg (full-range passthrough). */
@Serializable
data class FilterFx(
    val enabled: Boolean = false,
    val hpHz: Double = 0.0,
    val lpHz: Double = 0.0,
    val q: Double = 0.707,
)

@Serializable
data class VibratoFx(
    val enabled: Boolean = false,
    val rateHz: Double = 5.0,
    val depth: Double = 0.5,
)

@Serializable
data class ChorusFx(
    val enabled: Boolean = false,
    val rateHz: Double = 1.5,
    val depth: Double = 0.5,
    val mix: Double = 0.5,
)

@Serializable
data class FlangerFx(
    val enabled: Boolean = false,
    val rateHz: Double = 0.5,
    val depth: Double = 0.5,
    val feedback: Double = 0.5,
    val mix: Double = 0.5,
)

@Serializable
data class PhaserFx(
    val enabled: Boolean = false,
    val rateHz: Double = 0.5,
    val depth: Double = 0.7,
    val feedback: Double = 0.4,
    val mix: Double = 0.5,
)

@Serializable
data class AutoWahFx(
    val enabled: Boolean = false,
    val sensitivity: Double = 0.5,
    val baseHz: Double = 300.0,
    val rangeHz: Double = 2000.0,
    val mix: Double = 1.0,
)

@Serializable
data class BitcrushFx(
    val enabled: Boolean = false,
    val bits: Double = 8.0,
    val downsample: Double = 4.0,
    val mix: Double = 1.0,
)

@Serializable
data class CompressorFx(
    val enabled: Boolean = false,
    val thresholdDb: Double = -18.0,
    val ratio: Double = 3.0,
    val attackMs: Double = 10.0,
    val releaseMs: Double = 120.0,
    val makeupDb: Double = 0.0,
)

// ---- Tier-3: channel-vocoder family (true formant shift / vocoder / whisper). Field names == the
//      C++ daemon JSON keys; all defaulted so presets without these deserialize unchanged. ----

/** Pitch-INDEPENDENT formant shift. 1.0 = neutral, >1 brighter/smaller head, <1 darker/larger. */
@Serializable
data class FormantFx(
    val enabled: Boolean = false,
    val shift: Double = 1.0,
)

@Serializable
data class VocoderFx(
    val enabled: Boolean = false,
    val carrierHz: Double = 110.0,
    /** 0=saw, 1=pulse, 2=noise. */
    val carrierType: Int = 0,
    val mix: Double = 1.0,
)

@Serializable
data class WhisperFx(
    val enabled: Boolean = false,
    val mix: Double = 1.0,
)

/**
 * Phase 7 EXPERIMENTAL: FFT spectral effects (each adds ~21 ms latency via a shared STFT). Field
 * names == the C++ daemon JSON keys (dsp.fx.spectral.*). All default off → no latency, no regression.
 */
@Serializable
data class SpectralFx(
    val robot: Boolean = false,
    val whisper: Boolean = false,
    val freeze: Boolean = false,
    val blur: Boolean = false,
    val blurAmt: Double = 0.5,
    /** Wet/dry strength of the spectral ops (1 = full effect). */
    val mix: Double = 1.0,
)

/** Phase 7 EXPERIMENTAL: phase-vocoder auto-tune (snap to nearest chromatic note). */
@Serializable
data class AutotuneFx(
    val enabled: Boolean = false,
    val strength: Double = 1.0,
)

/** Phase 7 EXPERIMENTAL: phase-vocoder harmonizer (adds 2 pitch-shifted voices at semitone intervals). */
@Serializable
data class HarmonizerFx(
    val enabled: Boolean = false,
    val interval1: Int = 4,
    val interval2: Int = 7,
    val mix: Double = 0.5,
)

object PresetDspRange {
    const val PITCH_MIN = -24.0
    const val PITCH_MAX = 24.0
    const val FORMANT_MIN = -12.0
    const val FORMANT_MAX = 12.0
    const val TIME_RATIO_MIN = 0.5
    const val TIME_RATIO_MAX = 2.0
    const val GAIN_DB_MIN = -24.0
    const val GAIN_DB_MAX = 24.0
    const val CRISPNESS_MIN = 0
    const val CRISPNESS_MAX = 6
}

/** Slider bounds for the effect params (the editor clamps to these; the native side is tolerant). */
object EffectRange {
    // gate
    const val GATE_THRESH_MIN = -80.0; const val GATE_THRESH_MAX = 0.0
    const val GATE_ATTACK_MIN = 0.1;   const val GATE_ATTACK_MAX = 50.0
    const val GATE_RELEASE_MIN = 1.0;  const val GATE_RELEASE_MAX = 500.0
    // eq
    const val EQ_DB_MIN = -15.0; const val EQ_DB_MAX = 15.0
    // ring-mod
    const val RING_CARRIER_MIN = 20.0; const val RING_CARRIER_MAX = 2000.0
    // tremolo
    const val TREM_RATE_MIN = 0.1; const val TREM_RATE_MAX = 20.0
    // distortion
    const val DIST_DRIVE_MIN = 1.0; const val DIST_DRIVE_MAX = 50.0
    // echo
    const val ECHO_TIME_MIN = 10.0;  const val ECHO_TIME_MAX = 1000.0
    const val ECHO_FB_MIN = 0.0;     const val ECHO_FB_MAX = 0.95
    // mix/amount 0..1 (depth, mix, wet, room, damping, width, sensitivity)
    const val UNIT_MIN = 0.0; const val UNIT_MAX = 1.0
    // ---- Tier-2 ----
    // filter (0 Hz = leg off)
    const val FILTER_HP_MIN = 0.0; const val FILTER_HP_MAX = 2000.0
    const val FILTER_LP_MIN = 0.0; const val FILTER_LP_MAX = 12000.0
    const val FILTER_Q_MIN = 0.5;  const val FILTER_Q_MAX = 8.0
    // vibrato / chorus / flanger / phaser rates
    const val VIB_RATE_MIN = 0.1;  const val VIB_RATE_MAX = 12.0
    const val CHOR_RATE_MIN = 0.1; const val CHOR_RATE_MAX = 8.0
    const val FLAN_RATE_MIN = 0.1; const val FLAN_RATE_MAX = 5.0
    const val PHAS_RATE_MIN = 0.1; const val PHAS_RATE_MAX = 5.0
    // feedbacks
    const val FLAN_FB_MIN = 0.0; const val FLAN_FB_MAX = 0.95
    const val PHAS_FB_MIN = 0.0; const val PHAS_FB_MAX = 0.9
    // auto-wah sweep
    const val WAH_BASE_MIN = 100.0;  const val WAH_BASE_MAX = 1500.0
    const val WAH_RANGE_MIN = 200.0; const val WAH_RANGE_MAX = 4000.0
    // bitcrush (min 2 bits — 1 bit quantises quiet signals to silence)
    const val CRUSH_BITS_MIN = 2.0; const val CRUSH_BITS_MAX = 16.0
    const val CRUSH_DS_MIN = 1.0;   const val CRUSH_DS_MAX = 50.0
    // compressor
    const val COMP_THRESH_MIN = -60.0; const val COMP_THRESH_MAX = 0.0
    const val COMP_RATIO_MIN = 1.0;    const val COMP_RATIO_MAX = 20.0
    const val COMP_ATTACK_MIN = 0.1;   const val COMP_ATTACK_MAX = 100.0
    const val COMP_RELEASE_MIN = 10.0; const val COMP_RELEASE_MAX = 1000.0
    const val COMP_MAKEUP_MIN = 0.0;   const val COMP_MAKEUP_MAX = 24.0
    // ---- Tier-3 ----
    // formant shift factor (1.0 = neutral)
    const val FORMANT_SHIFT_MIN = 0.5; const val FORMANT_SHIFT_MAX = 2.0
    // vocoder carrier frequency
    const val VOCODER_CARRIER_MIN = 50.0; const val VOCODER_CARRIER_MAX = 400.0
    // harmonizer voice intervals (semitones)
    const val HARMON_INTERVAL_MIN = -12.0; const val HARMON_INTERVAL_MAX = 12.0
}

object PresetDspValidator {
    fun isValid(dsp: PresetDsp): Boolean =
        dsp.pitchSemitones in PresetDspRange.PITCH_MIN..PresetDspRange.PITCH_MAX &&
            dsp.formantSemitones in PresetDspRange.FORMANT_MIN..PresetDspRange.FORMANT_MAX &&
            dsp.timeRatio in PresetDspRange.TIME_RATIO_MIN..PresetDspRange.TIME_RATIO_MAX &&
            dsp.gainDb in PresetDspRange.GAIN_DB_MIN..PresetDspRange.GAIN_DB_MAX &&
            dsp.crispness in PresetDspRange.CRISPNESS_MIN..PresetDspRange.CRISPNESS_MAX
}
