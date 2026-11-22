package dev.avcrt.data.di

import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dev.avcrt.data.backup.UserBackupImpl
import dev.avcrt.data.log.ExternalLogReaderImpl
import dev.avcrt.data.log.LogRepositoryImpl
import dev.avcrt.data.media.MediaRepositoryImpl
import dev.avcrt.data.preset.PresetRepositoryImpl
import dev.avcrt.data.settings.SettingsRepositoryImpl
import dev.avcrt.domain.log.ExternalLogReader
import dev.avcrt.domain.log.LogRepository
import dev.avcrt.domain.media.MediaRepository
import dev.avcrt.domain.media.UserBackup
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.settings.SettingsRepository
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class DataBindingsModule {

    @Binds
    @Singleton
    abstract fun bindMediaRepository(impl: MediaRepositoryImpl): MediaRepository

    @Binds
    abstract fun bindUserBackup(impl: UserBackupImpl): UserBackup

    @Binds
    @Singleton
    abstract fun bindSettingsRepository(impl: SettingsRepositoryImpl): SettingsRepository

    @Binds
    @Singleton
    abstract fun bindPresetRepository(impl: PresetRepositoryImpl): PresetRepository

    @Binds
    @Singleton
    abstract fun bindLogRepository(impl: LogRepositoryImpl): LogRepository

    @Binds
    @Singleton
    abstract fun bindExternalLogReader(impl: ExternalLogReaderImpl): ExternalLogReader
}
