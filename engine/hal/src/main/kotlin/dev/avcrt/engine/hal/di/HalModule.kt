package dev.avcrt.engine.hal.di

import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dagger.multibindings.IntoSet
import dev.avcrt.core.common.startup.Initializer
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.media.Soundboard
import dev.avcrt.engine.api.EngineDiagnostics
import dev.avcrt.engine.hal.HalDiagnostics
import dev.avcrt.engine.hal.HalEngine
import dev.avcrt.engine.hal.sound.HalSoundboard
import dev.avcrt.engine.hal.sound.SoundboardCache
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class HalBindingsModule {

    @Binds
    @Singleton
    abstract fun bindVoiceEnginePort(impl: HalEngine): VoiceEnginePort

    @Binds
    @Singleton
    abstract fun bindSoundboard(impl: HalSoundboard): Soundboard

    @Binds
    @IntoSet
    abstract fun bindSoundboardInitializer(impl: SoundboardCache): Initializer

    @Binds
    @Singleton
    abstract fun bindEngineDiagnostics(impl: HalDiagnostics): EngineDiagnostics
}
