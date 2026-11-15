package dev.avcrt.core.root

import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.io.SuFile
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import timber.log.Timber
import javax.inject.Inject
import javax.inject.Singleton

/**
 * libsu-backed [RootAccess]. libsu transparently supports KernelSU, Magisk and
 * classic `su` — we only have to probe which provider is actually present so
 * the UI can show the right label and so we can choose the right install
 * strategy for the bundled module.
 */
@Singleton
class LibsuRootAccess @Inject constructor(
    private val dispatchers: DispatcherProvider,
) : RootAccess {

    private val _status = MutableStateFlow<RootStatus>(RootStatus.Unknown)
    override val status: StateFlow<RootStatus> = _status.asStateFlow()

    init {
        Shell.enableVerboseLogging = false
        Shell.setDefaultBuilder(
            Shell.Builder.create()
                .setFlags(Shell.FLAG_MOUNT_MASTER)
                .setTimeout(SHELL_TIMEOUT_SECONDS),
        )
    }

    override fun currentStatus(): RootStatus = _status.value

    override suspend fun requestAccess(): AvcrtResult<Unit> = withContext(dispatchers.io) {
        try {
            val shell = Shell.getShell()
            val provider = detectProvider()
            val granted = shell.isRoot
            _status.value = if (provider == null) {
                RootStatus.Unavailable
            } else {
                RootStatus.Available(provider, granted)
            }
            if (granted) AvcrtResult.Success(Unit)
            else AvcrtResult.Failure(AvcrtError.RootDenied())
        } catch (t: Throwable) {
            Timber.e(t, "Failed to acquire root shell")
            _status.value = RootStatus.Unavailable
            AvcrtResult.Failure(AvcrtError.RootUnavailable())
        }
    }

    override suspend fun exec(command: String): AvcrtResult<String> = withContext(dispatchers.io) {
        runCatching {
            val result = Shell.cmd(command).exec()
            if (result.isSuccess) AvcrtResult.Success(result.out.joinToString("\n"))
            else AvcrtResult.Failure(
                AvcrtError.Io(
                    message = "Command failed (code=${result.code}): " +
                        result.err.joinToString("\n"),
                ),
            )
        }.getOrElse { AvcrtResult.Failure(AvcrtError.Io("Command threw", it)) }
    }

    override suspend fun writeFile(path: String, content: String, mode: String): AvcrtResult<Unit> =
        withContext(dispatchers.io) {
            runCatching {
                SuFile(path).parentFile?.let { parent ->
                    if (!parent.exists()) parent.mkdirs()
                }
                SuFile(path).newOutputStream().use { out ->
                    out.write(content.toByteArray(Charsets.UTF_8))
                }
                Shell.cmd("chmod $mode '$path'").exec()
                AvcrtResult.Success(Unit)
            }.getOrElse {
                AvcrtResult.Failure(AvcrtError.Io("Failed to write $path", it))
            }
        }

    override suspend fun readFile(path: String): AvcrtResult<String> = withContext(dispatchers.io) {
        runCatching {
            val text = SuFile(path).newInputStream().use { it.bufferedReader().readText() }
            AvcrtResult.Success(text)
        }.getOrElse { AvcrtResult.Failure(AvcrtError.Io("Failed to read $path", it)) }
    }

    override suspend fun fileExists(path: String): Boolean = withContext(dispatchers.io) {
        runCatching { SuFile(path).exists() }.getOrDefault(false)
    }

    private fun detectProvider(): RootProvider? = when {
        SuFile("/data/adb/ksu").exists() ||
            SuFile("/data/adb/ksud").exists() -> RootProvider.KERNELSU
        SuFile("/data/adb/magisk").exists() -> RootProvider.MAGISK
        Shell.cmd("which su").exec().isSuccess -> RootProvider.GENERIC_SU
        else -> null
    }

    companion object {
        private const val SHELL_TIMEOUT_SECONDS = 20L
    }
}
