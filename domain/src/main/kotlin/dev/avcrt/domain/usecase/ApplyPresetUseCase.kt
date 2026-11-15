package dev.avcrt.domain.usecase

import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.settings.SettingsRepository
import kotlinx.coroutines.flow.first
import javax.inject.Inject

/**
 * Activates a preset both on disk (so the home tab can read it back) and on
 * the running engine (so it actually starts modifying audio if the engine is
 * already running). Order matters — settings first so a crash in [engine]
 * doesn't leave us with the wrong saved preset.
 */
class ApplyPresetUseCase @Inject constructor(
    private val engine: VoiceEnginePort,
    private val presets: PresetRepository,
    private val settings: SettingsRepository,
) {
    suspend operator fun invoke(presetId: String): AvcrtResult<Unit> {
        val preset = presets.observeAll().first()
            .firstOrNull { it.id == presetId }
            ?: return AvcrtResult.Failure(
                AvcrtError.Unknown("Preset not found: $presetId"),
            )
        settings.setActivePresetId(preset.id)
        presets.setActive(preset.id)
        return engine.applyPreset(preset)
    }
}
