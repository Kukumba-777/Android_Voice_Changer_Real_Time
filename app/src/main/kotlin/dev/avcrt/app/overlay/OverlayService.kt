package dev.avcrt.app.overlay

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.ComponentCallbacks
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.content.res.Configuration
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.LocaleList
import android.os.Looper
import android.provider.Settings
import android.view.Display
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.Toast
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.core.app.NotificationCompat
import androidx.core.graphics.drawable.toBitmap
import androidx.core.os.LocaleListCompat
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import dagger.hilt.android.AndroidEntryPoint
import dev.avcrt.app.R
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.userText
import dev.avcrt.domain.engine.EngineState
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.media.MediaFile
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.MediaRepository
import dev.avcrt.domain.media.Soundboard
import dev.avcrt.domain.media.SoundboardState
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.settings.SettingsRepository
import dev.avcrt.domain.usecase.ApplyPresetUseCase
import dev.avcrt.domain.usecase.ToggleEngineUseCase
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import timber.log.Timber
import kotlin.math.roundToInt
import javax.inject.Inject

/**
 * Hosts the floating quick-controls overlay (a draggable bubble that expands into a control card)
 * as a foreground service so it survives the app going to the background. All controls reuse the
 * existing use-cases, so the overlay drives the SAME engine/preset state as the Home screen.
 *
 * Started/stopped by the Home "floating window" toggle (see [start]/[stop]); also self-stops if the
 * overlay permission is missing.
 */
@AndroidEntryPoint
class OverlayService : Service() {

