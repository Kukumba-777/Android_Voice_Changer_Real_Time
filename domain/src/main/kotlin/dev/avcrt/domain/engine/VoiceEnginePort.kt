package dev.avcrt.domain.engine

import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.domain.preset.Preset
import kotlinx.coroutines.flow.Flow

/**
 * The domain-side port for the engine. The :engine:hal module supplies the
 * actual implementation. ViewModels depend on this — not on the HAL class —
 * so a future swap (Magisk module, Xposed hook, in-app preview) doesn't ripple
 * through the UI.
 */
interface VoiceEnginePort {

    fun observeState(): Flow<EngineState>

    suspend fun refreshState(): AvcrtResult<EngineState>

    suspend fun start(): AvcrtResult<Unit>

    suspend fun stop(): AvcrtResult<Unit>

    suspend fun applyPreset(preset: Preset): AvcrtResult<Unit>

    /**
     * Applies an UNSAVED preset's DSP to the live runtime so the user can hear it while editing,
     * WITHOUT changing the persisted active preset / master switch. [stopPreview] restores the real
     * active config.
     */
    suspend fun previewPreset(preset: Preset): AvcrtResult<Unit>

    /**
     * Restores the persisted active preset's config after a [previewPreset]. Fire-and-forget on the
     * engine's own scope so it survives the editor screen closing. No-op if not currently previewing.
     */
    fun stopPreview()

    suspend fun installModule(withoutZygisk: Boolean = false): AvcrtResult<Unit>

    suspend fun uninstallModule(): AvcrtResult<Unit>

    suspend fun reboot(): AvcrtResult<Unit>
}
