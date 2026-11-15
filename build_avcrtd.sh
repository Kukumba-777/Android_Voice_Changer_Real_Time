#!/bin/bash
# Standalone build of the avcrtd daemon (arm64) — reconstructed from the cached CMake deps.
# Output: /tmp/afhook_build/avcrtd  (self-contained, -static-libstdc++).
set -u
NDK=/home/niftox/Android/Sdk/ndk/27.0.12077973
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX=$BIN/aarch64-linux-android30-clang++
CC=$BIN/aarch64-linux-android30-clang
ROOT=/home/niftox/avcrt/ksu-module
SRC=$ROOT/src/main/cpp
ST=$ROOT/.cxx/RelWithDebInfo/h3l6c6h2/arm64-v8a/_deps/soundtouch-src
TA=$ROOT/.cxx/Debug/4g5j6w72/arm64-v8a/_deps/tinyalsa-src
JSON=$ROOT/.cxx/RelWithDebInfo/h3l6c6h2/arm64-v8a/_deps/nlohmann_json-src/include
OUT=/tmp/afhook_build
rm -rf "$OUT"; mkdir -p "$OUT"
fail() { echo "BUILD FAILED: $1"; exit 1; }

echo "== tinyalsa (.c) =="
for f in mixer mixer_hw mixer_plugin pcm pcm_hw pcm_plugin snd_utils; do
  $CC -c "$TA/$f.c" -I"$TA/include" -O2 -Wno-macro-redefined -Wno-unused-parameter \
      -o "$OUT/ta_$f.o" || fail "tinyalsa $f"
done

echo "== SoundTouch (.cpp) =="
for f in "$ST"/source/SoundTouch/*.cpp; do
  $CXX -c "$f" -I"$ST/include" -I"$ST/source/SoundTouch" -O2 -std=c++17 -fPIC \
       -o "$OUT/st_$(basename "$f" .cpp).o" || fail "soundtouch $(basename "$f")"
done

echo "== avcrt engine/config/log + daemon =="
INC="-I$SRC/src -I$ST/include -I$JSON -I$TA/include"
$CXX -c "$SRC/src/avcrt_engine.cpp" $INC -O2 -std=c++17 -o "$OUT/avcrt_engine.o" || fail "engine"
$CXX -c "$SRC/src/avcrt_config.cpp" $INC -O2 -std=c++17 -o "$OUT/avcrt_config.o" || fail "config"
$CXX -c "$SRC/src/avcrt_log.cpp"    $INC -O2 -std=c++17 -o "$OUT/avcrt_log.o"    || fail "log"
$CXX -c "$SRC/daemon/avcrtd.cpp"    $INC -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter \
     -o "$OUT/avcrtd.o" || fail "avcrtd.cpp"

echo "== link =="
$CXX "$OUT"/*.o -static-libstdc++ -llog -o "$OUT/avcrtd" || fail "link"
echo "OK -> $OUT/avcrtd"
file "$OUT/avcrtd"
ls -la "$OUT/avcrtd"