    @Inject lateinit var toggleEngine: ToggleEngineUseCase
    @Inject lateinit var applyPreset: ApplyPresetUseCase
    @Inject lateinit var presets: PresetRepository
    @Inject lateinit var engine: VoiceEnginePort
    @Inject lateinit var library: MediaRepository
    @Inject lateinit var soundboard: Soundboard
    @Inject lateinit var settings: SettingsRepository

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val lifecycleOwner = OverlayLifecycleOwner()
    private val main = Handler(Looper.getMainLooper())
    private val placement = OverlayPlacement()
    private lateinit var displayManager: DisplayManager
    private lateinit var windowContext: Context
    private lateinit var windowManager: WindowManager
    private var overlayView: ComposeView? = null
    private lateinit var params: WindowManager.LayoutParams
    private var monitoringDisplay = false
    private val bounds = mutableStateOf(OverlayBounds(0, 0, 0, 0))
    private val localeSetting = mutableStateOf<String?>(null)
    private val systemLocales = mutableStateOf(LocaleListCompat.getAdjustedDefault().toLanguageTags())
    private val syncWindow = Runnable { ensureOverlay() }
    private val windowConfig = object : ComponentCallbacks {
        override fun onConfigurationChanged(newConfig: Configuration) = scheduleWindowSync()
        override fun onLowMemory() = Unit
    }
    private val displayChanges = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
        override fun onDisplayChanged(displayId: Int) {
            if (displayId == Display.DEFAULT_DISPLAY) scheduleWindowSync()
        }
    }

    // Compose-observable state (the service mutates these; the overlay recomposes).
    private val engineOn = mutableStateOf(false)
    private val engineBusy = mutableStateOf(false)
    private val allPresets = mutableStateOf(listOf<Preset>())
    private val sounds = mutableStateOf(listOf<MediaFile>())
    private val playback = mutableStateOf<SoundboardState>(SoundboardState.Idle)
    private val activeId = mutableStateOf<String?>(null)
    private val expanded = mutableStateOf(false)
    private var playJob: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        // Move the owner to a valid (non-INITIALIZED) state up front, so an early bail below still
        // has a clean onDestroy() path.
        lifecycleOwner.onCreate()

        // No overlay permission → can't draw. Bail cleanly (the toggle should have requested it).
        if (!Settings.canDrawOverlays(this)) {
            Timber.tag(TAG).w("overlay permission missing — stopping")
            stopSelf()
            return
        }

        startAsForeground()
        displayManager = getSystemService(DisplayManager::class.java)
        val display = displayManager.getDisplay(Display.DEFAULT_DISPLAY)
        if (display == null) {
            stopSelf()
            return
        }
        windowContext = createDisplayContext(display).createWindowContext(OVERLAY_TYPE, null)
        windowManager = windowContext.getSystemService(WindowManager::class.java)
        windowContext.registerComponentCallbacks(windowConfig)
        displayManager.registerDisplayListener(displayChanges, main)
        monitoringDisplay = true

        localeSetting.value = settings.snapshot().locale.tag
        refreshSystemLocales()
        scope.launch {
            settings.observe().map { it.locale }.distinctUntilChanged().collect {
                localeSetting.value = it.tag
                refreshSystemLocales()
            }
        }

        scope.launch {
            engine.observeState().collect { st ->
                engineOn.value = st is EngineState.Running
            }
        }
        scope.launch { presets.observeAll().collect { allPresets.value = it } }
        scope.launch { presets.observeActive().collect { activeId.value = it } }
        scope.launch {
            library.observe(MediaFolder.SOUNDS).collect { files ->
                sounds.value = files
                val active = when (val state = soundboard.state.value) {
                    is SoundboardState.Preparing -> state.id
                    is SoundboardState.Playing -> state.id
                    SoundboardState.Idle -> null
                }
                if (active != null && files.none { it.id == active }) stopClip()
            }
        }
        scope.launch { soundboard.state.collect { playback.value = it } }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!monitoringDisplay) return START_NOT_STICKY
        ensureOverlay()
        return START_STICKY
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        scheduleWindowSync()
    }

    private fun scheduleWindowSync() {
        if (!monitoringDisplay) return
        main.removeCallbacks(syncWindow)
        main.post(syncWindow)
    }

    private fun refreshSystemLocales() {
        // Not Locale.getDefault(): Android can set it to AVCRT's per-app locale.
        systemLocales.value = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getSystemService(android.app.LocaleManager::class.java).systemLocales.toLanguageTags()
        } else {
            android.content.res.Resources.getSystem().configuration.locales.toLanguageTags()
        }
    }

    private fun ensureOverlay() {
        if (!Settings.canDrawOverlays(this)) {
            stopSelf()
            return
        }
        refreshSystemLocales()
        val metrics = windowManager.currentWindowMetrics
        val insets = metrics.windowInsets.getInsetsIgnoringVisibility(
            WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout(),
        )
        val safe = OverlayBounds(
            insets.left, insets.top,
            (metrics.bounds.width() - insets.right).coerceAtLeast(insets.left + 1),
            (metrics.bounds.height() - insets.bottom).coerceAtLeast(insets.top + 1),
        )
        bounds.value = safe
        val view = overlayView
        // Android may hide an attached overlay over protected windows. Leave it attached and let
        // the system show it again; only recover a window that actually went away.
        if (view == null) addOverlay() else moveWindow(view)
    }

    private fun addOverlay() {
        val (startX, startY) = positionFor(null)
        params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            OVERLAY_TYPE,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.LEFT
            // Offsets are display-relative; apply stable insets only in OverlayBounds, not twice.
            fitInsetsTypes = 0
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
            x = startX
            y = startY
        }

        val view = ComposeView(windowContext).apply {
            setViewTreeLifecycleOwner(lifecycleOwner)
            setViewTreeViewModelStoreOwner(lifecycleOwner)
            setViewTreeSavedStateRegistryOwner(lifecycleOwner)
            setContent {
                val config = LocalConfiguration.current
                val density = LocalDensity.current
                val tags = localeSetting.value ?: systemLocales.value
                val localConfig = remember(config, tags) {
                    Configuration(config).apply { setLocales(LocaleList.forLanguageTags(tags)) }
                }
                val localizedContext = remember(localConfig) { windowContext.createConfigurationContext(localConfig) }
                val icon = remember {
                    packageManager.getApplicationIcon(applicationInfo).toBitmap(144, 144).asImageBitmap()
                }
                val safe = bounds.value
                CompositionLocalProvider(LocalContext provides localizedContext, LocalConfiguration provides localConfig) {
                    OverlayRoot(
                        expanded = expanded.value,
                        engineOn = engineOn.value,
                        engineBusy = engineBusy.value,
                        presets = allPresets.value,
                        activeId = activeId.value,
                        sounds = sounds.value,
                        playback = playback.value,
                        appIcon = icon,
                        availableWidth = with(density) { safe.width.toDp() },
                        availableHeight = with(density) { safe.height.toDp() },
                        onDrag = ::onDrag,
                        onToggleExpand = ::toggleExpanded,
                        onToggleEngine = { on -> engineCommand { toggleEngine(on) } },
                        onPickPreset = { id -> engineCommand { applyPreset(id) } },
                        onPlay = ::playClip,
                        onStop = ::stopClip,
                    )
                }
            }
            // The bubble may be on an edge when it expands. Clamp using the measured card, not
            // just its top-left corner; do the window update outside the current layout pass.
            addOnLayoutChangeListener { v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom ->
                if (right - left != oldRight - oldLeft || bottom - top != oldBottom - oldTop) {
                    main.post { if (v === overlayView) moveWindow(v) }
                }
            }
            addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
                override fun onViewAttachedToWindow(v: View) = Unit
                override fun onViewDetachedFromWindow(v: View) {
                    if (v !== overlayView) return
                    Timber.tag(TAG).w("Overlay window detached; scheduling a new attachment")
                    overlayView = null
                    scheduleWindowSync()
                }
            })
        }
        try {
            windowManager.addView(view, params)
            overlayView = view
        } catch (e: WindowManager.BadTokenException) {
            Timber.tag(TAG).w(e, "Overlay window rejected")
        } catch (e: WindowManager.InvalidDisplayException) {
            Timber.tag(TAG).w(e, "Overlay display unavailable")
        }
    }

    private fun toggleExpanded() {
        if (!expanded.value) {
            placement.open()
            expanded.value = true
            scope.launch { library.refresh(MediaFolder.SOUNDS) }
        } else {
            expanded.value = false
            overlayView?.let(::moveWindow)
        }
    }

    private fun bubbleSizePx() = (windowContext.resources.displayMetrics.density * BubbleSize.value).roundToInt()

    private fun onDrag(dx: Float, dy: Float) {
        val v = overlayView ?: return
        val size = bubbleSizePx()
        val card = expanded.value
        placement.drag(bounds.value, size, if (card) v.width else size, if (card) v.height else size, card, dx, dy)
        moveWindow(v)
    }

    private fun positionFor(view: View?): Pair<Int, Int> {
        val size = bubbleSizePx()
        val card = expanded.value
        return placement.position(
            bounds.value, size,
            if (card) view?.width?.takeIf { it > 0 } ?: size else size,
            if (card) view?.height?.takeIf { it > 0 } ?: size else size,
            card,
        )
    }

    private fun moveWindow(view: View) {
        val (x, y) = positionFor(view)
        if (x == params.x && y == params.y) return
        params.x = x
        params.y = y
        try {
            windowManager.updateViewLayout(view, params)
        } catch (e: IllegalArgumentException) {
            Timber.tag(TAG).w(e, "Overlay layout could not be updated")
        }
    }

    private fun engineCommand(action: suspend () -> AvcrtResult<Unit>) {
        if (engineBusy.value) return
        engineBusy.value = true
        scope.launch {
            try { showFailure(action()) } finally { engineBusy.value = false }
        }
    }

    private fun playClip(id: String, clipVolumePercent: Int) {
        playJob?.cancel()
        playJob = scope.launch { showFailure(soundboard.play(id, clipVolumePercent)) }
    }

    private fun stopClip() {
        playJob?.cancel()
        scope.launch { showFailure(soundboard.stop()) }
    }

    private fun showFailure(result: AvcrtResult<Unit>) {
        if (result is AvcrtResult.Failure) {
            Timber.tag(TAG).w(result.error.cause, "%s", result.error.message)
            Toast.makeText(this, result.error.userText(this), Toast.LENGTH_SHORT).show()
        }
    }

    private fun startAsForeground() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.overlay_channel_name), NotificationManager.IMPORTANCE_MIN),
            )
        }
        val notif: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentTitle(getString(R.string.overlay_notification_title))
            .setContentText(getString(R.string.overlay_notification_text))
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    override fun onDestroy() {
        if (monitoringDisplay) {
            monitoringDisplay = false
            displayManager.unregisterDisplayListener(displayChanges)
            windowContext.unregisterComponentCallbacks(windowConfig)
        }
        main.removeCallbacksAndMessages(null)
        val view = overlayView
        overlayView = null
        view?.let { runCatching { windowManager.removeViewImmediate(it) } }
        lifecycleOwner.onDestroy()
        scope.cancel()
        if (playJob != null || soundboard.state.value != SoundboardState.Idle) {
            CoroutineScope(Dispatchers.Main.immediate).launch { soundboard.stop() }
        }
        super.onDestroy()
    }

    companion object {
        private const val TAG = "OverlayService"
        private const val CHANNEL_ID = "avcrt_overlay"
        private const val NOTIF_ID = 4711
        private const val OVERLAY_TYPE = WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY

        /** Starts the overlay. Caller must have verified [Settings.canDrawOverlays]. */
        fun start(context: Context) {
            context.startForegroundService(Intent(context, OverlayService::class.java))
        }

        /** Stops the overlay. A plain stopService is a no-op if the service isn't running, so this
         *  never spins the service up just to tear it down (which would hit the no-permission path). */
        fun stop(context: Context) {
            context.stopService(Intent(context, OverlayService::class.java))
        }
    }
}
