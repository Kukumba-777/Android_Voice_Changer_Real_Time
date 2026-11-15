package dev.avcrt.app.overlay

import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import androidx.savedstate.SavedStateRegistry
import androidx.savedstate.SavedStateRegistryController
import androidx.savedstate.SavedStateRegistryOwner

/**
 * A ComposeView added to a WindowManager overlay has no Activity to supply the
 * lifecycle / saved-state / view-model owners that Compose's ViewTree* lookups need.
 * This is a minimal standalone owner the service drives manually (onCreate→RESUMED,
 * onDestroy→DESTROYED) and attaches to the overlay ComposeView.
 */
class OverlayLifecycleOwner :
    LifecycleOwner,
    ViewModelStoreOwner,
    SavedStateRegistryOwner {

    private val lifecycleRegistry = LifecycleRegistry(this)
    private val savedStateController = SavedStateRegistryController.create(this)
    private val store = ViewModelStore()

    override val lifecycle: Lifecycle get() = lifecycleRegistry
    override val viewModelStore: ViewModelStore get() = store
    override val savedStateRegistry: SavedStateRegistry get() = savedStateController.savedStateRegistry

    fun onCreate() {
        savedStateController.performRestore(null)
        lifecycleRegistry.currentState = Lifecycle.State.RESUMED
    }

    fun onDestroy() {
        // LifecycleRegistry forbids INITIALIZED -> DESTROYED (must pass through CREATED). If the
        // service bailed before onCreate() ran (e.g. no overlay permission), there's nothing to tear
        // down — skip, or the move-to-DESTROYED throws and crashes the process.
        if (lifecycleRegistry.currentState == Lifecycle.State.INITIALIZED) return
        lifecycleRegistry.currentState = Lifecycle.State.DESTROYED
        store.clear()
    }
}
