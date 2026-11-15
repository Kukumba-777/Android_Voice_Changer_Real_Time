package dev.avcrt.feature.logs

import android.content.Intent
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.DeleteSweep
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.core.content.ContextCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavController
import dev.avcrt.core.common.logging.LogEntry
import dev.avcrt.core.common.logging.LogLevel
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@Composable
fun LogsScreen(
    navController: NavController,
    viewModel: LogsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val listState = rememberLazyListState()
    val shareSubject = stringResource(R.string.logs_share_subject)
    val dateFormat = remember { SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault()) }

    LaunchedEffect(state.source, state.entries.firstOrNull()?.id) {
        if (state.entries.isNotEmpty()) listState.animateScrollToItem(0)
    }

    AvcrtScreen(
        title = stringResource(R.string.logs_title),
        actions = {
            if (state.source != LogSource.APP) {
                IconButton(onClick = { viewModel.refresh() }) {
                    Icon(
                        Icons.Rounded.Refresh,
                        contentDescription = stringResource(R.string.logs_refresh),
                    )
                }
            }
            IconButton(onClick = {
                val text = state.entries.joinToString(separator = "\n") {
                    it.toShareLine(dateFormat)
                }
                val intent = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_SUBJECT, shareSubject)
                    putExtra(Intent.EXTRA_TEXT, text)
                }
                ContextCompat.startActivity(
                    context,
                    Intent.createChooser(intent, shareSubject),
                    null,
                )
            }) {
                Icon(Icons.Rounded.Share, contentDescription = stringResource(R.string.logs_share))
            }
            IconButton(onClick = { viewModel.clear() }) {
                Icon(Icons.Rounded.DeleteSweep, contentDescription = stringResource(R.string.logs_clear))
            }
        },
    ) { _ ->
        Column(modifier = Modifier.fillMaxSize()) {
            SourceRow(state.source, viewModel::selectSource)
            FilterRow(state.filter, viewModel::setFilter)
            Box(modifier = Modifier.fillMaxSize()) {
                when {
                    state.loading -> CircularProgressIndicator(
                        modifier = Modifier.align(Alignment.Center),
                    )
                    state.errorMessage != null -> CenteredMessage(
                        stringResource(R.string.logs_error_prefix, state.errorMessage!!),
                    )
                    state.entries.isEmpty() -> CenteredMessage(
                        stringResource(
                            if (state.source == LogSource.APP) R.string.logs_empty
                            else R.string.logs_empty_source,
                        ),
                    )
                    else -> LazyColumn(
                        state = listState,
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(
                            horizontal = AvcrtSpacing.md,
                            vertical = AvcrtSpacing.sm,
                        ),
                        verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.xxs),
                    ) {
                        items(state.entries, key = { "${state.source}-${it.id}" }) { entry ->
                            LogRow(entry, dateFormat)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SourceRow(current: LogSource, onSelect: (LogSource) -> Unit) {
    val labels = listOf(
        LogSource.APP to stringResource(R.string.logs_source_app),
        LogSource.DAEMON to stringResource(R.string.logs_source_daemon),
        LogSource.ZYGISK to stringResource(R.string.logs_source_zygisk),
        LogSource.KERNEL to stringResource(R.string.logs_source_kernel),
    )
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = AvcrtSpacing.md, vertical = AvcrtSpacing.xs),
        horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
    ) {
        labels.forEach { (source, label) ->
            FilterChip(
                selected = source == current,
                onClick = { onSelect(source) },
                label = { Text(label) },
            )
        }
    }
}

@Composable
private fun FilterRow(current: LogFilter, onSelect: (LogFilter) -> Unit) {
    val labels = listOf(
        LogFilter.ALL to stringResource(R.string.logs_filter_all),
        LogFilter.INFO_PLUS to stringResource(R.string.logs_filter_info),
        LogFilter.WARN_PLUS to stringResource(R.string.logs_filter_warn),
        LogFilter.ERROR_ONLY to stringResource(R.string.logs_filter_error),
    )
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = AvcrtSpacing.md, vertical = AvcrtSpacing.sm),
        horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
    ) {
        labels.forEach { (filter, label) ->
            FilterChip(
                selected = filter == current,
                onClick = { onSelect(filter) },
                label = { Text(label) },
            )
        }
    }
}

@Composable
private fun LogRow(entry: LogEntry, dateFormat: SimpleDateFormat) {
    Surface(
        color = colorForLevel(entry.level),
        shape = MaterialTheme.shapes.small,
    ) {
        Column(modifier = Modifier.padding(AvcrtSpacing.sm)) {
            val time = if (entry.timestampMillis > 0L) {
                "${dateFormat.format(Date(entry.timestampMillis))} "
            } else {
                ""
            }
            Text(
                text = "$time${entry.level.name.take(1)} ${entry.tag ?: ""}".trim(),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                text = entry.message,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
            entry.throwable?.let {
                Text(
                    text = it.stackTraceToString().take(MAX_STACK),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error,
                    fontFamily = FontFamily.Monospace,
                )
            }
        }
    }
}

@Composable
private fun colorForLevel(level: LogLevel) = when (level) {
    LogLevel.ERROR, LogLevel.ASSERT -> MaterialTheme.colorScheme.errorContainer
    LogLevel.WARN -> MaterialTheme.colorScheme.tertiaryContainer
    LogLevel.INFO -> MaterialTheme.colorScheme.surfaceVariant
    LogLevel.DEBUG, LogLevel.VERBOSE -> MaterialTheme.colorScheme.surfaceVariant
}

@Composable
private fun CenteredMessage(text: String) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(AvcrtSpacing.xxl),
        )
    }
}

private fun LogEntry.toShareLine(dateFormat: SimpleDateFormat): String = buildString {
    if (timestampMillis > 0L) {
        append(dateFormat.format(Date(timestampMillis)))
        append(' ')
    }
    append(level.name.take(1))
    if (tag != null) {
        append(' ')
        append(tag)
    }
    append(": ")
    append(message)
    throwable?.let {
        append('\n')
        append(it.stackTraceToString())
    }
}

private const val MAX_STACK = 2_000
