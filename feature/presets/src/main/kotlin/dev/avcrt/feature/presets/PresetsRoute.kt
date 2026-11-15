package dev.avcrt.feature.presets

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.NavType
import androidx.navigation.compose.composable
import androidx.navigation.navArgument

object PresetsRoute {
    const val path = "presets"
}

object PresetEditorRoute {
    const val ARG_PRESET_ID = "presetId"

    /** Route pattern with an optional presetId query arg (absent = create new). */
    const val path = "presets/editor?$ARG_PRESET_ID={$ARG_PRESET_ID}"

    /** Open the editor to create a brand-new custom preset. */
    fun create(): String = "presets/editor"

    /** Open the editor to modify an existing custom preset. */
    fun edit(presetId: String): String = "presets/editor?$ARG_PRESET_ID=$presetId"
}

fun NavGraphBuilder.presetsGraph(navController: NavController) {
    composable(PresetsRoute.path) {
        PresetsScreen(navController = navController)
    }
    composable(
        route = PresetEditorRoute.path,
        arguments = listOf(
            navArgument(PresetEditorRoute.ARG_PRESET_ID) {
                type = NavType.StringType
                nullable = true
                defaultValue = null
            },
        ),
    ) {
        PresetEditorScreen(navController = navController)
    }
}
