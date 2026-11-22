package dev.avcrt.data.media

import android.content.Context
import android.media.MediaExtractor
import android.media.MediaFormat
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.R
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.UserMessage
import dev.avcrt.domain.media.MediaFile
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.MediaRepository
import dev.avcrt.domain.media.SOUNDBOARD_MAX_SECONDS
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.io.InputStream
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class MediaRepositoryImpl @Inject constructor(
    @ApplicationContext private val context: Context,
    private val dispatchers: DispatcherProvider,
) : MediaRepository {
    private val entries = MediaFolder.entries.associateWith { MutableStateFlow<List<MediaFile>>(emptyList()) }
    private val lock = Mutex()

    override fun observe(folder: MediaFolder) = flow {
        refresh(folder)
        emitAll(entries.getValue(folder))
    }

    override suspend fun refresh(folder: MediaFolder) = withContext(dispatchers.io) {
        lock.withLock { scan(folder) }
    }

    override suspend fun import(folder: MediaFolder, name: String, open: () -> InputStream) = edit(folder) {
        val safeName = name.substringAfterLast('/').substringAfterLast('\\').trim()
            .replace(Regex("[\\p{Cntrl}]"), "_")
        if (!isLibraryName(folder, safeName)) throw ImportError(R.string.error_media_file_type)
        val dir = directory(folder)
        check(dir.isDirectory || dir.mkdirs())
        val destination = File(dir, safeName)
        if (destination.exists()) throw ImportError(R.string.error_media_exists)
        val temporary = File.createTempFile(".import-", ".tmp", dir)
        try {
            val limit = maxBytes(folder)
            val scope = currentCoroutineContext()
            var total = 0L
            open().use { input ->
                temporary.outputStream().buffered().use { output ->
                    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                    while (true) {
                        scope.ensureActive()
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        if (total > limit) throw ImportError(R.string.error_media_too_large)
                        output.write(buffer, 0, count)
                    }
                }
            }
            if (total == 0L) throw ImportError(R.string.error_media_file_type)
            if (folder == MediaFolder.SOUNDS) validateAudio(temporary)
            scope.ensureActive()
            check(temporary.renameTo(destination))
        } finally {
            temporary.delete()
        }
    }

    override suspend fun delete(folder: MediaFolder, id: String) = edit(folder) {
        val file = directory(folder).listFiles()?.firstOrNull { it.isFile && !it.isHidden && it.name == id }
        check(file?.delete() == true)
    }

    internal suspend fun <T> withLibraryFiles(action: suspend (Map<MediaFolder, List<MediaFile>>) -> T): T =
        lock.withLock { action(MediaFolder.entries.associateWith(::listFiles)) }

    internal data class RestoreFile(val folder: MediaFolder, val name: String, val copyTo: suspend (File) -> Unit)

    /** Validate and stage every file before publishing; never replace an existing library file. */
    internal suspend fun restoreFiles(files: List<RestoreFile>, saveMetadata: suspend () -> Unit) = lock.withLock {
        val staged = mutableListOf<Pair<File, File>>()
        val added = mutableListOf<File>()
        try {
            for (file in files) {
                check(isLibraryName(file.folder, file.name))
                val dir = directory(file.folder)
                check(dir.isDirectory || dir.mkdirs())
                val target = File(dir, file.name)
                // Stage even collisions so corrupt entries are still rejected before applying metadata.
                val temporary = File.createTempFile(".restore-", ".tmp", dir)
                staged.add(temporary to target)
                file.copyTo(temporary)
                if (file.folder == MediaFolder.SOUNDS) validateAudio(temporary)
            }
            currentCoroutineContext().ensureActive()
            for ((temporary, target) in staged) {
                if (target.exists()) continue
                check(temporary.renameTo(target))
                added.add(target)
            }
            withContext(NonCancellable) { saveMetadata() }
        } catch (e: Exception) {
            added.forEach { it.delete() }
            throw e
        } finally {
            staged.forEach { (temporary, _) -> temporary.delete() }
            MediaFolder.entries.forEach(::scan)
        }
    }

    private fun directory(folder: MediaFolder): File = when (folder) {
        // Keep existing manually-installed ONNX models visible to the converter.
        MediaFolder.MODELS -> File(context.getExternalFilesDir(null) ?: context.filesDir, AvcrtPaths.APP_MODELS_DIR)
        MediaFolder.SOUNDS -> File(context.filesDir, AvcrtPaths.APP_SOUNDS_DIR)
    }

    private fun listFiles(folder: MediaFolder) = directory(folder).listFiles().orEmpty()
            .filter { it.isFile && !it.isHidden && (folder != MediaFolder.MODELS || it.extension.equals("onnx", true)) }
            .map { MediaFile(it.name, it.absolutePath, it.length(), it.lastModified()) }

    private fun scan(folder: MediaFolder) {
        entries.getValue(folder).value = listFiles(folder)
    }

    private suspend fun edit(folder: MediaFolder, action: suspend () -> Unit): AvcrtResult<Unit> =
        withContext(dispatchers.io) {
            lock.withLock {
                try {
                    action()
                    scan(folder)
                    AvcrtResult.Success(Unit)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: ImportError) {
                    AvcrtResult.Failure(AvcrtError.Io("Media import rejected", e, UserMessage(e.messageRes)))
                } catch (e: Exception) {
                    AvcrtResult.Failure(AvcrtError.Io("Media library operation failed", e))
                }
            }
        }

    private fun validateAudio(file: File) {
        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(file.absolutePath)
            val audio = (0 until extractor.trackCount).map(extractor::getTrackFormat)
                .firstOrNull { it.getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true }
                ?: throw ImportError(R.string.error_media_file_type)
            if (audio.containsKey(MediaFormat.KEY_DURATION) &&
                audio.getLong(MediaFormat.KEY_DURATION) > SOUNDBOARD_MAX_SECONDS * 1_000_000L
            ) throw ImportError(R.string.error_sound_too_long)
        } catch (e: ImportError) {
            throw e
        } catch (e: Exception) {
            throw ImportError(R.string.error_media_file_type)
        } finally {
            extractor.release()
        }
    }

    private class ImportError(val messageRes: Int) : Exception()

    internal companion object {
        fun maxBytes(folder: MediaFolder) = if (folder == MediaFolder.SOUNDS) 32L shl 20 else 1024L shl 20

        fun isLibraryName(folder: MediaFolder, name: String) = name.isNotBlank() && !name.startsWith('.') &&
            name.toByteArray().size <= 240 && '/' !in name && '\\' !in name && name.none { it.isISOControl() } &&
            (folder != MediaFolder.MODELS || name.endsWith(".onnx", ignoreCase = true))
    }
}
