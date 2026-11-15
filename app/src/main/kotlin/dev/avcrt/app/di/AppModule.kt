package dev.avcrt.app.di

import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dagger.multibindings.IntoSet
import dev.avcrt.app.engine.EngineAutoStartInitializer
import dev.avcrt.app.locale.LocaleApplier
import dev.avcrt.core.common.startup.Initializer
import dev.avcrt.engine.hal.EngineDiagnosticsAutoRunner

@Module
@InstallIn(SingletonComponent::class)
object AppModule {

    @Provides
    @IntoSet
    fun provideLocaleInitializer(applier: LocaleApplier): Initializer = object : Initializer {
        override val priority: Int get() = 0
        override fun initialize() = applier.start()
    }

    @Provides
    @IntoSet
    fun provideEngineAutoStartInitializer(impl: EngineAutoStartInitializer): Initializer = impl

    @Provides
    @IntoSet
    fun provideDiagnosticsAutoRunner(runner: EngineDiagnosticsAutoRunner): Initializer =
        object : Initializer {
            override val priority: Int get() = 100
            override fun initialize() = runner.start()
        }
}
