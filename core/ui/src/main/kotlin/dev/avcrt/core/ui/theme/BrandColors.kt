package dev.avcrt.core.ui.theme

import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.ui.graphics.Color

/**
 * Brand fallback palette used when Material You dynamic color is disabled or
 * unavailable. Built from a single seed (#5B3FB1, a balanced violet that reads
 * well for both a “voice” theme and either system theme).
 *
 * Tone values follow the Material 3 token spec — do not introduce ad-hoc colors
 * elsewhere in the codebase; extend this file instead.
 */
internal object BrandColors {
    private val Primary = Color(0xFF5B3FB1)
    private val OnPrimary = Color(0xFFFFFFFF)
    private val PrimaryContainer = Color(0xFFE7DFFF)
    private val OnPrimaryContainer = Color(0xFF1A0A55)

    private val Secondary = Color(0xFF625B70)
    private val OnSecondary = Color(0xFFFFFFFF)
    private val SecondaryContainer = Color(0xFFE8DEF8)
    private val OnSecondaryContainer = Color(0xFF1E192B)

    private val Tertiary = Color(0xFF7E5260)
    private val OnTertiary = Color(0xFFFFFFFF)
    private val TertiaryContainer = Color(0xFFFFD8E4)
    private val OnTertiaryContainer = Color(0xFF31101D)

    private val Error = Color(0xFFB3261E)
    private val OnError = Color(0xFFFFFFFF)
    private val ErrorContainer = Color(0xFFF9DEDC)
    private val OnErrorContainer = Color(0xFF410E0B)

    private val Background = Color(0xFFFFFBFE)
    private val OnBackground = Color(0xFF1C1B1F)
    private val Surface = Color(0xFFFFFBFE)
    private val OnSurface = Color(0xFF1C1B1F)
    private val SurfaceVariant = Color(0xFFE7E0EC)
    private val OnSurfaceVariant = Color(0xFF49454F)
    private val Outline = Color(0xFF79747E)

    private val DarkPrimary = Color(0xFFCFBCFF)
    private val DarkOnPrimary = Color(0xFF2A1380)
    private val DarkPrimaryContainer = Color(0xFF422998)
    private val DarkOnPrimaryContainer = Color(0xFFE7DFFF)

    private val DarkSecondary = Color(0xFFCCC2DC)
    private val DarkOnSecondary = Color(0xFF332D41)
    private val DarkSecondaryContainer = Color(0xFF4A4458)
    private val DarkOnSecondaryContainer = Color(0xFFE8DEF8)

    private val DarkTertiary = Color(0xFFEFB8C8)
    private val DarkOnTertiary = Color(0xFF492532)
    private val DarkTertiaryContainer = Color(0xFF633B48)
    private val DarkOnTertiaryContainer = Color(0xFFFFD8E4)

    private val DarkBackground = Color(0xFF1C1B1F)
    private val DarkOnBackground = Color(0xFFE6E1E5)
    private val DarkSurface = Color(0xFF1C1B1F)
    private val DarkOnSurface = Color(0xFFE6E1E5)
    private val DarkSurfaceVariant = Color(0xFF49454F)
    private val DarkOnSurfaceVariant = Color(0xFFCAC4D0)
    private val DarkOutline = Color(0xFF938F99)

    val lightScheme = lightColorScheme(
        primary = Primary,
        onPrimary = OnPrimary,
        primaryContainer = PrimaryContainer,
        onPrimaryContainer = OnPrimaryContainer,
        secondary = Secondary,
        onSecondary = OnSecondary,
        secondaryContainer = SecondaryContainer,
        onSecondaryContainer = OnSecondaryContainer,
        tertiary = Tertiary,
        onTertiary = OnTertiary,
        tertiaryContainer = TertiaryContainer,
        onTertiaryContainer = OnTertiaryContainer,
        error = Error,
        onError = OnError,
        errorContainer = ErrorContainer,
        onErrorContainer = OnErrorContainer,
        background = Background,
        onBackground = OnBackground,
        surface = Surface,
        onSurface = OnSurface,
        surfaceVariant = SurfaceVariant,
        onSurfaceVariant = OnSurfaceVariant,
        outline = Outline,
    )

    val darkScheme = darkColorScheme(
        primary = DarkPrimary,
        onPrimary = DarkOnPrimary,
        primaryContainer = DarkPrimaryContainer,
        onPrimaryContainer = DarkOnPrimaryContainer,
        secondary = DarkSecondary,
        onSecondary = DarkOnSecondary,
        secondaryContainer = DarkSecondaryContainer,
        onSecondaryContainer = DarkOnSecondaryContainer,
        tertiary = DarkTertiary,
        onTertiary = DarkOnTertiary,
        tertiaryContainer = DarkTertiaryContainer,
        onTertiaryContainer = DarkOnTertiaryContainer,
        error = Color(0xFFF2B8B5),
        onError = Color(0xFF601410),
        errorContainer = Color(0xFF8C1D18),
        onErrorContainer = Color(0xFFF9DEDC),
        background = DarkBackground,
        onBackground = DarkOnBackground,
        surface = DarkSurface,
        onSurface = DarkOnSurface,
        surfaceVariant = DarkSurfaceVariant,
        onSurfaceVariant = DarkOnSurfaceVariant,
        outline = DarkOutline,
    )
}
