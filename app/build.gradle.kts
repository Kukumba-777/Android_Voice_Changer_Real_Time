import java.util.Properties

plugins {
    alias(libs.plugins.avcrt.android.application)
    alias(libs.plugins.avcrt.android.compose)
    alias(libs.plugins.avcrt.android.hilt)
    alias(libs.plugins.kotlin.serialization)
}

val keystorePropsFile = rootProject.file("keystore.properties")
val hasKeystore = keystorePropsFile.exists()

android {
    namespace = "dev.avcrt.app"

    defaultConfig {
        applicationId = "dev.avcrt.app"

        // Resources merge in from every feature module. Keep only the locales we actually ship.
        resourceConfigurations += listOf("en", "ru")
    }

    if (hasKeystore) {
        signingConfigs {
            create("release") {
                val props = Properties().apply { keystorePropsFile.inputStream().use { load(it) } }
                storeFile = rootProject.file(props.getProperty("storeFile"))
                storePassword = props.getProperty("storePassword")
                keyAlias = props.getProperty("keyAlias")
                keyPassword = props.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (hasKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    buildFeatures {
        buildConfig = true
    }
}

dependencies {
    implementation(project(":core:common"))
    implementation(project(":core:ui"))
    implementation(project(":core:datastore"))
    implementation(project(":core:root"))

    implementation(project(":domain"))
    implementation(project(":data"))

    implementation(project(":feature:home"))
    implementation(project(":feature:presets"))
    implementation(project(":feature:converter"))
    implementation(project(":feature:logs"))
    implementation(project(":feature:settings"))

    implementation(project(":engine:api"))
    implementation(project(":engine:hal"))
    implementation(project(":engine:rvc"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.google.material)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.startup.runtime)
    implementation(libs.hilt.navigation.compose)
    implementation(libs.bundles.lifecycle)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.timber)
}

/**
 * Make sure the KSU module zip is built and dropped into the app's assets/
 * before AGP packages anything. `preBuild` is the earliest task that runs
 * for every variant, so depending on it covers debug + release without
 * having to enumerate variants.
 */
afterEvaluate {
    tasks.named("preBuild").configure {
        dependsOn(":ksu-module:bundleModuleZip")
    }
}
