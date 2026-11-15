plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "dev.avcrt.data"
}

dependencies {
    api(project(":domain"))

    implementation(project(":core:common"))
    implementation(project(":core:datastore"))
    implementation(project(":core:root"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.timber)
}
