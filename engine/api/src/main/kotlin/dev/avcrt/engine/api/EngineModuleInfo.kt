package dev.avcrt.engine.api

import kotlinx.serialization.Serializable

/**
 * Metadata written by the installer the first time the bundled module zip is
 * extracted onto the device. Lets the app know whether the installed module
 * matches the version it was shipped with and whether a reboot is still pending.
 */
@Serializable
data class EngineModuleInfo(
    val moduleId: String,
    val moduleVersion: String,
    val moduleVersionCode: Long,
    val installedAtMillis: Long,
    val requiresReboot: Boolean,
)
