package dev.avcrt.app

import android.app.Application
import dagger.hilt.android.HiltAndroidApp
import dev.avcrt.core.common.logging.LogSink
import timber.log.Timber
import javax.inject.Inject

@HiltAndroidApp
class AvcrtApplication : Application() {

    @Inject lateinit var logSink: LogSink
    @Inject lateinit var appInitializers: AppInitializers

    override fun onCreate() {
        super.onCreate()

        if (BuildConfig.DEBUG) {
            Timber.plant(Timber.DebugTree())
        }
        Timber.plant(logSink.asTree())

        appInitializers.runAll()
    }
}
