package dev.avcrt.engine.hal

import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootStatus
import dev.avcrt.engine.api.DiagnosticReport
import dev.avcrt.engine.api.DiagnosticSeverity
import dev.avcrt.engine.api.EngineDiagnostics
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.launch
import timber.log.Timber
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Pipes a single, automatic diagnostics run into the shared LogSink as soon as
 * root is granted. No user-visible button is needed — by the time the user
 * opens the Logs tab the report is already there.
 *
 * Re-runs are not scheduled here on purpose: the report is a snapshot and
 * keeping it on screen until the next launch is less surprising than logs
 * silently changing. Manual re-run can be added later as a Settings action.
 */
@Singleton
class EngineDiagnosticsAutoRunner @Inject constructor(
    private val diagnostics: EngineDiagnostics,
    private val rootAccess: RootAccess,
    private val dispatchers: DispatcherProvider,
) {
    private val scope = CoroutineScope(SupervisorJob() + dispatchers.io)

    fun start() {
        // Wait for the FIRST root-granted status, then fire one run.
        rootAccess.status
            .filter { it is RootStatus.Available && it.granted }
            .distinctUntilChanged()
            .take(1)
            .onEach { runOnce() }
            .launchIn(scope)
    }

    private fun runOnce() {
        scope.launch {
            Timber.tag(TAG).i("Auto-running engine diagnostics…")
            when (val result = diagnostics.run()) {
                is AvcrtResult.Success -> emit(result.value)
                is AvcrtResult.Failure -> Timber.tag(TAG).w("Diagnostics failed: %s", result.error.message)
            }
        }
    }

    private fun emit(report: DiagnosticReport) {
        Timber.tag(TAG).i("--- diagnostics report (%d items) ---", report.items.size)
        report.items.forEach { item ->
            val header = "[${item.severity.name}] ${item.label}"
            val body = "$header\n${item.detail}"
            when (item.severity) {
                DiagnosticSeverity.FAIL -> Timber.tag(TAG).e(body)
                DiagnosticSeverity.WARN -> Timber.tag(TAG).w(body)
                DiagnosticSeverity.OK   -> Timber.tag(TAG).i(body)
                DiagnosticSeverity.INFO -> Timber.tag(TAG).d(body)
            }
        }
        Timber.tag(TAG).i("--- end of report ---")
    }

    private companion object {
        const val TAG = "AvcrtDiag"
    }
}
