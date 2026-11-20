package dev.avcrt.feature.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.InstallMobile
import androidx.compose.material.icons.rounded.RestartAlt
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavController
import dev.avcrt.core.root.RootProvider
import dev.avcrt.core.root.RootStatus
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import dev.avcrt.domain.engine.EngineState

@Composable
fun HomeScreen(
    navController: NavController,
    viewModel: HomeViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current

    // Returning from the "draw over other apps" settings screen: if granted, enable the window.
    val overlayPermLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        if (Settings.canDrawOverlays(context)) viewModel.onSetFloatingWindow(true)
    }

    val onToggleFloating: (Boolean) -> Unit = { want ->
        when {
            !want -> viewModel.onSetFloatingWindow(false)
            Settings.canDrawOverlays(context) -> viewModel.onSetFloatingWindow(true)
            else -> overlayPermLauncher.launch(
                Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:${context.packageName}"),
                ),
            )
        }
    }

    LaunchedEffect(state.transientMessage) {
        state.transientMessage?.let {
            snackbar.showSnackbar(it)
            viewModel.consumeTransient()
        }
    }

    AvcrtScreen(title = stringResource(R.string.home_title)) { _ ->
        Box(Modifier.fillMaxSize()) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = AvcrtSpacing.lg),
                verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.lg),
            ) {
                MasterSwitchCard(state, viewModel)
                ActivePresetCard(state)
                FloatingWindowCard(state.floatingWindowEnabled, onToggleFloating)
                ModuleStatusCard(state, viewModel)
                RootStatusCard(state, viewModel)
                Spacer(Modifier.height(AvcrtSpacing.xxl))
            }
            SnackbarHost(hostState = snackbar, modifier = Modifier.align(Alignment.BottomCenter))
        }
    }
}

@Composable
private fun FloatingWindowCard(enabled: Boolean, onToggle: (Boolean) -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(AvcrtSpacing.lg),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.home_floating_window_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = stringResource(R.string.home_floating_window_desc),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Box(
                modifier = Modifier.size(width = 52.dp, height = 48.dp),
                contentAlignment = Alignment.Center,
            ) {
                Switch(checked = enabled, onCheckedChange = onToggle)
            }
        }
    }
}

@Composable
private fun MasterSwitchCard(state: HomeUiState, vm: HomeViewModel) {
    val running = state.engine is EngineState.Running
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (running) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(AvcrtSpacing.lg),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.home_master_switch_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = stringResource(
                        if (running) R.string.home_master_switch_on
                        else R.string.home_master_switch_off,
                    ),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // Fixed-size slot so the card doesn't jump when the Switch (which
            // reserves a 48dp touch target) swaps to the smaller spinner.
            Box(
                modifier = Modifier.size(width = 52.dp, height = 48.dp),
                contentAlignment = Alignment.Center,
            ) {
                if (state.isToggling) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(24.dp),
                        strokeWidth = 2.5.dp,
                    )
                } else {
                    val canToggle = state.engine is EngineState.Running ||
                        state.engine is EngineState.ReadyDisabled
                    Switch(
                        checked = running,
                        enabled = canToggle,
                        onCheckedChange = { vm.onToggleEngine(it) },
                    )
                }
            }
        }
    }
}

@Composable
private fun ActivePresetCard(state: HomeUiState) {
    val activeName = when (val s = state.engine) {
        is EngineState.Running -> s.activePreset.displayName(localeTag = currentLocaleTag())
        is EngineState.ReadyDisabled -> s.activePreset?.displayName(localeTag = currentLocaleTag())
        else -> null
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(AvcrtSpacing.lg),
        ) {
            Text(
                text = stringResource(R.string.home_active_preset_label),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(AvcrtSpacing.xs))
            Text(
                text = activeName ?: stringResource(R.string.home_no_preset),
                style = MaterialTheme.typography.titleLarge,
            )
        }
    }
}

