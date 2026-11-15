package dev.avcrt.engine.api

import dev.avcrt.core.common.result.AvcrtResult

/**
 * Runs a bag of "did the module actually land where we expected?" checks and
 * returns a flat report the UI can show, share, and log.
 *
 * Each check captures both a binary verdict (`ok`) and the raw evidence
 * (`detail`) so the user can paste the lot into a bug report.
 */
interface EngineDiagnostics {
    suspend fun run(): AvcrtResult<DiagnosticReport>
}

data class DiagnosticReport(
    val collectedAtMillis: Long,
    val items: List<DiagnosticItem>,
) {
    val anyFailing: Boolean = items.any { it.severity == DiagnosticSeverity.FAIL }
}

data class DiagnosticItem(
    val label: String,
    val detail: String,
    val severity: DiagnosticSeverity,
)

enum class DiagnosticSeverity { OK, WARN, FAIL, INFO }
