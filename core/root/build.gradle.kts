plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
}

android {
    namespace = "dev.avcrt.core.root"
}

dependencies {
    api(project(":core:common"))

    implementation(libs.bundles.libsu)
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)
}
