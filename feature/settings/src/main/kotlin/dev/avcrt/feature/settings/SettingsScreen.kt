package dev.avcrt.feature.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavController
import dev.avcrt.core.root.RootProvider
import dev.avcrt.core.root.RootStatus
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import dev.avcrt.core.ui.components.SectionHeader
import dev.avcrt.core.ui.components.SettingsRow
import dev.avcrt.domain.settings.AppLocale
import dev.avcrt.domain.settings.ThemeMode

@Composable
fun SettingsScreen(
    navController: NavController,
    viewModel: SettingsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val context = LocalContext.current

    AvcrtScreen(title = stringResource(R.string.settings_title)) { _ ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(bottom = AvcrtSpacing.xxxl),
        ) {
            AppearanceSection(state, viewModel)
            LocaleSection(state, viewModel)
            EngineSection(state, viewModel)
            ExperimentalSection(state, viewModel)
            RootSection(state.rootStatus)
            AboutSection(context)
        }
    }
}

@Composable
private fun AppearanceSection(state: SettingsUiState, vm: SettingsViewModel) {
    SectionHeader(title = stringResource(R.string.settings_section_appearance))
    Card(
        modifier = Modifier
            .padding(horizontal = AvcrtSpacing.lg)
            .padding(bottom = AvcrtSpacing.sm),
    ) {
        Column {
            ThemeModeRows(current = state.settings.themeMode, onSelect = vm::onThemeMode)
            SettingsRow(
                title = stringResource(R.string.settings_dynamic_color_title),
                subtitle = stringResource(R.string.settings_dynamic_color_subtitle),
                trailing = {
                    Switch(
                        checked = state.settings.useDynamicColor,
                        onCheckedChange = vm::onDynamicColor,
                    )
                },
                onClick = { vm.onDynamicColor(!state.settings.useDynamicColor) },
            )
        }
    }
}

@Composable
private fun ThemeModeRows(current: ThemeMode, onSelect: (ThemeMode) -> Unit) {
    val options = listOf(
        ThemeMode.SYSTEM to stringResource(R.string.settings_theme_mode_system),
        ThemeMode.LIGHT to stringResource(R.string.settings_theme_mode_light),
        ThemeMode.DARK to stringResource(R.string.settings_theme_mode_dark),
    )
    options.forEach { (mode, label) ->
        SettingsRow(
            title = label,
            trailing = {
                RadioButton(
                    selected = mode == current,
                    onClick = { onSelect(mode) },
                )
            },
            onClick = { onSelect(mode) },
        )
    }
}

@Composable
private fun LocaleSection(state: SettingsUiState, vm: SettingsViewModel) {
    SectionHeader(title = stringResource(R.string.settings_section_locale))
    Card(
        modifier = Modifier
            .padding(horizontal = AvcrtSpacing.lg)
            .padding(bottom = AvcrtSpacing.sm),
    ) {
        Column {
            val options = listOf(
                AppLocale.SYSTEM to stringResource(R.string.settings_locale_system),
                AppLocale.EN to stringResource(R.string.settings_locale_en),
                AppLocale.RU to stringResource(R.string.settings_locale_ru),
            )
            options.forEach { (loc, label) ->
                SettingsRow(
                    title = label,
                    trailing = {
                        RadioButton(
                            selected = loc == state.settings.locale,
                            onClick = { vm.onLocale(loc) },
                        )
                    },
                    onClick = { vm.onLocale(loc) },
                )
            }
        }
    }
}

@Composable
private fun EngineSection(state: SettingsUiState, vm: SettingsViewModel) {
    SectionHeader(title = stringResource(R.string.settings_section_engine))
    Card(
        modifier = Modifier
            .padding(horizontal = AvcrtSpacing.lg)
            .padding(bottom = AvcrtSpacing.sm),
    ) {
        SettingsRow(
            title = stringResource(R.string.settings_engine_enabled_title),
            subtitle = stringResource(R.string.settings_engine_enabled_subtitle),
            trailing = {
                Switch(
                    checked = state.settings.autoEnableOnLaunch,
                    onCheckedChange = vm::onAutoEnableOnLaunch,
                )
            },
            onClick = { vm.onAutoEnableOnLaunch(!state.settings.autoEnableOnLaunch) },
        )
    }
}

@Composable
private fun ExperimentalSection(state: SettingsUiState, vm: SettingsViewModel) {
    SectionHeader(title = stringResource(R.string.settings_section_experimental))
    Card(
        modifier = Modifier
            .padding(horizontal = AvcrtSpacing.lg)
            .padding(bottom = AvcrtSpacing.sm),
    ) {
        SettingsRow(
            title = stringResource(R.string.settings_experimental_title),
            subtitle = stringResource(R.string.settings_experimental_subtitle),
            trailing = {
                Switch(
                    checked = state.settings.experimentalEnabled,
                    onCheckedChange = vm::onExperimentalEnabled,
                )
            },
            onClick = { vm.onExperimentalEnabled(!state.settings.experimentalEnabled) },
        )
    }
}

@Composable
private fun RootSection(status: RootStatus) {
    SectionHeader(title = stringResource(R.string.settings_section_root))
    Card(
        modifier = Modifier
            .padding(horizontal = AvcrtSpacing.lg)
            .padding(bottom = AvcrtSpacing.sm),
    ) {
        val label = when (status) {
            RootStatus.Unknown -> "—"
            RootStatus.Unavailable -> "—"
            is RootStatus.Available -> when (status.provider) {
                RootProvider.KERNELSU -> "KernelSU"
                RootProvider.MAGISK -> "Magisk"
                RootProvider.GENERIC_SU -> "su"
                RootProvider.UNKNOWN -> "Unknown"
            }
        }
        SettingsRow(
            title = stringResource(R.string.settings_root_title),
            subtitle = label,
        )
    }
}

@Composable
private fun AboutSection(context: android.content.Context) {
    SectionHeader(title = stringResource(R.string.settings_section_about))
    Card(
        modifier = Modifier
            .padding(horizontal = AvcrtSpacing.lg)
            .padding(bottom = AvcrtSpacing.sm),
    ) {
        val pkg = context.packageManager.getPackageInfo(context.packageName, 0)
        Column {
            SettingsRow(
                title = stringResource(R.string.settings_about_version),
                subtitle = pkg.versionName ?: "—",
            )
            SettingsRow(
                title = stringResource(R.string.settings_about_build),
                subtitle = pkg.longVersionCode.toString(),
            )
        }
    }
}
