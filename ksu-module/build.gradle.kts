import java.security.MessageDigest

/**
 * Build the bundled KSU module:
 *   1. compile `libavcrtfx.so` for the target ABIs via NDK + CMake
 *   2. assemble a Magisk/KernelSU-format module zip with the .so files in
 *      `system/lib[64]/soundfx/` and the rest of the module template
 *   3. drop the zip into `app/src/main/assets/avcrt-module.zip`
 *
 * Module identity (id/name/version/etc.) comes from the version catalog so
 * the Kotlin side, module.prop, and CMake never disagree. The :engine:hal
 * module reads the same values via buildConfigField — see its build.gradle.kts.
 *
 * NOTE: AGP produces an AAR as a byproduct — nobody consumes it; the only
 * artifacts we care about are the .so files and the bundled zip.
 */
plugins {
    alias(libs.plugins.avcrt.android.library)
}

android {
    namespace = "dev.avcrt.ksumodule"

    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags("-std=c++17", "-fexceptions", "-frtti", "-O3", "-fvisibility=hidden")
                // c++_STATIC, not c++_shared: libavcrtfx.so is dlopen'd by the
                // audio HAL / audioserver, NOT by our app, so libc++_shared.so is
                // NOT in the loading process's linker namespace and a c++_shared
                // build fails to dlopen (effect silently skipped). Static libc++
                // makes the .so self-contained; the version-script exports only
                // AUDIO_EFFECT_LIBRARY_INFO_SYM so there's no STL symbol leakage.
                arguments("-DANDROID_STL=c++_static")
                // AGP builds all module natives from one CMakeLists: avcrtd (the
                // cellular daemon), zygisk_avcrt (the app-capture hook, arm64 only),
                // and the legacy avcrtfx soundfx lib (kept but no longer staged).
            }
        }
        ndk {
            // Ship BOTH ARM ABIs: arm64-v8a (64-bit HALs, e.g.
            // android.hardware.audio.service_64 on HyperOS) AND armeabi-v7a (32-bit HALs,
            // e.g. android.hardware.audio.service on the OnePlus Nord — its effects loader
            // searches /vendor/lib/soundfx). The audio HAL only loads a soundfx lib matching
            // its OWN bitness, so a 32-bit HAL fails with "Could not find library in effect
            // directories" if we ship arm64 only. (AGP runs CMake — incl. FetchContent of
            // SoundTouch + nlohmann/json — once per ABI, so this ~doubles cold-build time.
            // x86_64 still omitted: emulator-only.)
            //noinspection ChromeOsAbiSupport
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = false
    }
}

// In build.gradle.kts the `libs` symbol is a type-safe accessor, not the
// `VersionCatalog` instance — that's why `.findVersion()` is unresolved. We
// read straight off the accessors instead; the catalog stays the only source.
val moduleId             = libs.versions.moduleId.get()
val moduleName           = libs.versions.moduleName.get()
val moduleVersion        = libs.versions.moduleVersion.get()
val moduleVersionCodeStr = libs.versions.moduleVersionCode.get()
val moduleAuthor         = libs.versions.moduleAuthor.get()
val moduleDescription    = libs.versions.moduleDescription.get()

val moduleTemplateDir = layout.projectDirectory.dir("src/main/module")
val assemblyDir       = layout.buildDirectory.dir("ksu-module-assembly")
val zipOutputFile     = layout.buildDirectory.file("outputs/avcrt-module.zip")

/**
 * Writes module.prop with values from the version catalog. The id MUST match
 * [dev.avcrt.core.common.AvcrtPaths.MODULE_ID] — both sides resolve from the
 * same catalog entry so a drift is impossible.
 */
val writeModuleProp by tasks.registering {
    val out = assemblyDir.map { it.file("module.prop") }
    outputs.file(out)
    val text = buildString {
        appendLine("id=$moduleId")
        appendLine("name=$moduleName")
        appendLine("version=$moduleVersion")
        appendLine("versionCode=$moduleVersionCodeStr")
        appendLine("author=$moduleAuthor")
        appendLine("description=$moduleDescription")
    }
    doLast {
        out.get().asFile.apply { parentFile.mkdirs() }.writeText(text)
    }
}

