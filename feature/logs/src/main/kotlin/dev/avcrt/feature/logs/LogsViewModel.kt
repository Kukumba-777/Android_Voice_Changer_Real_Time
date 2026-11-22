package dev.avcrt.feature.logs

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.logging.LogEntry
import dev.avcrt.core.common.logging.LogLevel
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.userText
import dev.avcrt.domain.log.ExternalLogReader
import dev.avcrt.domain.log.ExternalLogSource
import dev.avcrt.domain.log.LogRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import javax.inject.Inject

enum class LogFilter(val minLevel: LogLevel) {
    ALL(LogLevel.VERBOSE),
    INFO_PLUS(LogLevel.INFO),
    WARN_PLUS(LogLevel.WARN),
    ERROR_ONLY(LogLevel.ERROR),
}

/** Which log stream the tab is showing. APP is the live in-process Timber log;
 *  the rest are pulled on demand via root ([ExternalLogReader]). */
enum class LogSource { APP, DAEMON, ZYGISK, KERNEL }

data class LogsUiState(
    val entries: List<LogEntry> = emptyList(),
    val filter: LogFilter = LogFilter.ALL,
    val source: LogSource = LogSource.APP,
    val loading: Boolean = false,
    val errorMessage: String? = null,
)

/**
 * Read-side ViewModel for the multi-source Logs tab.
 *
 * APP entries stream live from the shared LogSink (via [LogRepository]); the
 * external sources (daemon file / zygisk+effect logcat / kernel dmesg) are
 * one-shot root reads refreshed on demand — selecting one auto-refreshes.
 */
@HiltViewModel
class LogsViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val logs: LogRepository,
    private val externalReader: ExternalLogReader,
) : ViewModel() {

    private data class ExternalState(
        val source: LogSource = LogSource.APP,
        val loading: Boolean = false,
        val error: String? = null,
    )

    private val filter = MutableStateFlow(LogFilter.ALL)
    private val external = MutableStateFlow(ExternalState())
    private val externalEntries = MutableStateFlow<List<LogEntry>>(emptyList())
    private var refreshGeneration = 0L

    val state: StateFlow<LogsUiState> = combine(
        logs.observeEntries(),
        externalEntries,
        external,
        filter,
    ) { appEntries, extEntries, ext, f ->
        val base = if (ext.source == LogSource.APP) appEntries else extEntries
        LogsUiState(
            entries = logs.filter(base, f.minLevel).asReversed(),
            filter = f,
            source = ext.source,
            loading = ext.loading,
            errorMessage = ext.error,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000L), LogsUiState())

    fun setFilter(f: LogFilter) { filter.value = f }

    fun selectSource(source: LogSource) {
        if (external.value.source == source) return
        refreshGeneration++
        external.value = ExternalState(source = source)
        externalEntries.value = emptyList()
        if (source != LogSource.APP) refresh()
    }

    fun refresh() {
        val source = external.value.source
        if (source == LogSource.APP) return
        val domainSource = source.toDomain() ?: return
        val generation = ++refreshGeneration
        viewModelScope.launch {
            if (generation != refreshGeneration) return@launch
            external.update { it.copy(loading = true, error = null) }
            when (val res = externalReader.read(domainSource)) {
                is AvcrtResult.Success -> {
                    if (generation != refreshGeneration) return@launch
                    externalEntries.value = res.value
                    external.update { it.copy(loading = false) }
                }
                is AvcrtResult.Failure -> {
                    if (generation != refreshGeneration) return@launch
                    external.update { it.copy(loading = false, error = res.error.userText(context)) }
                }
            }
        }
    }

    /** Clear only makes sense for the in-memory APP log; for external sources it
     *  just empties the on-screen snapshot (the underlying logs are untouched). */
    fun clear() {
        if (external.value.source == LogSource.APP) {
            logs.clear()
        } else {
            refreshGeneration++
            externalEntries.value = emptyList()
            external.update { it.copy(loading = false, error = null) }
        }
    }

    private fun LogSource.toDomain(): ExternalLogSource? = when (this) {
        LogSource.APP -> null
        LogSource.DAEMON -> ExternalLogSource.DAEMON
        LogSource.ZYGISK -> ExternalLogSource.ZYGISK
        LogSource.KERNEL -> ExternalLogSource.KERNEL
    }
}
