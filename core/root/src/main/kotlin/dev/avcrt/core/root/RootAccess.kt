package dev.avcrt.core.root

import dev.avcrt.core.common.result.AvcrtResult
import kotlinx.coroutines.flow.Flow

/**
 * Abstraction over the device’s root subsystem (KernelSU, Magisk, or generic
 * `su`). The HAL engine uses this for everything that needs elevated rights —
 * file writes under `/data/local/tmp/avcrt`, module installation, `setprop`,
 * etc.
 *
 * Implementations MUST never block the calling thread on the su shell init;
 * use coroutines or schedule on an IO dispatcher.
 */
interface RootAccess {

    /** Reactive status that the UI observes for the “root” chip. */
    val status: Flow<RootStatus>

    /** Cheap synchronous read of the last known status. */
    fun currentStatus(): RootStatus

    /** Asks the su provider to grant access, suspends until the user answers. */
    suspend fun requestAccess(): AvcrtResult<Unit>

    /** Runs a one-shot shell command as root and returns stdout. */
    suspend fun exec(command: String): AvcrtResult<String>

    /** Writes a file (UTF-8) under a path normally not writable by the app. */
    suspend fun writeFile(path: String, content: String, mode: String = "0644"): AvcrtResult<Unit>

    /** Reads a file the app cannot normally read. */
    suspend fun readFile(path: String): AvcrtResult<String>

    /** True if a given file exists from root’s perspective. */
    suspend fun fileExists(path: String): Boolean
}

sealed interface RootStatus {
    data object Unknown : RootStatus
    data object Unavailable : RootStatus
    data class Available(val provider: RootProvider, val granted: Boolean) : RootStatus
}

enum class RootProvider { KERNELSU, MAGISK, GENERIC_SU, UNKNOWN }
