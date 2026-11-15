package dev.avcrt.feature.converter

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.composable

object ConverterRoute {
    const val path = "converter"
}

fun NavGraphBuilder.converterGraph(navController: NavController) {
    composable(ConverterRoute.path) {
        ConverterScreen()
    }
}
