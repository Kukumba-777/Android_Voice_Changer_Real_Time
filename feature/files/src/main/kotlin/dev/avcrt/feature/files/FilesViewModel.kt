package dev.avcrt.feature.files

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.userText
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.MediaRepository
import dev.avcrt.domain.media.UserBackup
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import timber.log.Timber
import java.io.IOException
import javax.inject.Inject

@HiltViewModel
internal class FilesViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val library: MediaRepository,
    private val backup: UserBackup,
    private val dispatchers: DispatcherProvider,
) : ViewModel() {
    val files = MediaFolder.entries.associateWith { folder ->
        library.observe(folder).stateIn(viewModelScope, SharingStarted.WhileSubscribed(), emptyList())
    }
    private val _busy = MutableStateFlow(false)
    val busy = _busy.asStateFlow()
    private val _message = MutableStateFlow<String?>(null)
    val message = _message.asStateFlow()

    fun import(folder: MediaFolder, uris: List<Uri>) = runEdit {
        if (uris.isEmpty()) return@runEdit
        val resolver = context.contentResolver
        var imported = 0
        var error: String? = null
        for (uri in uris) {
            try {
                val name = withContext(dispatchers.io) {
                    resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                        if (it.moveToFirst()) it.getString(0) else null
                    }
                } ?: throw IOException("No display name")
                when (val result = library.import(folder, name) {
                    resolver.openInputStream(uri) ?: throw IOException("Cannot open document")
                }) {
                    is AvcrtResult.Success -> imported++
                    is AvcrtResult.Failure -> error = result.error.userText(context)
                }
            } catch (e: IOException) {
                error = context.getString(R.string.files_open_failed)
            } catch (e: SecurityException) {
                error = context.getString(R.string.files_open_failed)
            } catch (e: IllegalArgumentException) {
                error = context.getString(R.string.files_open_failed)
            }
        }
        val summary = context.getString(R.string.files_imported, imported, uris.size)
        _message.value = listOfNotNull(summary, error).joinToString("\n")
    }

    fun delete(folder: MediaFolder, id: String) = runEdit {
        _message.value = when (val result = library.delete(folder, id)) {
            is AvcrtResult.Success -> context.getString(R.string.files_deleted)
            is AvcrtResult.Failure -> result.error.userText(context)
        }
    }

    fun exportBackup(uri: Uri) = runEdit {
        val result = backup.export {
            context.contentResolver.openOutputStream(uri, "wt") ?: throw IOException("Cannot write backup")
        }
        backupMessage(result, R.string.files_backup_saved, R.string.files_backup_export_failed)
    }

    fun restoreBackup(uri: Uri) = runEdit {
        val result = backup.restore {
            context.contentResolver.openInputStream(uri) ?: throw IOException("Cannot read backup")
        }
        backupMessage(result, R.string.files_backup_restored, R.string.files_backup_restore_failed)
    }

    private fun backupMessage(result: AvcrtResult<Unit>, success: Int, failure: Int) {
        if (result is AvcrtResult.Failure) Timber.w(result.error.cause, "%s", result.error.message)
        _message.value = context.getString(if (result is AvcrtResult.Success) success else failure)
    }

    fun consumeMessage() { _message.value = null }

    private fun runEdit(action: suspend () -> Unit) {
        if (_busy.value) return
        _busy.value = true
        viewModelScope.launch {
            try { action() } finally { _busy.value = false }
        }
    }
}
