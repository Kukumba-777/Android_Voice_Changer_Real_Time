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
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.MediaRepository
import dev.avcrt.domain.media.Soundboard
import dev.avcrt.domain.media.SoundboardState
import dev.avcrt.engine.hal.BundledModuleMeta
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
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
    private val library: MediaRepository,
    private val root: RootAccess,
    private val dispatchers: DispatcherProvider,
) : Soundboard {
    private val scope = CoroutineScope(SupervisorJob() + dispatchers.io)
    private val mutex = Mutex()
    private val playback = MutableStateFlow<SoundboardState>(SoundboardState.Idle)
    override val state = playback.asStateFlow()
    private var request = 0L
    private var expiration: Job? = null
    private var published = false

    init {
        scope.launch {
            engine.observeState().collect {
                if (it !is EngineState.Running && playback.value != SoundboardState.Idle) stop()
            }
        }
    }

    override suspend fun play(id: String, clipVolumePercent: Int): AvcrtResult<Unit> {
        val token = mutex.withLock {
            if (!isEngineRunning()) return failure(R.string.error_sound_engine_off)
            expiration?.cancel()
            if (published) {
                val result = clearClip()
                if (result is AvcrtResult.Failure) return result
            }
            playback.value = SoundboardState.Preparing(id)
            ++request
        }
        return try {
            withContext(dispatchers.io) {
                if (!hasSoundboardModule()) return@withContext failure(R.string.error_sound_module)
                val sound = library.observe(MediaFolder.SOUNDS).first().firstOrNull { it.id == id }
                    ?: return@withContext failure(R.string.error_sound_decode)
                val pcm = SoundDecoder.decode(sound.absolutePath, clipVolumePercent.coerceIn(0, 100) / 100f)
                val staging = File.createTempFile("avcrt-sound-", ".pcm", context.cacheDir)
                try {
                    mutex.withLock {
                        if (request != token) return@withLock AvcrtResult.Success(Unit)
                        if (!isEngineRunning()) return@withLock failure(R.string.error_sound_engine_off)
                        val startMs = SystemClock.elapsedRealtime() + START_LEAD_MS
                        staging.outputStream().buffered().use { stream ->
                            stream.write(ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                                .putLong(CLIP_MAGIC).putLong(startMs).array())
                            stream.write(pcm)
                        }
                        // Complete publication even if the calling overlay is being torn down.
                        withContext(NonCancellable) publishClip@{
                            val result = publish(staging)
                            if (result is AvcrtResult.Failure) return@publishClip result
                            published = true
                            playback.value = SoundboardState.Playing(id)
                            val endMs = startMs + pcm.size * 1000L / (Short.SIZE_BYTES * SoundDecoder.SAMPLE_RATE)
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
        } catch (e: SoundDecoder.TooLong) {
            failure(R.string.error_sound_too_long, e)
        } catch (e: Exception) {
            Timber.w(e, "Soundboard could not prepare clip")
            failure(R.string.error_sound_decode, e)
        } finally {
            withContext(NonCancellable) {
                mutex.withLock {
                    if (request == token && playback.value is SoundboardState.Preparing) {
                        playback.value = SoundboardState.Idle
                    }
                }
            }
        }
    }

    override suspend fun stop(): AvcrtResult<Unit> = mutex.withLock {
        ++request
        expiration?.cancel()
        clearClip()
    }

    private suspend fun clearClip(): AvcrtResult<Unit> = withContext(NonCancellable) {
        val result = if (published) root.exec("rm -f '${AvcrtPaths.SOUNDBOARD_CLIP}'") else AvcrtResult.Success("")
        if (result is AvcrtResult.Failure) return@withContext result
        published = false
        playback.value = SoundboardState.Idle
        AvcrtResult.Success(Unit)
    }

    private suspend fun isEngineRunning() = engine.observeState().first() is EngineState.Running

    private suspend fun hasSoundboardModule(): Boolean {
        val result = root.exec(
            "[ -d '${AvcrtPaths.SOUNDBOARD_DIR}' ] && sed -n 's/^versionCode=//p' '${AvcrtPaths.MODULE_DIR}/module.prop'",
        ) as? AvcrtResult.Success ?: return false
        return (result.value.trim().toLongOrNull() ?: 0L) >= BundledModuleMeta.VERSION_CODE
    }

    private suspend fun publish(staging: File): AvcrtResult<String> {
        val dir = AvcrtPaths.SOUNDBOARD_DIR
        val dest = AvcrtPaths.SOUNDBOARD_CLIP
        val module = AvcrtPaths.MODULE_DIR
        val source = "'${staging.absolutePath.replace("'", "'\\''")}'"
        return root.exec("""
            (
                umask 077
                tmp=${'$'}(mktemp '$dir/.clip.XXXXXX') || exit 1
                trap 'rm -f "${'$'}tmp"' EXIT
                cat $source > "${'$'}tmp" || exit 1
                if [ -d '$module/zygisk' ] || [ -f '$module/bin/avcrt_inject' ]; then
                    chcon '${AvcrtPaths.SOUNDBOARD_CONTEXT}' "${'$'}tmp" || exit 1
                fi
                chmod 0644 "${'$'}tmp" && mv -f "${'$'}tmp" '$dest'
            )
        """.trimIndent())
    }

    private fun failure(resId: Int, cause: Throwable? = null) = AvcrtResult.Failure(
        AvcrtError.Io("Soundboard operation failed", cause, UserMessage(resId)),
    )

    private companion object {
        const val START_LEAD_MS = 220L
        const val CLIP_MAGIC = 0x314D435054524356L // "VCRTPCM1" in little endian
    }
}
