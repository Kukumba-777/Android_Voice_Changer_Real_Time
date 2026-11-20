package dev.avcrt.engine.hal

import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.root.RootAccess
import javax.inject.Inject

class ModuleRuntime @Inject constructor(private val root: RootAccess) {
    enum class State {
        MISSING, REBOOT, DISABLED, NOT_STARTED, NOT_STARTED_WITH_ZYGISK,
        EARLY_BOOT_ONLY, SERVICE_SEEN, STARTED, UNKNOWN,
    }

    suspend fun inspect(): State {
        val dir = AvcrtPaths.MODULE_DIR
        val result = root.exec("""
            [ "${'$'}(id -u)" = 0 ] || exit 1
            boot=${'$'}(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
            if [ -f '${AvcrtPaths.STAGED_MODULE_DIR}/module.prop' ] || [ -e '$dir/remove' ]; then
                echo REBOOT
            elif [ ! -f '$dir/module.prop' ]; then
                echo MISSING
            elif [ -e '$dir/disable' ]; then
                echo DISABLED
            elif [ -z "${'$'}boot" ]; then
                echo UNKNOWN
            elif [ "${'$'}(cat '${AvcrtPaths.INSTALL_PENDING_MARKER}' 2>/dev/null)" = "${'$'}boot" ]; then
                echo REBOOT
            elif pidof avcrtd >/dev/null 2>&1; then
                echo STARTED
            elif [ "${'$'}(cat '${AvcrtPaths.SERVICE_BOOT_FILE}' 2>/dev/null)" = "${'$'}boot" ]; then
                echo SERVICE_SEEN
            elif [ "${'$'}(getprop sys.boot_completed)" != 1 ]; then
                echo UNKNOWN
            elif [ "${'$'}(cat '${AvcrtPaths.EARLY_BOOT_FILE}' 2>/dev/null)" = "${'$'}boot" ]; then
                echo EARLY_BOOT_ONLY
            elif [ -d '$dir/zygisk' ]; then
                echo NOT_STARTED_WITH_ZYGISK
            else
                echo NOT_STARTED
            fi
        """.trimIndent()) as? AvcrtResult.Success ?: return State.UNKNOWN
        return State.entries.firstOrNull { it.name == result.value.trim() } ?: State.UNKNOWN
    }
}
