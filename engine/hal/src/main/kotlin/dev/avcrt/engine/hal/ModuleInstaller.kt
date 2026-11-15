package dev.avcrt.engine.hal

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootProvider
import dev.avcrt.core.root.RootStatus
import dev.avcrt.engine.api.EngineModuleInfo
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import timber.log.Timber
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Installs / uninstalls the bundled KSU module zip and tracks whether a reboot
 * is still required for the audio HAL effect to be picked up.
 *
 * Install flow:
 *   1. extract `avcrt-module.zip` from APK assets into the app's cache dir
 *   2. run the root provider's module installer:
 *        - KernelSU  → `ksud module install <zip>`
 *        - Magisk    → `magisk --install-module <zip>`
 *   3. write a manifest under /data/local/tmp/avcrt with the version we shipped
 *      (best-effort, used for diagnostics + version diffs)
 *   4. **drop the "install pending" marker file at [AvcrtPaths.INSTALL_PENDING_MARKER]**
 *      — this is what [requiresReboot] checks. The KSU module's `post-fs-data.sh`
 *      removes the marker on every boot, so its presence is a hard guarantee
 *      that the device hasn't booted since the install. Surviving a reboot
 *      would silently clear it.
 *
 * Uninstall delegates to `ksud module uninstall <id>` or removes the magisk
 * module directory + `.remove` flag.
 */
@Singleton
class ModuleInstaller @Inject constructor(
    @ApplicationContext private val context: Context,
    private val rootAccess: RootAccess,
    private val dispatchers: DispatcherProvider,
) {
    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    suspend fun isInstalled(): Boolean = withContext(dispatchers.io) {
        rootAccess.fileExists("$MODULES_DIR/${AvcrtPaths.MODULE_ID}/module.prop") ||
            rootAccess.fileExists("$MODULES_UPDATE_DIR/${AvcrtPaths.MODULE_ID}/module.prop")
    }

    /**
     * True iff the install-pending marker is present on disk. The marker is
     * dropped by [installFromAssets] and cleared by the module's
     * `post-fs-data.sh` on every boot — so this resolves the user's question
     * "have I rebooted since install?" without depending on uptime math or
     * JSON parsing.
     */
    suspend fun requiresReboot(): Boolean = withContext(dispatchers.io) {
        rootAccess.fileExists(AvcrtPaths.INSTALL_PENDING_MARKER)
    }

    suspend fun installFromAssets(): AvcrtResult<Unit> = withContext(dispatchers.io) {
        runCatching {
            val zip = extractBundledZip() ?: return@runCatching AvcrtResult.Failure(
                AvcrtError.Io("Bundled module zip is missing — rebuild :ksu-module first"),
            )
            val provider = (rootAccess.currentStatus() as? RootStatus.Available)?.provider
                ?: return@runCatching AvcrtResult.Failure(AvcrtError.RootUnavailable())

            val installCmd = when (provider) {
                RootProvider.KERNELSU -> "ksud module install '${zip.absolutePath}'"
                RootProvider.MAGISK -> "magisk --install-module '${zip.absolutePath}'"
                RootProvider.GENERIC_SU,
                RootProvider.UNKNOWN -> return@runCatching AvcrtResult.Failure(
                    AvcrtError.Unknown(
                        "Generic su without KernelSU/Magisk can't host the audio HAL module",
                    ),
                )
            }

            when (val res = rootAccess.exec(installCmd)) {
                is AvcrtResult.Failure -> res
                is AvcrtResult.Success -> {
                    Timber.i("Module install command output:\n%s", res.value)
                    // Mark "reboot pending" via the marker file; the module's
                    // post-fs-data.sh deletes it on next boot. Touching it via
                    // shell guarantees it lands in /data/adb with the right
                    // SELinux context (no need to mkdir parents).
                    rootAccess.exec("touch '${AvcrtPaths.INSTALL_PENDING_MARKER}'")
                    // Best-effort manifest write — used only for diagnostics
                    // (e.g. "what version is installed?"). The reboot check
                    // does NOT depend on this file.
                    writeManifest(
                        EngineModuleInfo(
                            moduleId = AvcrtPaths.MODULE_ID,
                            moduleVersion = BundledModuleMeta.VERSION,
                            moduleVersionCode = BundledModuleMeta.VERSION_CODE,
                            installedAtMillis = System.currentTimeMillis(),
                            requiresReboot = true,
                        ),
                    )
                    AvcrtResult.Success(Unit)
                }
            }
        }.getOrElse { AvcrtResult.Failure(AvcrtError.Unknown("Install failed", it)) }
    }

    suspend fun uninstall(): AvcrtResult<EngineModuleInfo?> = withContext(dispatchers.io) {
        val provider = (rootAccess.currentStatus() as? RootStatus.Available)?.provider
            ?: return@withContext AvcrtResult.Failure(AvcrtError.RootUnavailable())
        val cmd = when (provider) {
            RootProvider.KERNELSU -> "ksud module uninstall ${AvcrtPaths.MODULE_ID}"
            RootProvider.MAGISK -> "touch '$MODULES_DIR/${AvcrtPaths.MODULE_ID}/remove'"
            RootProvider.GENERIC_SU, RootProvider.UNKNOWN -> "rm -rf '$MODULES_DIR/${AvcrtPaths.MODULE_ID}'"
        }
        rootAccess.exec(cmd).let {
            when (it) {
                is AvcrtResult.Failure -> AvcrtResult.Failure(it.error)
                is AvcrtResult.Success -> {
                    rootAccess.exec("rm -f '${AvcrtPaths.MODULE_MANIFEST_FILE}'")
                    // Uninstall also requires a reboot to actually unmount the
                    // overlay — re-arm the marker so the UI tells the user so.
                    rootAccess.exec("touch '${AvcrtPaths.INSTALL_PENDING_MARKER}'")
                    AvcrtResult.Success(null)
                }
            }
        }
    }

    private fun extractBundledZip(): File? {
        val out = File(context.cacheDir, MODULE_ZIP_FILE)
        return runCatching {
            context.assets.open(MODULE_ZIP_FILE).use { input ->
                out.outputStream().use { input.copyTo(it) }
            }
            out
        }.onFailure {
            Timber.e(it, "Failed to extract bundled module zip")
        }.getOrNull()
    }

    /** Diagnostic-only: returns the persisted manifest if present. */
    suspend fun readManifest(): EngineModuleInfo? = withContext(dispatchers.io) {
        val read = rootAccess.readFile(AvcrtPaths.MODULE_MANIFEST_FILE)
        when (read) {
            is AvcrtResult.Failure -> null
            is AvcrtResult.Success -> runCatching {
                json.decodeFromString(EngineModuleInfo.serializer(), read.value)
            }.getOrNull()
        }
    }

    private suspend fun writeManifest(info: EngineModuleInfo) {
        val payload = json.encodeToString(EngineModuleInfo.serializer(), info)
        rootAccess.writeFile(AvcrtPaths.MODULE_MANIFEST_FILE, payload, "0644")
    }

    private companion object {
        const val MODULES_DIR = "/data/adb/modules"
        const val MODULES_UPDATE_DIR = "/data/adb/modules_update"
        const val MODULE_ZIP_FILE = "avcrt-module.zip"
    }
}
