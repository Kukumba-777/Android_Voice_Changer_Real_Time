package dev.avcrt.data.log

import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.logging.LogEntry
import dev.avcrt.core.common.logging.LogLevel
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.map
import dev.avcrt.core.root.RootAccess
import dev.avcrt.domain.log.ExternalLogReader
import dev.avcrt.domain.log.ExternalLogSource
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Reads the out-of-process log sources via a single root shell command each and
 * parses the lines into [LogEntry]s.
 *
 * Parsing is deliberately MINIMAL/robust: the whole raw line is kept as the
 * message (logcat/dmesg lines already carry their own timestamp + tag, which we
 * want verbatim for debugging), with only a best-effort level so the existing
 * level filter + colours still work. [LogEntry.timestampMillis] is left at 0 —
 * the UI omits the wall-clock column for these and shows the embedded time
 * instead (uptime-relative for dmesg, so converting would be misleading).
 */
@Singleton
class ExternalLogReaderImpl @Inject constructor(
    private val root: RootAccess,
) : ExternalLogReader {

    override suspend fun read(source: ExternalLogSource): AvcrtResult<List<LogEntry>> {
        val (command, tag) = when (source) {
            // Never display a persisted daemon log that belongs to another boot.
            ExternalLogSource.DAEMON ->
                "if ${AvcrtPaths.CURRENT_DAEMON_LOG_GUARD}; then tail -n $MAX_LINES '${AvcrtPaths.DAEMON_LOG_FILE}'; fi" to "avcrtd"
            ExternalLogSource.ZYGISK ->
                "(set -o pipefail; logcat -d -v threadtime -s avcrtzy:V avcrtaf:V avcrtfx:V | tail -n $MAX_LINES)" to "zygisk"
            ExternalLogSource.KERNEL ->
                "dmesg 2>/dev/null | grep -i avcrt | tail -n $MAX_LINES" to "kernel"
        }
        return root.exec(command).map { stdout -> parse(stdout, tag) }
    }

    private fun parse(stdout: String, tag: String): List<LogEntry> {
        var id = 0L
        return stdout.lineSequence()
            .map { it.trimEnd() }
            .filter { it.isNotBlank() }
            .filterNot { it.startsWith("--------- beginning of ") }
            .map { line ->
                LogEntry(
                    id = id++,
                    level = detectLevel(line),
                    tag = tag,
                    message = line,
                    throwable = null,
                    timestampMillis = 0L,
                )
            }
            .toList()
    }

    /** Parse the priority column in logcat threadtime output, otherwise use keywords. */
    private fun detectLevel(line: String): LogLevel {
        LOGCAT_LEVEL.find(line)?.groupValues?.getOrNull(1)?.let { letter ->
            when (letter) {
                "V" -> LogLevel.VERBOSE
                "D" -> LogLevel.DEBUG
                "I" -> LogLevel.INFO
                "W" -> LogLevel.WARN
                "E" -> LogLevel.ERROR
                "F" -> LogLevel.ASSERT
                else -> null
            }?.let { return it }
        }
        val l = line.lowercase()
        return when {
            "fatal" in l || "error" in l || " err" in l || "fail" in l || "abort" in l -> LogLevel.ERROR
            "warn" in l -> LogLevel.WARN
            else -> LogLevel.INFO
        }
    }

    private companion object {
        const val MAX_LINES = 400
        val LOGCAT_LEVEL = Regex("^\\d{2}-\\d{2}\\s+\\S+\\s+\\d+\\s+\\d+\\s+([VDIWEF])\\s+")
    }
}