@Composable
private fun ModuleStatusCard(state: HomeUiState, vm: HomeViewModel) {
    val isReboot = state.engine is EngineState.RebootRequired
    val offerRecovery = state.engine is EngineState.ModuleNotStartedWithZygisk
    val (label, showInstall) = when (val engine = state.engine) {
        EngineState.ModuleNotInstalled -> stringResource(R.string.home_module_install) to true
        EngineState.RebootRequired -> stringResource(R.string.home_module_needs_reboot) to false
        EngineState.ModuleNotStartedWithZygisk -> {
            val hint = when ((state.rootStatus as? RootStatus.Available)?.provider) {
                RootProvider.MAGISK -> R.string.home_zygisk_hint_magisk
                RootProvider.KERNELSU -> R.string.home_zygisk_hint_kernelsu
                else -> R.string.home_zygisk_hint_generic
            }
            stringResource(R.string.home_module_not_started_zygisk, stringResource(hint)) to true
        }
        is EngineState.Error -> engine.reason to false
        else -> stringResource(R.string.home_module_installed) to false
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(AvcrtSpacing.lg),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.md),
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.weight(1f),
            )
            // Fixed-height action slot so the card keeps a stable height as it
            // swaps between the install/reboot button, the spinner and nothing.
            Box(
                modifier = Modifier.height(40.dp),
                contentAlignment = Alignment.CenterEnd,
            ) {
                when {
                    state.isInstallingModule -> CircularProgressIndicator(
                        modifier = Modifier.size(24.dp),
                        strokeWidth = 2.5.dp,
                    )
                    showInstall -> CompactAction(
                        icon = Icons.Rounded.InstallMobile,
                        text = stringResource(if (offerRecovery) R.string.home_module_install_cellular else R.string.home_module_install),
                        onClick = { vm.onInstallModule(withoutZygisk = offerRecovery) },
                    )
                    isReboot -> CompactAction(
                        icon = Icons.Rounded.RestartAlt,
                        text = stringResource(R.string.home_reboot_action),
                        onClick = vm::onReboot,
                    )
                }
            }
        }
    }
}

/** Compact icon + label text button used for the secondary card actions. */
@Composable
private fun CompactAction(icon: ImageVector, text: String, onClick: () -> Unit) {
    TextButton(
        onClick = onClick,
        contentPadding = PaddingValues(
            horizontal = AvcrtSpacing.md,
            vertical = AvcrtSpacing.xs,
        ),
    ) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(18.dp))
        Spacer(Modifier.width(AvcrtSpacing.xs))
        Text(text)
    }
}

@Composable
private fun RootStatusCard(state: HomeUiState, vm: HomeViewModel) {
    val (label, showRequest) = when (val r = state.rootStatus) {
        RootStatus.Unknown -> stringResource(R.string.home_root_status_unknown) to false
        RootStatus.Unavailable -> stringResource(R.string.home_root_status_unavailable) to false
        is RootStatus.Available -> {
            val text = if (!r.granted) {
                stringResource(R.string.home_root_status_denied)
            } else when (r.provider) {
                RootProvider.KERNELSU -> stringResource(R.string.home_root_status_granted_ksu)
                RootProvider.MAGISK -> stringResource(R.string.home_root_status_granted_magisk)
                else -> stringResource(R.string.home_root_status_granted_generic)
            }
            text to !r.granted
        }
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(AvcrtSpacing.lg),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.home_root_status_label),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(text = label, style = MaterialTheme.typography.bodyLarge)
            }
            if (showRequest) {
                TextButton(onClick = vm::onRequestRoot) {
                    Text(stringResource(R.string.home_root_request))
                }
            }
        }
    }
}

@Composable
private fun currentLocaleTag(): String {
    val context = androidx.compose.ui.platform.LocalContext.current
    val locales = context.resources.configuration.locales
    return if (locales.isEmpty) "en" else locales[0].language.ifBlank { "en" }
}
