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
 *   4. The install script records the current boot ID for [ModuleRuntime].
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

    suspend fun installFromAssets(withoutZygisk: Boolean): AvcrtResult<Unit> = withContext(dispatchers.io) {
        runCatching {
            val provider = (rootAccess.currentStatus() as? RootStatus.Available)?.provider
                ?: return@runCatching AvcrtResult.Failure(AvcrtError.RootUnavailable())

            val installer = when (provider) {
                RootProvider.KERNELSU -> "${RootProvider.KSU_COMMAND}; \"\$ksu\" module install"
                RootProvider.MAGISK -> "magisk --install-module"
                RootProvider.GENERIC_SU,
                RootProvider.UNKNOWN -> return@runCatching AvcrtResult.Failure(
                    AvcrtError.Unknown(
                        "Generic su without KernelSU/Magisk can't host the audio HAL module",
                    ),
                )
            }
            val asset = if (withoutZygisk) CELLULAR_ZIP_FILE else MODULE_ZIP_FILE
            val zip = extractBundledZip(asset) ?: return@runCatching AvcrtResult.Failure(
                AvcrtError.Io("Bundled module zip is missing — rebuild :ksu-module first"),
            )
            val installCmd = "$installer '${zip.absolutePath.replace("'", "'\\''")}'"

            when (val res = rootAccess.exec(installCmd)) {
                is AvcrtResult.Failure -> res
                is AvcrtResult.Success -> {
                    Timber.i("Module install command output:\n%s", res.value)
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
            RootProvider.KERNELSU -> "${RootProvider.KSU_COMMAND}; \"\$ksu\" module uninstall ${AvcrtPaths.MODULE_ID}"
            RootProvider.MAGISK -> "test -f '${AvcrtPaths.MODULE_DIR}/module.prop' && touch '${AvcrtPaths.MODULE_DIR}/remove'"
            RootProvider.GENERIC_SU, RootProvider.UNKNOWN -> return@withContext AvcrtResult.Failure(
                AvcrtError.Io("No supported module manager detected"),
            )
        }
        rootAccess.exec(cmd).let {
            when (it) {
                is AvcrtResult.Failure -> AvcrtResult.Failure(it.error)
                is AvcrtResult.Success -> {
                    rootAccess.exec("rm -f '${AvcrtPaths.MODULE_MANIFEST_FILE}'")
                    AvcrtResult.Success(null)
                }
            }
        }
    }

    private fun extractBundledZip(asset: String): File? {
        val out = File(context.cacheDir, asset)
        return runCatching {
            context.assets.open(asset).use { input ->
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
        const val MODULE_ZIP_FILE = "avcrt-module.zip"
        const val CELLULAR_ZIP_FILE = "avcrt-module-cellular.zip"
    }
}
