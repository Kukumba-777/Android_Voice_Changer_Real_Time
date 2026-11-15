cmake_minimum_required(VERSION 3.22.1)
project(avcrtfx LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------------------------------------------------------
# Third-party deps fetched on first build into ${CMAKE_BINARY_DIR}/_deps
# so the source tree stays small + git stays clean.
# -------------------------------------------------------------------
include(FetchContent)

# nlohmann/json — header-only JSON parser used to read the active preset JSON.
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# SoundTouch — LGPL pitch / tempo shifter, self-contained (no external deps),
# ships with its own CMakeLists.txt that defines the `SoundTouch` target.
set(SOUNDTOUCH_DLL OFF CACHE BOOL "" FORCE)
set(SOUNDTOUCH_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SOUNDSTRETCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        soundtouch
        GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git
        GIT_TAG 2.3.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(soundtouch)

# tinyalsa — userspace ALSA (PCM + mixer) for the cellular voice-changer daemon.
# Use the AOSP fork, NOT github.com/tinyalsa/tinyalsa: the github v2.0.0 release's
# pcm_read returns EFAULT ("Bad address") on this device → engage/disengage flap →
# torn audio. The AOSP fork has the Android pcm fixes (verified clean on a live call).
# AOSP tinyalsa ships Android.bp (no CMakeLists), so we only download it and compile
# its .c sources into our own static lib (mirrors the working standalone build).
FetchContent_Declare(
        tinyalsa
        GIT_REPOSITORY https://android.googlesource.com/platform/external/tinyalsa
        GIT_TAG 4e466e8f90e5c15c029a18461cc384c0f7777193
)
FetchContent_GetProperties(tinyalsa)
if(NOT tinyalsa_POPULATED)
        FetchContent_Populate(tinyalsa)
endif()
add_library(tinyalsa STATIC
        ${tinyalsa_SOURCE_DIR}/mixer.c
        ${tinyalsa_SOURCE_DIR}/mixer_hw.c
        ${tinyalsa_SOURCE_DIR}/mixer_plugin.c
        ${tinyalsa_SOURCE_DIR}/pcm.c
        ${tinyalsa_SOURCE_DIR}/pcm_hw.c
        ${tinyalsa_SOURCE_DIR}/pcm_plugin.c
        ${tinyalsa_SOURCE_DIR}/snd_utils.c
)
target_include_directories(tinyalsa PUBLIC ${tinyalsa_SOURCE_DIR}/include)
target_compile_options(tinyalsa PRIVATE -Wno-macro-redefined -Wno-unused-parameter)

# -------------------------------------------------------------------
# libavcrtfx.so — the Android Audio Effect implementation.
# -------------------------------------------------------------------
add_library(avcrtfx SHARED
        src/avcrt_effect.cpp
        src/avcrt_engine.cpp
        src/avcrt_config.cpp
        src/avcrt_log.cpp
)

target_include_directories(avcrtfx PRIVATE
        src
)

target_link_libraries(avcrtfx PRIVATE
        log
        SoundTouch
        nlohmann_json::nlohmann_json
)

target_compile_options(avcrtfx PRIVATE
        -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
)

# Built WITHOUT CFI on purpose. bionic marks a loaded library that exports no __cfi_check as
# "unchecked" in the CFI shadow, so the CFI-instrumented effects factory's indirect calls into us
# pass cleanly — a non-CFI effect lib is valid. (A whole session was lost trying to satisfy CFI
# from our side — hand-rolled/compiler __cfi_check, recover, generalize-pointers, static-libsan —
# all dead ends, because the real crash was use-after-unload, not a CFI type-id mismatch.)
target_link_options(avcrtfx PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/avcrtfx.version"
        # Keep the .so permanently mapped. The effects factory dlopen's us to read the descriptor,
        # then dlclose's us; bionic AddInvalid's the unloaded pages (kInvalid), and a later
        # createEffect through a stale get_descriptor pointer SIGABRTs the audio HAL
        # (__loader_cfi_fail at Effect_GetDescriptor). -z nodelete makes dlclose a no-op so our
        # pages stay valid for the process lifetime — fixes both boot enumeration and record create.
        -Wl,-z,nodelete
)

# -------------------------------------------------------------------
# avcrtd — cellular voice-changer daemon (the clean-replace service).
# Self-contained native executable shipped in the KSU module's bin/ and launched
# by service.sh. Needs the patched kernel ("AVCRT Dec Mute" tx-macro kcontrol).
# -------------------------------------------------------------------
add_executable(avcrtd
        daemon/avcrtd.cpp
        src/avcrt_engine.cpp
        src/avcrt_config.cpp
        src/avcrt_log.cpp
)
target_include_directories(avcrtd PRIVATE src)
target_link_libraries(avcrtd PRIVATE
        log
        SoundTouch
        nlohmann_json::nlohmann_json
        tinyalsa
)
target_compile_options(avcrtd PRIVATE
        -Wall -Wextra -Wno-unused-parameter -O2
)
# Static C++ runtime so the binary is self-contained in /data/adb/modules.
target_link_options(avcrtd PRIVATE -static-libstdc++)

# -------------------------------------------------------------------
# libzygisk_avcrt.so — Zygisk module for system-wide app voice change.
# Loaded into every app process; PLT-hooks android::AudioRecord::read and pitches
# the captured buffer in-place via avcrt::Engine. arm64 only — Zygisk doesn't load
# the module into 32-bit app processes on a 64-bit device, so armeabi-v7a is skipped.
# -------------------------------------------------------------------
if(ANDROID_ABI STREQUAL "arm64-v8a")
    # Zygisk app-capture hook. Inline-hooks android::AudioRecord::read in libaudioclient via our
    # own tiny AArch64 hooker (zygisk/avcrt_inlinehook.h) — see that file for why we don't use a
    # general lib (shadowhook is dead on Android 16; Dobby's only release doesn't compile for
    # Android). No external hook dependency, fully self-contained.
    add_library(zygisk_avcrt SHARED
            zygisk/zygisk_avcrt.cpp
            src/avcrt_engine.cpp
            src/avcrt_log.cpp
    )
    target_include_directories(zygisk_avcrt PRIVATE src zygisk)
    target_link_libraries(zygisk_avcrt PRIVATE
            log
            SoundTouch
    )
    target_compile_options(zygisk_avcrt PRIVATE
            -Wall -Wextra -Wno-unused-parameter -O2 -fPIC -fvisibility=hidden
    )
    # self-contained C++ runtime. Do NOT add -Wl,-z,max-page-size=16384: this device is 4KB
    # pages and Magisk Zygisk's memfd module loader SILENTLY REFUSES a 16KB-aligned module .so
    # (it then loads in NO app process — confirmed: zygisk_avcrt.so align 0x4000 didn't load,
    # while default-4KB avcrtfx/avcrtd did). NDK's default 4KB alignment is what Zygisk accepts.
    target_link_options(zygisk_avcrt PRIVATE -static-libstdc++)

    # ---------------------------------------------------------------
    # libavcrt_afhook.so — the UNIVERSAL AudioFlinger-level voice hook.
    # ptrace-injected (by avcrtd, via avcrt_inject) into the audioserver process, where it
    # inline-hooks android::AudioStreamIn::read in libaudioflinger_datapath — the one chokepoint
    # every mic capture passes through at the HAL read (apps + mediaserver + camera). Pitches the
    # buffer in place via the shared avcrt::Engine. Reuses the same self-checking inline hooker as
    # the Zygisk module. Staged to system/lib64 (the audioserver namespace only loads from there).
    # ---------------------------------------------------------------
    add_library(avcrt_afhook SHARED
            afhook/avcrt_afhook.cpp
            src/avcrt_engine.cpp
            src/avcrt_log.cpp
    )
    target_include_directories(avcrt_afhook PRIVATE src zygisk afhook)
    target_link_libraries(avcrt_afhook PRIVATE log SoundTouch)
    target_compile_options(avcrt_afhook PRIVATE
            -Wall -Wextra -Wno-unused-parameter -O2 -fPIC -fvisibility=hidden
    )
    # nodelete: the hook + its trampolines live in this .so for the audioserver lifetime.
    target_link_options(avcrt_afhook PRIVATE -static-libstdc++ -Wl,-z,nodelete)

    # avcrt_inject — tiny root ptrace remote-dlopen helper. avcrtd forks it to load the afhook .so
    # into audioserver (isolated in a child so a ptrace glitch can't take down the daemon).
    add_executable(avcrt_inject afhook/avcrt_inject.cpp)
    target_compile_options(avcrt_inject PRIVATE -O2)
    target_link_libraries(avcrt_inject PRIVATE dl)
    target_link_options(avcrt_inject PRIVATE -static-libstdc++)
endif()
