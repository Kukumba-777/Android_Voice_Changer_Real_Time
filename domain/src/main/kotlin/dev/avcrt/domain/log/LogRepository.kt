package dev.avcrt.domain.log

import dev.avcrt.core.common.logging.LogEntry
import dev.avcrt.core.common.logging.LogLevel
import kotlinx.coroutines.flow.Flow

interface LogRepository {
    fun observeEntries(): Flow<List<LogEntry>>
    fun observeNew(): Flow<LogEntry>
    fun clear()
    fun filter(entries: List<LogEntry>, minLevel: LogLevel): List<LogEntry>
}
