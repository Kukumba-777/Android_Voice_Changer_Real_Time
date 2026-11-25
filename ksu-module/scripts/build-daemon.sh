#!/bin/bash
# Builds the avcrtd cellular voice-changer daemon as a self-contained arm64 binary.
# Reference/standalone build (the CMake `avcrtd` target is the in-Gradle path).
#   env: NDK=/path/to/ndk  (default: $ANDROID_NDK_HOME or the SDK's ndk/27.x)
#   --va-private-lab: separate idle capture-only probe with the verified VA graph profile
#   --usb-capture-lab: separate USB SIM capture/readback diagnostics (never installed by Gradle)
set -e
agm_flags=()
link_flags=()
bin=avcrtd
entry=avcrtd
default_out=/tmp/avcrtd_build
if [ "$#" -eq 1 ] && [ "$1" = --va-private-lab ]; then
  agm_flags=(-DAVCRT_AGM_LAB -DAVCRT_VA_TEST_PRIVATE_PCM -DAVCRT_VA_TEST_PCM -DAVCRT_VA_TEST_SPEAKER
             '-DAVCRT_VA_TEST_INSTANCE="Instance_2"' -DAVCRT_VA_TEST_ACOUSTIC)
  bin=avcrtd-va-private-acoustic
  entry=avcrt_agm_va_lab_main
  default_out=/tmp/avcrtd_va_private_build
elif [ "$#" -eq 1 ] && [ "$1" = --usb-capture-lab ]; then
  agm_flags=(-DAVCRT_USB_CAPTURE_LAB -ffunction-sections -fdata-sections)
  link_flags=(-Wl,--gc-sections)
  bin=avcrt-usb-capture-lab
  entry=avcrt_agm_usb_lab_main
  default_out=/tmp/avcrt_usb_capture_build
elif [ "$#" -ne 0 ]; then
  echo "Usage: $0 [--va-private-lab|--usb-capture-lab]" >&2
  exit 2
fi
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # ksu-module/
SRC="$HERE/src/main/cpp/src"
DST="$HERE/src/main/cpp/daemon"
if [[ ! -f "$DST/$entry.cpp" ]]; then
  echo "Missing optional local lab source: $DST/$entry.cpp" >&2
  exit 2
fi
NDK="${NDK:-${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/aarch64-linux-android31-clang"; CXX="$TC/aarch64-linux-android31-clang++"
OUT="${OUT:-$default_out}"; mkdir -p "$OUT"
OBJ="$(mktemp -d "$OUT/obj.XXXXXXXX")"
trap 'rm -f -- "$OBJ"/*.o "$OBJ/$bin"; rmdir -- "$OBJ"' EXIT
DEPS="${DEPS:-/tmp/avcrt-deps}"; mkdir -p "$DEPS"
# tinyalsa — AOSP fork (NOT github.com/tinyalsa/tinyalsa, whose v2.0.0 pcm_read
# returns EFAULT on this device). The AOSP fork has the Android pcm fixes.
TA_COMMIT=4e466e8f90e5c15c029a18461cc384c0f7777193
if [ ! -d "$DEPS/tinyalsa" ]; then
  git clone https://android.googlesource.com/platform/external/tinyalsa "$DEPS/tinyalsa"
  git -C "$DEPS/tinyalsa" checkout "$TA_COMMIT"
fi
TA="$DEPS/tinyalsa"
# soundtouch + nlohmann: reuse the Gradle .cxx fetch if present, else clone
ST="$(ls -d "$HERE"/.cxx/*/*/arm64-v8a/_deps/soundtouch-src 2>/dev/null | head -1)"
[ -n "$ST" ] || { [ -d "$DEPS/soundtouch" ] || git clone --depth 1 -b 2.3.3 https://codeberg.org/soundtouch/soundtouch.git "$DEPS/soundtouch"; ST="$DEPS/soundtouch"; }
NJ="$(ls -d "$HERE"/.cxx/*/*/arm64-v8a/_deps/nlohmann_json-src/single_include 2>/dev/null | head -1)"
[ -n "$NJ" ] || { [ -d "$DEPS/json" ] || git clone --depth 1 -b v3.11.3 https://github.com/nlohmann/json.git "$DEPS/json"; NJ="$DEPS/json/single_include"; }
echo "[*] tinyalsa=$TA soundtouch=$ST nlohmann=$NJ"
for f in mixer mixer_hw mixer_plugin pcm pcm_hw pcm_plugin snd_utils; do
  "$CC" -I"$TA/include" -O2 -Wno-macro-redefined -c "$TA/src/$f.c" -o "$OBJ/ta_$f.o" 2>/dev/null \
   || "$CC" -I"$TA/include" -O2 -Wno-macro-redefined -c "$TA/$f.c" -o "$OBJ/ta_$f.o"
done
for f in "$ST"/source/SoundTouch/*.cpp; do
  "$CXX" -std=c++17 -I"$ST/include" -O2 -DSOUNDTOUCH_FLOAT_SAMPLES=1 -c "$f" -o "$OBJ/st_$(basename "$f" .cpp).o"
done
"$CXX" -std=c++17 -I"$SRC" -I"$ST/include" -O2 -c "$SRC/avcrt_engine.cpp" -o "$OBJ/avcrt_engine.o"
"$CXX" -std=c++17 -I"$SRC" -O2 -c "$SRC/avcrt_log.cpp" -o "$OBJ/avcrt_log.o"
"$CXX" -std=c++17 -I"$SRC" -I"$NJ" -O2 -c "$SRC/avcrt_config.cpp" -o "$OBJ/avcrt_config.o"
"$CXX" -std=c++17 -I"$SRC" -I"$ST/include" -I"$TA/include" -O2 "${agm_flags[@]}" -c "$DST/$entry.cpp" -o "$OBJ/main.o"
"$CXX" -std=c++17 -I"$SRC" -I"$TA/include" -O2 "${agm_flags[@]}" -c "$DST/avcrt_agm.cpp" -o "$OBJ/avcrt_agm.o"
"$CXX" "$OBJ"/*.o -llog -ldl -static-libstdc++ "${link_flags[@]}" -o "$OBJ/$bin"
file "$OBJ/$bin" | grep -o 'ARM aarch64'
mv -f -- "$OBJ/$bin" "$OUT/$bin"
echo "[*] built: $OUT/$bin"
