package dev.avcrt.domain.media

import dev.avcrt.core.common.result.AvcrtResult
import java.io.InputStream
import java.io.OutputStream

interface UserBackup {
    suspend fun export(open: () -> OutputStream): AvcrtResult<Unit>
    suspend fun restore(open: () -> InputStream): AvcrtResult<Unit>
}
