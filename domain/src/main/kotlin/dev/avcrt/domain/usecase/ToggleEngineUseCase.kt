package dev.avcrt.domain.usecase

import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.settings.SettingsRepository
import javax.inject.Inject

/**
 * Toggles the engine on/off and persists the user's intent so we resume the
 * right state on next launch.
 */
class ToggleEngineUseCase @Inject constructor(
    private val engine: VoiceEnginePort,
    private val settings: SettingsRepository,
) {
    suspend operator fun invoke(enable: Boolean): AvcrtResult<Unit> {
        settings.setEngineEnabled(enable)
        return if (enable) engine.start() else engine.stop()
    }
}
