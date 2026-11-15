package dev.avcrt.data.log

import dev.avcrt.core.common.logging.LogEntry
import dev.avcrt.core.common.logging.LogLevel
import dev.avcrt.core.common.logging.LogSink
import dev.avcrt.domain.log.LogRepository
import kotlinx.coroutines.flow.Flow
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class LogRepositoryImpl @Inject constructor(
    private val sink: LogSink,
) : LogRepository {

    override fun observeEntries(): Flow<List<LogEntry>> = sink.entries

    override fun observeNew(): Flow<LogEntry> = sink.events

    override fun clear() {
        sink.clear()
    }

    override fun filter(entries: List<LogEntry>, minLevel: LogLevel): List<LogEntry> =
        entries.filter { it.level.androidPriority >= minLevel.androidPriority }
}