val stageModuleTemplate by tasks.registering(Copy::class) {
    from(moduleTemplateDir)
    into(assemblyDir)
}

/**
 * Stages the cellular voice-changer daemon (`avcrtd`, arm64) into the module's
 * `bin/` and the Zygisk app-capture hook (`libzygisk_avcrt.so`, arm64) into
 * `zygisk/arm64-v8a.so`. Both come from the CMake build output. This REPLACES the
 * old soundfx libavcrtfx.so staging — that HAL-effect approach is dead (vendor
 * libeffects use-after-unload crash); the daemon + Zygisk hook are the live paths.
 *
 * AGP puts CMake target outputs under `build/intermediates/cxx/<variant>/<hash>/
 * obj/<abi>/` (unstripped) and `stripped_native_libs/.../<abi>/` (stripped). We
 * glob both and pick the executable + the .so.
 */
val stageModuleBinaries by tasks.registering(Copy::class) {
    dependsOn("externalNativeBuildRelease")
    val intermediates = layout.buildDirectory.dir("intermediates")

    // avcrtd executable (arm64-v8a) -> bin/avcrtd
    from(intermediates.map { dir ->
        fileTree(dir) {
            include("**/cxx/**/obj/arm64-v8a/avcrtd")
            include("**/RelWithDebInfo/**/arm64-v8a/avcrtd")
        }
    }) {
        eachFile { path = "bin/avcrtd" }
        includeEmptyDirs = false
        duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    }

    // zygisk hook (arm64-v8a) -> zygisk/arm64-v8a.so
    from(intermediates.map { dir ->
        fileTree(dir) {
            include("**/cxx/**/obj/arm64-v8a/libzygisk_avcrt.so")
            include("**/RelWithDebInfo/**/arm64-v8a/libzygisk_avcrt.so")
        }
    }) {
        eachFile { path = "zygisk/arm64-v8a.so" }
        includeEmptyDirs = false
        duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    }

    // universal AudioFlinger hook (arm64-v8a) -> system/lib64/libavcrt_afhook.so.
    // Magic-mounts to /system/lib64 (the only path the isolated audioserver linker namespace
    // will dlopen from); avcrtd ptrace-injects it into audioserver at boot.
    from(intermediates.map { dir ->
        fileTree(dir) {
            include("**/cxx/**/obj/arm64-v8a/libavcrt_afhook.so")
            include("**/RelWithDebInfo/**/arm64-v8a/libavcrt_afhook.so")
        }
    }) {
        eachFile { path = "system/lib64/libavcrt_afhook.so" }
        includeEmptyDirs = false
        duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    }

    // ptrace injector helper (arm64-v8a) -> bin/avcrt_inject (avcrtd forks it).
    from(intermediates.map { dir ->
        fileTree(dir) {
            include("**/cxx/**/obj/arm64-v8a/avcrt_inject")
            include("**/RelWithDebInfo/**/arm64-v8a/avcrt_inject")
        }
    }) {
        eachFile { path = "bin/avcrt_inject" }
        includeEmptyDirs = false
        duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    }
    into(assemblyDir)
}

/**
 * Top-level task — assembles the module zip and drops it into the app's
 * assets so it ships inside the APK.
 */
val bundleModuleZip by tasks.registering(Zip::class) {
    dependsOn(writeModuleProp, stageModuleTemplate, stageModuleBinaries)
    archiveFileName.set("avcrt-module.zip")
    destinationDirectory.set(layout.buildDirectory.dir("outputs"))
    from(assemblyDir)

    // Resolve rootProject refs at configuration time — only plain File objects
    // (which are serializable) are captured in the doLast action below.
    val assetsDst = rootProject.file("app/src/main/assets/avcrt-module.zip")
    val rootDir = rootProject.projectDir

    doLast {
        val src = archiveFile.get().asFile
        assetsDst.parentFile.mkdirs()
        src.copyTo(assetsDst, overwrite = true)
        val sha = MessageDigest.getInstance("SHA-256").digest(src.readBytes())
            .joinToString("") { "%02x".format(it) }
        logger.lifecycle(
            "Bundled KSU module -> ${assetsDst.relativeTo(rootDir)} (sha256: ${sha.take(16)}…)",
        )
    }
}
