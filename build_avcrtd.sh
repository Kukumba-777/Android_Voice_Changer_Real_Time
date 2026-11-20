#!/usr/bin/env bash
# Build the AVCRT debug APK and its bundled KernelSU/Magisk module.
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_ROOT"

if [[ ! -x ./gradlew ]]; then
  echo "Gradle wrapper is missing or not executable: $PROJECT_ROOT/gradlew" >&2
  exit 1
fi

SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [[ -z "$SDK_ROOT" ]]; then
  echo "Set ANDROID_HOME or ANDROID_SDK_ROOT to an Android SDK installation." >&2
  exit 1
fi
export ANDROID_SDK_ROOT="$SDK_ROOT"
export ANDROID_HOME="$SDK_ROOT"

catalog_version() {
  local value
  value="$(sed -nE "s/^$1[[:space:]]*=[[:space:]]*\"([0-9.]+)\"[[:space:]]*$/\1/p" gradle/libs.versions.toml)"
  if [[ ! "$value" =~ ^[0-9]+(\.[0-9]+)*$ ]]; then
    echo "Missing or invalid $1 in gradle/libs.versions.toml" >&2
    return 1
  fi
  printf '%s\n' "$value"
}

DEFAULT_COMPILE_SDK="$(catalog_version compileSdk)"
COMPILE_SDK="${ANDROID_COMPILE_SDK:-$DEFAULT_COMPILE_SDK}"
if [[ $# -gt 0 ]]; then
  if [[ $# -eq 2 && "$1" == "--compile-sdk" ]]; then
    COMPILE_SDK="$2"
  else
    echo "Usage: $0 [--compile-sdk API]" >&2
    exit 2
  fi
fi

case "$COMPILE_SDK" in
  34|35) ;;
  *)
    echo "Unsupported Android compile SDK: $COMPILE_SDK (choose 34 or 35)." >&2
    exit 1
    ;;
esac
export ANDROID_COMPILE_SDK="$COMPILE_SDK"

if command -v sdkmanager >/dev/null 2>&1; then
  SDKMANAGER="$(command -v sdkmanager)"
elif [[ -x "$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" ]]; then
  SDKMANAGER="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
else
  echo "Android sdkmanager is missing. Install Android command-line tools first." >&2
  exit 1
fi

"$SDKMANAGER" --sdk_root="$SDK_ROOT" --install \
  "platforms;android-$COMPILE_SDK" \
  "build-tools;$(catalog_version androidBuildTools)" \
  "ndk;$(catalog_version androidNdk)" \
  "cmake;$(catalog_version androidCmake)"

./gradlew --no-daemon --stacktrace -Pavcrt.compileSdk="$COMPILE_SDK" :app:assembleDebug

APK="$PROJECT_ROOT/app/build/outputs/apk/debug/app-debug.apk"
MODULE="$PROJECT_ROOT/ksu-module/build/outputs/avcrt-module.zip"
RECOVERY="$PROJECT_ROOT/ksu-module/build/outputs/avcrt-module-cellular.zip"

for artifact in "$APK" "$MODULE" "$RECOVERY"; do
  if [[ ! -s "$artifact" ]]; then
    echo "Expected build artifact was not produced: $artifact" >&2
    exit 1
  fi
  sha256sum "$artifact"
done

echo "APK: $APK"
echo "Module: $MODULE"
echo "Cellular recovery module: $RECOVERY"
