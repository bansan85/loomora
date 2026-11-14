package fr.bansan.loomora.probe

import android.app.UiModeManager
import android.content.Context
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/**
 * Theme of the Modern UI: the light or dark Material 3 scheme follows the system (API 24 and later
 * all have the dark setting). The text of every scheme has a contrast of at least 4.5:1 with its
 * background (Material 3 default text roles are above 12:1; the high contrast schemes are black on
 * white and white on black, 21:1).
 *
 * High contrast: Android 14 (API 34) added UiModeManager.getContrast(); that API is only called
 * behind a run-time version check. On API 24 to 33 there is no system contrast level: the standard
 * schemes are used and the feature is reported as disabled ("uimode-contrast" in the probe
 * record's disabled_features, fallback `android-uimode-contrast` in validation/fallbacks.json).
 */
internal object Contrast {
    const val MIN_API = 34
    const val FEATURE = "uimode-contrast"

    /** Comma-separated disabled features of the probe record on this system ("" if none). */
    fun disabledFeatures(): String = if (Build.VERSION.SDK_INT >= MIN_API) "" else FEATURE

    fun highContrast(context: Context): Boolean =
        Build.VERSION.SDK_INT >= MIN_API && levelOf(context) >= 0.5f

    @RequiresApi(MIN_API)
    private fun levelOf(context: Context): Float {
        val manager = context.getSystemService(UiModeManager::class.java)
        return manager?.contrast ?: 0f
    }
}

private val HighContrastLight: ColorScheme = lightColorScheme(
    primary = Color.Black,
    onPrimary = Color.White,
    background = Color.White,
    onBackground = Color.Black,
    surface = Color.White,
    onSurface = Color.Black,
    onSurfaceVariant = Color.Black,
    outline = Color.Black,
    outlineVariant = Color.Black,
)

private val HighContrastDark: ColorScheme = darkColorScheme(
    primary = Color.White,
    onPrimary = Color.Black,
    background = Color.Black,
    onBackground = Color.White,
    surface = Color.Black,
    onSurface = Color.White,
    onSurfaceVariant = Color.White,
    outline = Color.White,
    outlineVariant = Color.White,
)

@Composable
internal fun LoomoraTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val high = Contrast.highContrast(LocalContext.current)
    val scheme = when {
        high && dark -> HighContrastDark
        high -> HighContrastLight
        dark -> darkColorScheme()
        else -> lightColorScheme()
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
