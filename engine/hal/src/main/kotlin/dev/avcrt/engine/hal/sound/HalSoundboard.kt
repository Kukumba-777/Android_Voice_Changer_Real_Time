package dev.avcrt.engine.hal.sound

import android.content.Context
import android.os.SystemClock
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.R
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.UserMessage
import dev.avcrt.core.root.RootAccess
import dev.avcrt.domain.engine.EngineState
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.media.Soundboard
import dev.avcrt.domain.media.SoundboardState
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import timber.log.Timber
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.inject.Inject
import javax.inject.Singleton

/** Publishes a single, expiring clip. Never plays through the device speaker. */
@Singleton
class HalSoundboard @Inject constructor(
    @ApplicationContext private val context: Context,
    private val engine: VoiceEnginePort,
    private val cache: SoundboardCache,
    private val root: RootAccess,
    private val dispatchers: DispatcherProvider,
) : Soundboard {
    private val scope = CoroutineScope(SupervisorJob() + dispatchers.io)
    private val mutex = Mutex()
    private val playback = MutableStateFlow<SoundboardState>(SoundboardState.Idle)
    override val state = playback.asStateFlow()
    private var request = 0L
    private var preparation: Job? = null
    private var expiration: Job? = null
    private var published = false

    init {
        scope.launch {
            engine.observeState().collect {
                if (it !is EngineState.Running && playback.value != SoundboardState.Idle) stop()
            }
        }
    }

    override suspend fun play(id: String, clipVolumePercent: Int): AvcrtResult<Unit> = coroutineScope {
        val token = mutex.withLock {
            if (!isEngineRunning()) return@coroutineScope failure(R.string.error_sound_engine_off)
            if (published) {
                val result = clearClip()
                if (result is AvcrtResult.Failure) return@coroutineScope result
            }
            expiration?.cancel()
            preparation?.cancel()
            preparation = coroutineContext[Job]
            playback.value = SoundboardState.Preparing(id)
            ++request
        }
        try {
            withContext(dispatchers.io) {
                if (!cache.moduleReady()) return@withContext failure(R.string.error_sound_module)
                val sound = cache.findSound(id)
                    ?: return@withContext failure(R.string.error_sound_decode)
                val prepared = cache.prepare(sound)
                val staging = storageIo { File.createTempFile("avcrt-play-", ".bin", context.cacheDir) }
                try {
                    mutex.withLock {
                        if (request != token) return@withLock AvcrtResult.Success(Unit)
                        if (!isEngineRunning()) return@withLock failure(R.string.error_sound_engine_off)
                        ensureActive()
                        val startMs = SystemClock.elapsedRealtime() + START_LEAD_MS
                        storageIo {
                            staging.writeBytes(ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.LITTLE_ENDIAN)
                                .putLong(CLIP_MAGIC).putLong(startMs)
                                .putLong(clipVolumePercent.coerceIn(0, 100).toLong()).putLong(prepared.inode).array())
                        }
                        // Complete publication even if the calling overlay is being torn down.
                        withContext(NonCancellable) publishClip@{
                            val result = publish(staging, prepared)
                            if (result is AvcrtResult.Failure) return@publishClip storageFailure(result)
                            published = true
                            playback.value = SoundboardState.Playing(id)
                            val framesPerMs = SoundDecoder.SAMPLE_RATE / 1000
                            val endMs = startMs + (prepared.frames + framesPerMs - 1) / framesPerMs
                            expiration = scope.launch {
                                delay((endMs - SystemClock.elapsedRealtime()).coerceAtLeast(0))
                                mutex.withLock { if (request == token) clearClip() }
                            }
                            AvcrtResult.Success(Unit)
                        }
                    }
                } finally {
                    staging.delete()
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: SoundStorageError) {
            failure(R.string.error_sound_storage, e)
        } catch (e: Exception) {
            Timber.w(e, "Soundboard could not prepare clip")
            failure(R.string.error_sound_decode, e)
        } finally {
            withContext(NonCancellable) {
                mutex.withLock {
                    if (request == token) {
                        preparation = null
                        if (playback.value is SoundboardState.Preparing) playback.value = SoundboardState.Idle
                    }
                }
            }
        }
    }

    override suspend fun stop(): AvcrtResult<Unit> = mutex.withLock {
        val result = clearClip()
        if (result is AvcrtResult.Failure) return@withLock result
        ++request
        preparation?.cancel()
        preparation = null
        expiration?.cancel()
        result
    }

    private suspend fun clearClip(): AvcrtResult<Unit> = withContext(NonCancellable) {
        val result = if (published) root.exec("rm -f '${AvcrtPaths.SOUNDBOARD_PLAYBACK}' '${AvcrtPaths.SOUNDBOARD_CLIP}'")
        else AvcrtResult.Success("")
        if (result is AvcrtResult.Failure) return@withContext result
        published = false
        playback.value = SoundboardState.Idle
        AvcrtResult.Success(Unit)
    }

    private suspend fun isEngineRunning() = engine.observeState().first() is EngineState.Running

    private suspend fun publish(staging: File, prepared: SoundboardCache.Prepared): AvcrtResult<String> {
        val module = AvcrtPaths.MODULE_DIR
        val dir = AvcrtPaths.SOUNDBOARD_DIR
        val clip = AvcrtPaths.SOUNDBOARD_CLIP
        val playbackFile = AvcrtPaths.SOUNDBOARD_PLAYBACK
        val tmpClip = "$dir/.link-pending"
        val tmpPlayback = "$dir/.playback-pending"
        // Only the active PCM is visible to capture clients. Its hard link and the tiny
        // start marker cost the same regardless of clip length. Rename the marker last.
        return root.exec("""
            (
                umask 077
                rm -f '$tmpClip' '$tmpPlayback' || exit 1
                trap 'rm -f "$tmpClip" "$tmpPlayback"' EXIT
                ln '${prepared.path}' '$tmpClip' || exit 1
                cat ${quote(staging.absolutePath)} > '$tmpPlayback' || exit 1
                if [ -d '$module/zygisk' ] || [ -f '$module/bin/avcrt_inject' ]; then
                    chcon '${AvcrtPaths.SOUNDBOARD_CONTEXT}' '$tmpClip' '$tmpPlayback' || exit 1
                fi
                chmod 0644 '$tmpClip' '$tmpPlayback' || exit 1
                mv -f '$tmpClip' '$clip' || exit 1
                mv -f '$tmpPlayback' '$playbackFile' || { rm -f '$clip'; exit 1; }
            )
        """.trimIndent())
    }

    private fun storageFailure(result: AvcrtResult.Failure): AvcrtResult.Failure {
        Timber.w(result.error.cause, "%s", result.error.message)
        return failure(R.string.error_sound_storage)
    }

    private fun failure(resId: Int, cause: Throwable? = null) = AvcrtResult.Failure(
        AvcrtError.Io("Soundboard operation failed", cause, UserMessage(resId)),
    )

    private companion object {
        const val START_LEAD_MS = 60L
        const val HEADER_BYTES = 32
        const val CLIP_MAGIC = 0x324D435054524356L // "VCRTPCM2" in little endian
    }
}
