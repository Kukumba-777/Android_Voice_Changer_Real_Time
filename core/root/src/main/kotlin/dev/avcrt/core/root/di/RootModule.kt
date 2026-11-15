package dev.avcrt.core.root.di

import dagger.Binds
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import dev.avcrt.core.common.dispatcher.DefaultDispatcherProvider
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.root.LibsuRootAccess
import dev.avcrt.core.root.RootAccess
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class RootBindingsModule {

    @Binds
    @Singleton
    abstract fun bindRootAccess(impl: LibsuRootAccess): RootAccess
}

@Module
@InstallIn(SingletonComponent::class)
object RootProvidersModule {

    @Provides
    @Singleton
    fun provideDispatcherProvider(): DispatcherProvider = DefaultDispatcherProvider()
}
