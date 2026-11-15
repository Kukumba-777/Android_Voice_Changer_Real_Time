#!/bin/bash
# Builds the avcrtd cellular voice-changer daemon as a self-contained arm64 binary.
# Reference/standalone build (the CMake `avcrtd` target is the in-Gradle path).
#   env: NDK=/path/to/ndk  (default: $ANDROID_NDK_HOME or the SDK's ndk/27.x)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # ksu-module/
SRC="$HERE/src/main/cpp/src"
DST="$HERE/src/main/cpp/daemon"
NDK="${NDK:-${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/aarch64-linux-android31-clang"; CXX="$TC/aarch64-linux-android31-clang++"
OUT="${OUT:-/tmp/avcrtd_build}"; rm -rf "$OUT"; mkdir -p "$OUT"
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
  "$CC" -I"$TA/include" -O2 -Wno-macro-redefined -c "$TA/src/$f.c" -o "$OUT/ta_$f.o" 2>/dev/null \
   || "$CC" -I"$TA/include" -O2 -Wno-macro-redefined -c "$TA/$f.c" -o "$OUT/ta_$f.o"
done
for f in "$ST"/source/SoundTouch/*.cpp; do
  "$CXX" -std=c++17 -I"$ST/include" -O2 -DSOUNDTOUCH_FLOAT_SAMPLES=1 -c "$f" -o "$OUT/st_$(basename "$f" .cpp).o"
done
"$CXX" -std=c++17 -I"$SRC" -I"$ST/include" -O2 -c "$SRC/avcrt_engine.cpp" -o "$OUT/avcrt_engine.o"
"$CXX" -std=c++17 -I"$SRC" -O2 -c "$SRC/avcrt_log.cpp" -o "$OUT/avcrt_log.o"
"$CXX" -std=c++17 -I"$SRC" -I"$NJ" -O2 -c "$SRC/avcrt_config.cpp" -o "$OUT/avcrt_config.o"
"$CXX" -std=c++17 -I"$SRC" -I"$ST/include" -I"$TA/include" -O2 -c "$DST/avcrtd.cpp" -o "$OUT/avcrtd.o"
"$CXX" "$OUT"/*.o -llog -ldl -static-libstdc++ -o "$OUT/avcrtd"
echo "[*] built: $OUT/avcrtd"
file "$OUT/avcrtd" | grep -o 'ARM aarch64'
