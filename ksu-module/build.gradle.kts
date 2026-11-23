import com.android.build.gradle.tasks.ExternalNativeBuildTask
import java.security.MessageDigest
import java.util.zip.ZipFile

/**
 * Build the bundled KSU module:
 *   1. compile the native targets via NDK + CMake
 *   2. assemble a Magisk/KernelSU-format module zip from the release native outputs
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
    ndkVersion = libs.versions.androidNdk.get()

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
            // Both ARM ABIs are built for the legacy soundfx target. The module
            // ZIPs package ARM64 only; a 32-bit app does not get this Zygisk payload.
            //noinspection ChromeOsAbiSupport
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = libs.versions.androidCmake.get()
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
val cellularOnly = providers.gradleProperty("avcrtCellularOnly").map { it.toBooleanStrict() }.orElse(false)

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
    inputs.property("metadata", text)
    doLast {
        out.get().asFile.apply { parentFile.mkdirs() }.writeText(text)
    }
}

val templateOutput = assemblyDir.map { it.dir("template") }
val binariesOutput = assemblyDir.map { it.dir("binaries") }
val metadataOutput = assemblyDir.map { it.file("module.prop") }

val stageModuleTemplate by tasks.registering(Sync::class) {
    from(moduleTemplateDir)
    into(templateOutput)
}

// Package exact files from the release native task, not from whichever CMake
// hash or variant happens to match a glob under build/intermediates.
val stageModuleBinaries by tasks.registering(Sync::class) {
    into(binariesOutput)
    includeEmptyDirs = false
    duplicatesStrategy = DuplicatesStrategy.FAIL
}

afterEvaluate {
    val releaseNativeOutput = tasks.named<ExternalNativeBuildTask>("externalNativeBuildRelease").flatMap { it.soFolder }
    stageModuleBinaries.configure {
        val payloads = mapOf(
            "avcrtd" to "bin/avcrtd",
            "libzygisk_avcrt.so" to "zygisk/arm64-v8a.so",
            "libavcrt_afhook.so" to "system/lib64/libavcrt_afhook.so",
            "avcrt_inject" to "bin/avcrt_inject",
        )
        for ((source, destination) in payloads) {
            val binary = releaseNativeOutput.map { it.file("arm64-v8a/$source") }
            // A single required input file: a missing output fails the build.
            inputs.file(binary)
            from(binary) { eachFile { path = destination } }
        }
    }
}

/**
 * Top-level task — assembles the module zip and drops it into the app's
 * assets so it ships inside the APK.
 */
fun Zip.configureModuleZip(fileName: String, withoutZygisk: Boolean, withAf: Boolean = false) {
    dependsOn(writeModuleProp, stageModuleTemplate, stageModuleBinaries)
    archiveFileName.set(fileName)
    destinationDirectory.set(layout.buildDirectory.dir("outputs"))
    isReproducibleFileOrder = true
    isPreserveFileTimestamps = false
    duplicatesStrategy = DuplicatesStrategy.FAIL
    inputs.property("withoutZygisk", withoutZygisk)
    inputs.property("withAf", withAf)
    from(templateOutput, binariesOutput, metadataOutput)
    includeEmptyDirs = false
    // Managers may skip the whole module when no Zygisk provider is enabled.
    if (withoutZygisk) exclude("zygisk", "zygisk/**")
    if (!withAf) exclude("system/**/libavcrt_afhook.so", "bin/avcrt_inject", "sepolicy.rule")

    // Resolve rootProject refs at configuration time — only plain File objects
    // (which are serializable) are captured in the doLast action below.
    val assetsDst = if (fileName == "avcrt-module-af-experimental.zip") null
        else rootProject.file("app/src/main/assets/$fileName")
    val rootDir = rootProject.projectDir
    assetsDst?.let { outputs.file(it) }

    doLast {
        val src = archiveFile.get().asFile
        ZipFile(src).use { zip ->
            for (required in listOf("module.prop", "customize.sh", "post-fs-data.sh", "service.sh", "uninstall.sh", "bin/avcrtd")) {
                check(zip.getEntry(required)?.isDirectory == false) { "$fileName is missing $required" }
            }
            val hasZygisk = zip.entries().asSequence().any { it.name == "zygisk" || it.name.startsWith("zygisk/") }
            check(hasZygisk != withoutZygisk) { "Unexpected Zygisk contents in $fileName" }
            for (entry in listOf("system/lib64/libavcrt_afhook.so", "bin/avcrt_inject", "sepolicy.rule")) {
                check((zip.getEntry(entry)?.isDirectory == false) == withAf) {
                    "Unexpected presence or absence of $entry in $fileName"
                }
            }
            if (!withoutZygisk) {
                check(zip.getEntry("zygisk/arm64-v8a.so")?.isDirectory == false) {
                    "$fileName is missing the Zygisk library"
                }
            }
        }
        if (assetsDst != null) {
            assetsDst.parentFile.mkdirs()
            src.copyTo(assetsDst, overwrite = true)
        }
        val sha = MessageDigest.getInstance("SHA-256").digest(src.readBytes())
            .joinToString("") { "%02x".format(it) }
        logger.lifecycle(
            "Bundled KSU module -> ${(assetsDst ?: src).relativeTo(rootDir)} (sha256: ${sha.take(16)}…)",
        )
    }
}

val bundleCellularModuleZip by tasks.registering(Zip::class) {
    configureModuleZip("avcrt-module-cellular.zip", withoutZygisk = true)
}

val bundleExperimentalAfModuleZip by tasks.registering(Zip::class) {
    configureModuleZip("avcrt-module-af-experimental.zip", withoutZygisk = true, withAf = true)
}

// Standalone manual diagnostic; never staged into the AVCRT APK or module.
val stagefrightSource = rootProject.layout.projectDirectory.file("tools/capture-probe/stagefright_breakpoint.c")
val stagefrightCounter = layout.buildDirectory.file("stagefright-probe/avcrt-stagefright-counter")
val stagefrightCompiler = androidComponents.sdkComponents.sdkDirectory.map {
    it.file("ndk/${libs.versions.androidNdk.get()}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang")
}
val compileStagefrightCounter by tasks.registering(Exec::class) {
    inputs.files(stagefrightSource, stagefrightCompiler)
    outputs.file(stagefrightCounter)
    val output = stagefrightCounter.get().asFile
    commandLine(stagefrightCompiler.get().asFile, "-O2", "-Wall", "-Wextra", "-Werror",
        stagefrightSource.asFile, "-o", output)
    doFirst { output.parentFile.mkdirs() }
}

val bundleExperimentalStagefrightModuleZip by tasks.registering(Zip::class) {
    dependsOn(compileStagefrightCounter)
    archiveFileName.set("avcrt-module-stagefright-probe.zip")
    destinationDirectory.set(layout.buildDirectory.dir("outputs"))
    from("src/stagefright-probe/module")
    from(stagefrightCounter) { into("bin") }
    doLast {
        ZipFile(archiveFile.get().asFile).use { zip ->
            val files = zip.entries().asSequence().filterNot { it.isDirectory }.map { it.name }.toSet()
            check(files == setOf("module.prop", "customize.sh", "bin/avcrt-stagefright-counter"))
        }
    }
}

val bundleModuleZip by tasks.registering(Zip::class) {
    dependsOn(bundleCellularModuleZip)
    configureModuleZip("avcrt-module.zip", withoutZygisk = cellularOnly.get(), withAf = !cellularOnly.get())
}
