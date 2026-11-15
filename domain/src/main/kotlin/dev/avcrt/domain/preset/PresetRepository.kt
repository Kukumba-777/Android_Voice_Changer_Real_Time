package dev.avcrt.domain.preset

import dev.avcrt.core.common.result.AvcrtResult
import kotlinx.coroutines.flow.Flow

interface PresetRepository {

    /** All presets — built-in + user custom, sorted with built-in first. */
    fun observeAll(): Flow<List<Preset>>

    /** ID of the currently active preset, or null when none is selected. */
    fun observeActive(): Flow<String?>

    suspend fun setActive(id: String?): AvcrtResult<Unit>

    suspend fun upsertCustom(preset: Preset): AvcrtResult<Unit>

    suspend fun delete(id: String): AvcrtResult<Unit>

    /** Returns the bundled defaults — used on first launch / reset. */
    suspend fun builtIns(): List<Preset>
}
