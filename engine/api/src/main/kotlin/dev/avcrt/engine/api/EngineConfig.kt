package dev.avcrt.engine.api

import dev.avcrt.domain.preset.PresetDsp
import dev.avcrt.domain.preset.PresetEffects
import kotlinx.serialization.Serializable

/**
 * Wire format the Kotlin side writes to [dev.avcrt.core.common.AvcrtPaths.ACTIVE_PRESET_FILE].
 * The native effect reads the SAME JSON; the C++ parser in `engine_config.h` matches this struct
 * field-for-field. When you add a field here, update the C++ side too.
 */
@Serializable
data class EngineConfigDto(
    val schema: Int = SCHEMA_VERSION,
    val enabled: Boolean,
    val presetId: String?,
    val dsp: EngineDspDto?,
    val revision: Long,
) {
    companion object {
        const val SCHEMA_VERSION = 1
    }
}

@Serializable
data class EngineDspDto(
    val pitchSemitones: Double,
    val formantSemitones: Double,
    val timeRatio: Double,
    val gainDb: Double,
    val crispness: Int,
    val preserveFormants: Boolean,
    // Phase-6 effect chain. PresetEffects' field names == the JSON keys the C++ daemon parses
    // (dsp.fx.gate.thresholdDb, …) so it serializes directly; reused, no duplicate DTO.
    val fx: PresetEffects = PresetEffects(),
) {
    companion object {
        fun from(dsp: PresetDsp): EngineDspDto = EngineDspDto(
            pitchSemitones = dsp.pitchSemitones,
            formantSemitones = dsp.formantSemitones,
            timeRatio = dsp.timeRatio,
            gainDb = dsp.gainDb,
            crispness = dsp.crispness,
            preserveFormants = dsp.preserveFormants,
            fx = dsp.fx,
        )
    }
}
