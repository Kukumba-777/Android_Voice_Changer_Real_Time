package dev.avcrt.core.common.result

import android.content.Context
import androidx.annotation.StringRes
import dev.avcrt.core.common.R

/**
 * A discriminated result type used at module boundaries where Kotlin's built-in
 * [kotlin.Result] would obscure the failure mode (e.g. "module not installed"
 * vs "permission denied" vs "io error"). UI layers pattern-match on the sealed
 * cases to render the right state.
 */
sealed interface AvcrtResult<out T> {
    data class Success<T>(val value: T) : AvcrtResult<T>
    data class Failure(val error: AvcrtError) : AvcrtResult<Nothing>
}

data class UserMessage(@StringRes val resId: Int, val args: List<String> = emptyList()) {
    fun resolve(context: Context): String = context.getString(resId, *args.toTypedArray())
}

fun AvcrtError.userText(context: Context): String = userMessage.resolve(context)

sealed class AvcrtError(
    open val message: String,
    open val cause: Throwable? = null,
    open val userMessage: UserMessage = UserMessage(R.string.error_generic),
) {
    data class RootUnavailable(
        override val message: String = "Root is not available on this device",
    ) : AvcrtError(message, userMessage = UserMessage(R.string.error_root_unavailable))

    data class RootDenied(
        override val message: String = "Root access was denied",
    ) : AvcrtError(message, userMessage = UserMessage(R.string.error_root_denied))

    data class ModuleNotInstalled(
        override val message: String = "AVCRT KSU module is not installed",
    ) : AvcrtError(message, userMessage = UserMessage(R.string.error_module_missing))

    data class ModuleVersionMismatch(val expected: String, val actual: String) :
        AvcrtError(
            "Module version mismatch: expected $expected, got $actual",
            userMessage = UserMessage(R.string.error_module_version),
        )

    data class Io(
        override val message: String,
        override val cause: Throwable? = null,
        override val userMessage: UserMessage = UserMessage(R.string.error_operation_failed),
    ) : AvcrtError(message, cause, userMessage)

    data class Unknown(
        override val message: String,
        override val cause: Throwable? = null,
        override val userMessage: UserMessage = UserMessage(R.string.error_generic),
    ) : AvcrtError(message, cause, userMessage)
}

inline fun <T, R> AvcrtResult<T>.map(transform: (T) -> R): AvcrtResult<R> = when (this) {
    is AvcrtResult.Success -> AvcrtResult.Success(transform(value))
    is AvcrtResult.Failure -> this
}

inline fun <T> AvcrtResult<T>.onSuccess(action: (T) -> Unit): AvcrtResult<T> = also {
    if (it is AvcrtResult.Success) action(it.value)
}

inline fun <T> AvcrtResult<T>.onFailure(action: (AvcrtError) -> Unit): AvcrtResult<T> = also {
    if (it is AvcrtResult.Failure) action(it.error)
}
