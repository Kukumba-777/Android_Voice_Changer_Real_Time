package dev.avcrt.domain.log

import dev.avcrt.core.common.logging.LogEntry
import dev.avcrt.core.common.result.AvcrtResult

/**
 * Log streams that live OUTSIDE the app process and require root to read:
 *   - [DAEMON] — the cellular daemon's file log (avcrtd via service.sh)
 *   - [ZYGISK] — the app-capture hook's logcat tags (avcrtzy + avcrtfx)
 *   - [KERNEL] — AVCRT lines from the kernel ring buffer (dmesg)
 *
 * The in-app Timber log is NOT here — it's served live by [LogRepository].
 */
enum class ExternalLogSource { DAEMON, ZYGISK, KERNEL }

interface ExternalLogReader {
    /**
     * Pulls a bounded, recent snapshot of [source] (oldest-first), parsed into
     * [LogEntry]s. A one-shot root read — callers re-invoke to refresh. Returns
     * a Failure (e.g. root unavailable/denied) the UI can surface verbatim.
     */
    suspend fun read(source: ExternalLogSource): AvcrtResult<List<LogEntry>>
}
