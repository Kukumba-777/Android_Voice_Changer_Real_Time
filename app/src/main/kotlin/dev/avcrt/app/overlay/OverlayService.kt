package dev.avcrt.app.overlay

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.view.Gravity
import android.view.WindowManager
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.platform.ComposeView
import androidx.core.app.NotificationCompat
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import dagger.hilt.android.AndroidEntryPoint
import dev.avcrt.domain.engine.EngineState
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetOrigin
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.usecase.ApplyPresetUseCase
import dev.avcrt.domain.usecase.ToggleEngineUseCase
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import timber.log.Timber
import java.util.Locale
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

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val lifecycleOwner = OverlayLifecycleOwner()
    private lateinit var windowManager: WindowManager
    private var overlayView: ComposeView? = null
    private lateinit var params: WindowManager.LayoutParams

    // Compose-observable state (the service mutates these; the overlay recomposes).
    private val engineOn = mutableStateOf(false)
    private val allPresets = mutableStateOf(listOf<Preset>())
    private val activeId = mutableStateOf<String?>(null)
    private val expanded = mutableStateOf(false)
    private val category = mutableStateOf(PresetOrigin.BUILT_IN)

    private val localeTag: String get() = Locale.getDefault().language

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(Context.WINDOW_SERVICE) as WindowManager
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
        addOverlay()

        scope.launch {
            engine.observeState().collect { st ->
                engineOn.value = st is EngineState.Running
            }
        }
        scope.launch { presets.observeAll().collect { allPresets.value = it } }
        scope.launch { presets.observeActive().collect { activeId.value = it } }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    private fun addOverlay() {
        @Suppress("DEPRECATION")
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            WindowManager.LayoutParams.TYPE_PHONE
        }
        params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = (resources.displayMetrics.widthPixels - resources.displayMetrics.density * 70).toInt()
            y = (resources.displayMetrics.heightPixels * 0.30f).toInt()
        }

        val view = ComposeView(this).apply {
            setViewTreeLifecycleOwner(lifecycleOwner)
            setViewTreeViewModelStoreOwner(lifecycleOwner)
            setViewTreeSavedStateRegistryOwner(lifecycleOwner)
            setContent {
                OverlayRoot(
                    expanded = expanded.value,
                    engineOn = engineOn.value,
                    category = category.value,
                    presets = allPresets.value.filter { it.origin == category.value },
                    activeId = activeId.value,
                    localeTag = localeTag,
                    onDrag = ::onDrag,
                    onToggleExpand = { expanded.value = !expanded.value },
                    onClose = { expanded.value = false },
                    onToggleEngine = { on -> scope.launch { toggleEngine(on) } },
                    onPickCategory = { c -> category.value = c },
                    onPickPreset = { id -> scope.launch { applyPreset(id) } },
                    onPlaySoundClick = { /* Stage 3: open the soundboard list */ },
                )
            }
        }
        windowManager.addView(view, params)
        overlayView = view
    }

    private fun onDrag(dx: Float, dy: Float) {
        val v = overlayView ?: return
        params.x += dx.toInt()
        params.y += dy.toInt()
        // keep it on-screen
        params.x = params.x.coerceIn(0, resources.displayMetrics.widthPixels - 1)
        params.y = params.y.coerceIn(0, resources.displayMetrics.heightPixels - 1)
        runCatching { windowManager.updateViewLayout(v, params) }
    }

    private fun startAsForeground() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "AVCRT overlay", NotificationManager.IMPORTANCE_MIN),
            )
        }
        val notif: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentTitle("AVCRT")
            .setContentText("Плавающее окно активно")
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
        overlayView?.let { runCatching { windowManager.removeView(it) } }
        overlayView = null
        lifecycleOwner.onDestroy()
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "OverlayService"
        private const val CHANNEL_ID = "avcrt_overlay"
        private const val NOTIF_ID = 4711

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
