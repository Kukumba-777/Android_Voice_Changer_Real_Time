package dev.avcrt.data.backup

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.datastore.SettingsDataStore
import dev.avcrt.data.media.MediaRepositoryImpl
import dev.avcrt.data.preset.PresetRepositoryImpl
import dev.avcrt.data.settings.SettingsRepositoryImpl
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.UserBackup
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetDspValidator
import dev.avcrt.domain.preset.PresetOrigin
import dev.avcrt.domain.settings.AppLocale
import dev.avcrt.domain.settings.ThemeMode
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import javax.inject.Inject

/** Versioned user content only. No raw DataStore, root/runtime files or active engine settings. */
class UserBackupImpl @Inject constructor(
    @ApplicationContext private val context: Context,
    @SettingsDataStore private val dataStore: DataStore<Preferences>,
    private val presets: PresetRepositoryImpl,
    private val settings: SettingsRepositoryImpl,
    private val library: MediaRepositoryImpl,
    private val dispatchers: DispatcherProvider,
) : UserBackup {
    private val json = Json { encodeDefaults = true }

    override suspend fun export(open: () -> OutputStream) = onIo("Backup export failed") {
        library.withLibraryFiles { files ->
            val snapshot = dataStore.data.first()
            val s = settings.readSettings(snapshot)
            val backup = Manifest(VERSION, s.themeMode, s.useDynamicColor, s.locale, s.experimentalEnabled, presets.readCustoms(snapshot))
            val metadata = json.encodeToString(Manifest.serializer(), backup).toByteArray()
            require(metadata.size <= MAX_METADATA_BYTES)
            val budget = ArchiveBudget()
            budget.add(metadata.size.toLong(), MAX_METADATA_BYTES)
            for ((folder, list) in files) for (file in list) {
                require(MediaRepositoryImpl.isLibraryName(folder, file.id))
                budget.add(file.sizeBytes, MediaRepositoryImpl.maxBytes(folder))
            }
            ZipOutputStream(open().buffered()).use { zip ->
                zip.putNextEntry(ZipEntry(MANIFEST))
                zip.write(metadata)
                zip.closeEntry()
                for ((folder, list) in files) for (file in list) {
                    zip.putNextEntry(ZipEntry("${folder.archivePath}/${file.id}").apply { time = file.modifiedAtMillis })
                    File(file.absolutePath).inputStream().use { input ->
                        require(copyBounded(input, zip, file.sizeBytes) == file.sizeBytes)
                    }
                    zip.closeEntry()
                }
            }
        }
    }

    override suspend fun restore(open: () -> InputStream) = onIo("Backup restore failed") {
        val archive = File.createTempFile("avcrt-backup-", ".zip", context.cacheDir)
        try {
            open().use { input -> archive.outputStream().use { copyBounded(input, it, MAX_ARCHIVE_BYTES) } }
            ZipFile(archive).use { zip -> restoreZip(zip) }
        } finally {
            archive.delete()
        }
    }

    private suspend fun restoreZip(zip: ZipFile) {
        val names = hashSetOf<String>()
        val files = mutableListOf<MediaRepositoryImpl.RestoreFile>()
        val budget = ArchiveBudget()
        var manifest: Manifest? = null
        for (entry in zip.entries()) {
            currentCoroutineContext().ensureActive()
            require(!entry.isDirectory && names.add(entry.name))
            if (entry.name == MANIFEST) {
                budget.add(entry.size, MAX_METADATA_BYTES)
                val bytes = java.io.ByteArrayOutputStream(entry.size.toInt())
                copyEntry(zip, entry, bytes)
                manifest = json.decodeFromString(Manifest.serializer(), bytes.toByteArray().decodeToString(throwOnInvalidSequence = true))
            } else {
                val folder = MediaFolder.entries.firstOrNull { it.archivePath == entry.name.substringBefore('/') }
                    ?: error("Unexpected backup entry")
                val name = entry.name.substringAfter('/', "")
                require(MediaRepositoryImpl.isLibraryName(folder, name))
                budget.add(entry.size, MediaRepositoryImpl.maxBytes(folder))
                files.add(MediaRepositoryImpl.RestoreFile(folder, name) { file ->
                    file.outputStream().buffered().use { copyEntry(zip, entry, it) }
                    if (entry.time > 0) file.setLastModified(entry.time)
                })
            }
        }
        val backup = requireNotNull(manifest)
        require(backup.version == VERSION)
        val ids = presets.builtIns().mapTo(hashSetOf()) { it.id }
        for (preset in backup.customs) {
            require(preset.origin == PresetOrigin.CUSTOM && CUSTOM_ID.matches(preset.id) && ids.add(preset.id))
            require(preset.names.values.any { it.isNotBlank() } && PresetDspValidator.isValid(preset.dsp))
        }
        library.restoreFiles(files) {
            dataStore.edit {
                presets.mergeCustoms(it, backup.customs)
                settings.restorePreferences(it, backup.themeMode, backup.dynamicColor, backup.locale, backup.experimental)
            }
        }
    }

    private suspend fun copyEntry(zip: ZipFile, entry: ZipEntry, output: OutputStream) {
        val crc = CRC32()
        val count = zip.getInputStream(entry).use { copyBounded(it, output, entry.size, crc) }
        require(count == entry.size && crc.value == entry.crc)
    }

    private suspend fun copyBounded(input: InputStream, output: OutputStream, limit: Long, crc: CRC32? = null): Long {
        val coroutine = currentCoroutineContext()
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        var bytes = 0L
        while (true) {
            coroutine.ensureActive()
            val n = input.read(buffer)
            if (n < 0) return bytes
            bytes += n
            require(bytes <= limit)
            output.write(buffer, 0, n)
            crc?.update(buffer, 0, n)
        }
    }

    private suspend fun onIo(message: String, action: suspend () -> Unit): AvcrtResult<Unit> = withContext(dispatchers.io) {
        try {
            action()
            AvcrtResult.Success(Unit)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            AvcrtResult.Failure(AvcrtError.Io(message, e))
        }
    }

    @Serializable
    private data class Manifest(
        val version: Int,
        val themeMode: ThemeMode,
        val dynamicColor: Boolean,
        val locale: AppLocale,
        val experimental: Boolean,
        val customs: List<Preset>,
    )

    private class ArchiveBudget {
        private var count = 0
        private var bytes = 0L

        fun add(size: Long, entryLimit: Long) {
            require(++count <= MAX_ENTRIES && size in 1..entryLimit)
            bytes += size
            require(bytes <= MAX_CONTENT_BYTES)
        }
    }

    private val MediaFolder.archivePath get() = when (this) {
        MediaFolder.MODELS -> "models"
        MediaFolder.SOUNDS -> "sounds"
    }

    private companion object {
        const val VERSION = 1
        const val MANIFEST = "manifest.json"
        const val MAX_ENTRIES = 4096
        const val MAX_METADATA_BYTES = 4L * 1024 * 1024
        const val MAX_CONTENT_BYTES = 4L * 1024 * 1024 * 1024
        const val MAX_ARCHIVE_BYTES = MAX_CONTENT_BYTES + 32 * 1024 * 1024
        val CUSTOM_ID = Regex("custom\\.[a-zA-Z0-9_-]{1,100}")
    }
}
