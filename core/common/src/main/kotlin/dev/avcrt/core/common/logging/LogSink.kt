package dev.avcrt.core.common.logging

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import timber.log.Timber
import java.util.concurrent.ConcurrentLinkedDeque
import java.util.concurrent.atomic.AtomicInteger

/**
 * In-memory ring buffer of log entries that backs the Logs tab.
 *
 * Bounded — drops the oldest entries when full so it can't run away on long-running
 * sessions. Use [asTree] to plug it into Timber so every `Timber.x()` call across
 * the app feeds the same buffer.
 */
class LogSink(private val capacity: Int = DEFAULT_CAPACITY) {

    private val buffer = ConcurrentLinkedDeque<LogEntry>()
    private val size = AtomicInteger(0)
    private val sequence = AtomicInteger(0)

    private val _entries = MutableStateFlow<List<LogEntry>>(emptyList())
    val entries: StateFlow<List<LogEntry>> = _entries.asStateFlow()

    private val _events = MutableSharedFlow<LogEntry>(extraBufferCapacity = 64)
    val events: Flow<LogEntry> = _events.asSharedFlow()

    fun write(
        level: LogLevel,
        tag: String?,
        message: String,
        throwable: Throwable? = null,
        timestampMillis: Long = System.currentTimeMillis(),
    ) {
        val entry = LogEntry(
            id = sequence.incrementAndGet().toLong(),
            level = level,
            tag = tag,
            message = message,
            throwable = throwable,
            timestampMillis = timestampMillis,
        )
        buffer.addLast(entry)
        if (size.incrementAndGet() > capacity) {
            buffer.pollFirst()
            size.decrementAndGet()
        }
        _entries.value = buffer.toList()
        _events.tryEmit(entry)
    }

    fun clear() {
        buffer.clear()
        size.set(0)
        _entries.value = emptyList()
    }

    fun asTree(): Timber.Tree = object : Timber.Tree() {
        override fun log(priority: Int, tag: String?, message: String, t: Throwable?) {
            write(LogLevel.fromAndroidPriority(priority), tag, message, t)
        }
    }

    companion object {
        const val DEFAULT_CAPACITY = 2_000
    }
}
