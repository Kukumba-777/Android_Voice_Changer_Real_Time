package dev.avcrt.engine.rvc.di

import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dev.avcrt.domain.converter.VoiceConverter
import dev.avcrt.engine.rvc.RvcVoiceConverter
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class RvcBindingsModule {

    @Binds
    @Singleton
    abstract fun bindVoiceConverter(impl: RvcVoiceConverter): VoiceConverter
}
