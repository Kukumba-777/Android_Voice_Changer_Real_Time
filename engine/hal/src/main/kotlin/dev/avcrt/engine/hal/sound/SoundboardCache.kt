package dev.avcrt.engine.hal.sound

import android.content.Context
import android.system.Os
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.startup.Initializer
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootStatus
import dev.avcrt.domain.media.MediaFile
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.MediaRepository
import dev.avcrt.engine.hal.BundledModuleMeta
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import timber.log.Timber
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.security.MessageDigest
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/** Persistent, disposable mono PCM. Only the original files belong to the library/backups. */
@Singleton
class SoundboardCache @Inject constructor(
    @ApplicationContext private val context: Context,
    private val library: MediaRepository,
    private val root: RootAccess,
    dispatchers: DispatcherProvider,
) : Initializer {
    private val scope = CoroutineScope(SupervisorJob() + dispatchers.io)
    // A play request can prepare a different clip without waiting for a long import. The
    // background worker is serial, and a second request for the same file joins that work.
    private val locks = Array(32) { Mutex() }
    private val files = Mutex() // only small commands (publish/prune), never bulk IO
    private val librarySnapshot = MutableStateFlow<List<MediaFile>?>(null)
    private val stagingDir by lazy {
        File(context.cacheDir, "soundboard-v2").apply {
            listFiles()?.filter { it.name.startsWith(".decode-") }?.forEach { it.delete() }
        }
    }

    override fun initialize() {
        scope.launch {
            library.observe(MediaFolder.SOUNDS).collect { librarySnapshot.value = it }
        }
        scope.launch {
            combine(librarySnapshot, root.status) { sounds, access ->
                sounds.takeIf { access is RootStatus.Available && access.granted }
            }.collectLatest { sounds ->
                if (sounds == null || !moduleReady()) return@collectLatest
                try {
                    prune()
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    Timber.w(e, "Could not prune soundboard cache")
                }
                for (sound in sounds) {
                    try {
                        prepare(sound)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        Timber.w(e, "Could not prebuild soundboard cache")
                    }
                }
            }
        }
    }

    internal suspend fun findSound(id: String): MediaFile? =
        librarySnapshot.filterNotNull().first().firstOrNull { it.id == id }

    internal suspend fun moduleReady(): Boolean {
        val module = AvcrtPaths.MODULE_DIR
        val dir = AvcrtPaths.SOUNDBOARD_CACHE
        val result = root.exec(
            "[ -d '$dir' ] && [ ! -e '$module/disable' ] && [ ! -e '$module/remove' ]" +
                " && [ ! -f '${AvcrtPaths.STAGED_MODULE_DIR}/module.prop' ]" +
                " && [ ! -e '${AvcrtPaths.INSTALL_PENDING_MARKER}' ]" +
                " && sed -n 's/^versionCode=//p' '$module/module.prop'",
        ) as? AvcrtResult.Success ?: return false
        return (result.value.trim().toLongOrNull() ?: 0L) >= BundledModuleMeta.VERSION_CODE
    }

    internal data class Prepared(val path: String, val frames: Long, val inode: Long)

    internal suspend fun prepare(sound: MediaFile): Prepared {
        val key = key(sound)
        val path = "${AvcrtPaths.SOUNDBOARD_CACHE}/$key.pcm"
        readPrepared(path)?.let { return it }
        return locks[(key.hashCode() and Int.MAX_VALUE) % locks.size].withLock {
            if (key(sound) != key) throw IOException("Sound source changed")
            readPrepared(path) ?: build(sound, key, path)
        }
    }

    private suspend fun build(sound: MediaFile, key: String, path: String): Prepared {
        val source = storageIo {
            val dir = stagingDir
            if (!dir.isDirectory && !dir.mkdir() && !dir.isDirectory) throw IOException("Cannot create PCM staging directory")
            File.createTempFile(".decode-", ".pcm", dir)
        }
        val privateStage = "${AvcrtPaths.SOUNDBOARD_CACHE}/.build-${UUID.randomUUID()}.pcm"
        var staged = false
        try {
            val frames = storageIo { source.outputStream() }.use { stream ->
                val output = object : OutputStream() {
                    override fun write(b: Int) = storageIo { stream.write(b) }
                    override fun write(b: ByteArray, off: Int, len: Int) = storageIo { stream.write(b, off, len) }
                }
                SoundDecoder.decode(sound.absolutePath, output)
            }
            currentCoroutineContext().ensureActive()
            staged = true
            requireSuccess(root.execBulkIo("""
                (
                    umask 077
                    set -C
                    cat ${quote(source.absolutePath)} > '$privateStage'
                )
            """.trimIndent()))
            files.withLock {
                if (key(sound) != key) throw IOException("Sound source changed")
                requireSuccess(root.exec("mv -f '$privateStage' '$path'"))
                staged = false
            }
            val prepared = readPrepared(path)
            if (prepared?.frames != frames) throw SoundStorageError(IOException("Invalid cached PCM size"))
            return prepared
        } finally {
            source.delete()
            if (staged) withContext(NonCancellable) { root.exec("rm -f '$privateStage'") }
        }
    }

    private suspend fun readPrepared(path: String): Prepared? {
        val result = root.exec("stat -c '%s %i' '$path' 2>/dev/null") as? AvcrtResult.Success ?: return null
        val (bytesText, inodeText) = result.value.trim().split(' ', limit = 2).takeIf { it.size == 2 } ?: return null
        val bytes = bytesText.toLongOrNull() ?: return null
        val inode = inodeText.toULongOrNull()?.toLong() ?: return null
        return if (bytes > 0 && bytes % 2 == 0L) Prepared(path, bytes / 2, inode) else null
    }

    private suspend fun prune() = files.withLock {
        // Read the latest library state under the publish lock so pruning cannot undo
        // a cache publication based on a newer library state.
        val valid = librarySnapshot.value.orEmpty().mapNotNullTo(hashSetOf()) { sound ->
            runCatching { "${key(sound)}.pcm" }.getOrNull()
        }
        val dir = AvcrtPaths.SOUNDBOARD_CACHE
        val entries = root.exec("ls -1 '$dir'") as? AvcrtResult.Success ?: return@withLock
        val stale = entries.value.lineSequence().filter { CACHE_NAME.matches(it) && it !in valid }
        for (chunk in stale.chunked(128)) {
            root.exec("rm -f " + chunk.joinToString(" ") { "'$dir/$it'" })
        }
    }

    private fun key(sound: MediaFile): String {
        val stat = Os.stat(sound.absolutePath)
        val time = stat.st_ctim
        val stamp = "${sound.absolutePath}\n${stat.st_dev}:${stat.st_ino}:${stat.st_size}:${time.tv_sec}:${time.tv_nsec}"
        return MessageDigest.getInstance("SHA-256").digest(stamp.toByteArray()).joinToString("") { "%02x".format(it) }
    }

    private fun requireSuccess(result: AvcrtResult<String>) {
        if (result is AvcrtResult.Failure) throw SoundStorageError(IOException(result.error.message, result.error.cause))
    }

    private companion object {
        val CACHE_NAME = Regex("[0-9a-f]{64}\\.pcm")
    }
}

internal fun quote(path: String) = "'${path.replace("'", "'\\''")}'"

internal class SoundStorageError(cause: IOException) : IOException(cause)

internal inline fun <T> storageIo(block: () -> T): T = try {
    block()
} catch (e: IOException) {
    throw SoundStorageError(e)
}
