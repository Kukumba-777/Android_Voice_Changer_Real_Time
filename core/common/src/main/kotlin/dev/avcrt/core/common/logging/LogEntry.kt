package dev.avcrt.core.common.logging

import android.util.Log

data class LogEntry(
    val id: Long,
    val level: LogLevel,
    val tag: String?,
    val message: String,
    val throwable: Throwable?,
    val timestampMillis: Long,
)

enum class LogLevel(val androidPriority: Int) {
    VERBOSE(Log.VERBOSE),
    DEBUG(Log.DEBUG),
    INFO(Log.INFO),
    WARN(Log.WARN),
    ERROR(Log.ERROR),
    ASSERT(Log.ASSERT);

    companion object {
        fun fromAndroidPriority(priority: Int): LogLevel = entries.firstOrNull {
            it.androidPriority == priority
        } ?: DEBUG
    }
}
