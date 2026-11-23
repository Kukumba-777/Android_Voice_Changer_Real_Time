package dev.avcrt.domain.media

import dev.avcrt.core.common.result.AvcrtResult
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow
import java.io.InputStream

enum class MediaFolder { MODELS, SOUNDS }

/** A file managed by the in-app media library. */
data class MediaFile(
    val id: String,
    val absolutePath: String,
    val sizeBytes: Long,
    val modifiedAtMillis: Long,
) {
    val displayName: String get() = id.substringBeforeLast('.')
}

interface MediaRepository {
    fun observe(folder: MediaFolder): Flow<List<MediaFile>>
    suspend fun refresh(folder: MediaFolder)
    /** The source is opened on IO and closed by the repository. */
    suspend fun import(folder: MediaFolder, name: String, open: () -> InputStream): AvcrtResult<Unit>
    suspend fun delete(folder: MediaFolder, id: String): AvcrtResult<Unit>
}

sealed interface SoundboardState {
    data object Idle : SoundboardState
    data class Preparing(val id: String) : SoundboardState
    data class Playing(val id: String) : SoundboardState
}

interface Soundboard {
    val state: StateFlow<SoundboardState>
    suspend fun play(id: String, clipVolumePercent: Int = 100): AvcrtResult<Unit>
    suspend fun stop(): AvcrtResult<Unit>
}
