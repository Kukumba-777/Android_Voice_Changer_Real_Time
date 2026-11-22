package dev.avcrt.domain.engine

import dev.avcrt.domain.preset.Preset

/**
 * Live status of the system-wide voice-change pipeline. Maps 1-1 to what the
 * Home screen renders.
 */
sealed interface EngineState {

    enum class ErrorReason {
        MODULE_DISABLED,
        SERVICE_NOT_STARTED,
        EARLY_BOOT_ONLY,
        SERVICE_NOT_RUNNING,
        STATUS_UNKNOWN,
    }

    /** Initial state before any probe runs. */
    data object Idle : EngineState

    /** KSU module has not been installed yet — UI offers an Install button. */
    data object ModuleNotInstalled : EngineState

    /** Module is installed but a reboot is needed for the audio HAL to pick it up. */
    data object RebootRequired : EngineState

    /** The boot completed without a service start, and the installed package contains Zygisk. */
    data object ModuleNotStartedWithZygisk : EngineState

    /** Module installed, configured, and ready — but the master switch is off. */
    data class ReadyDisabled(val activePreset: Preset?) : EngineState

    /** Engine is running, applying [activePreset] across the system audio path. */
    data class Running(val activePreset: Preset) : EngineState

    /** Something went wrong; the UI localizes [reason]. */
    data class Error(val reason: ErrorReason) : EngineState
}
