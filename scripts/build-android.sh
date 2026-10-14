#!/usr/bin/env bash
#
# Reproducible Android APK build script for oSBM (OpenStarbound Mobile)
#
# Usage:
#   ./scripts/build-android.sh [ABI] [BUILD_TYPE]
#
# Arguments:
#   ABI:        arm64 (default), armv7, x64, universal
#   BUILD_TYPE: release (default), debug (universal must be release)
#
# Environment variables recognized (defaults to /opt paths if present):
#   ANDROID_SDK_ROOT / ANDROID_HOME
#   ANDROID_NDK_HOME
#   VCPKG_ROOT
#   STAR_SIGNING_KEYSTORE
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ABI="${1:-arm64}"
BUILD_TYPE="${2:-release}"

# Normalize ABI
case "${ABI,,}" in
  arm64|arm64-v8a|aarch64)
    ARCH="arm64"
    ;;
  armv7|armeabi-v7a|arm)
    ARCH="armv7"
    ;;
  x64|x86_64|x86-64)
    ARCH="x64"
    ;;
  universal|all)
    ARCH="universal"
    ;;
  -h|--help)
    echo "Usage: $0 [arm64|armv7|x64|universal] [release|debug]"
    exit 0
    ;;
  *)
    echo "Error: Unknown ABI: ${ABI}. Supported: arm64, armv7, x64, universal" >&2
    exit 1
    ;;
esac

# Normalize build type
case "${BUILD_TYPE,,}" in
  release|rel)
    CONFIG_TYPE="release"
    SUFFIX="release"
    ;;
  debug|dbg)
    CONFIG_TYPE="debug"
    SUFFIX="debug"
    ;;
  *)
    echo "Error: Unknown build type: ${BUILD_TYPE}. Supported: release, debug" >&2
    exit 1
    ;;
esac

if [ "${ARCH}" = "universal" ] && [ "${CONFIG_TYPE}" = "debug" ]; then
  echo "Error: Universal APKs are release-only in oSBM." >&2
  exit 1
fi

echo "============================================================"
echo " oSBM Android Build: ARCH=${ARCH}, CONFIG=${CONFIG_TYPE}"
echo "============================================================"

# Resolve tool paths and environment
export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/android-sdk}}"
export ANDROID_SDK_ROOT="${ANDROID_HOME}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-${ANDROID_HOME}/ndk/26.3.11579264}"
export VCPKG_ROOT="${VCPKG_ROOT:-/opt/vcpkg}"

if [ -d "/opt/gradle/gradle-8.7/bin" ] && [[ ":$PATH:" != *":/opt/gradle/gradle-8.7/bin:"* ]]; then
  export PATH="/opt/gradle/gradle-8.7/bin:$PATH"
fi

if [ -d "${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin" ] && [[ ":$PATH:" != *":${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin:"* ]]; then
  export PATH="${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin:$PATH"
fi

if [ -z "${JAVA_HOME:-}" ]; then
  if [ -d "/usr/lib/jvm/java-17-openjdk-amd64" ]; then
    export JAVA_HOME="/usr/lib/jvm/java-17-openjdk-amd64"
  elif command -v javac >/dev/null 2>&1; then
    export JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
  fi
fi

export CCACHE_COMPILERCHECK="${CCACHE_COMPILERCHECK:-content}"

echo "Environment:"
echo "  REPO_ROOT:        ${REPO_ROOT}"
echo "  ANDROID_SDK_ROOT: ${ANDROID_SDK_ROOT}"
echo "  ANDROID_NDK_HOME: ${ANDROID_NDK_HOME}"
echo "  VCPKG_ROOT:       ${VCPKG_ROOT}"
echo "  JAVA_HOME:        ${JAVA_HOME:-not set}"

# Sanity check
if [ ! -d "${ANDROID_SDK_ROOT}" ]; then
  echo "Error: ANDROID_SDK_ROOT directory not found: ${ANDROID_SDK_ROOT}" >&2
  exit 1
fi
if [ ! -d "${ANDROID_NDK_HOME}" ]; then
  echo "Error: ANDROID_NDK_HOME directory not found: ${ANDROID_NDK_HOME}" >&2
  exit 1
fi
if [ ! -x "${VCPKG_ROOT}/vcpkg" ]; then
  echo "Error: vcpkg executable not found in ${VCPKG_ROOT}" >&2
  exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  echo "Error: cmake not found in PATH" >&2
  exit 1
fi
if ! command -v ninja >/dev/null 2>&1; then
  echo "Error: ninja not found in PATH" >&2
  exit 1
fi

# Ensure signing keystore exists
KEYSTORE="${STAR_SIGNING_KEYSTORE:-${REPO_ROOT}/keystore/osbm-release.keystore}"
if [ ! -f "${KEYSTORE}" ]; then
  echo "Generating release keystore at ${KEYSTORE}..."
  mkdir -p "$(dirname "${KEYSTORE}")"
  keytool -genkey -v -keystore "${KEYSTORE}" \
    -storepass android -alias androiddebugkey -keypass android \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=oSBM Release,OU=OpenStarbound,O=oSBM,C=CN"
fi
export STAR_SIGNING_KEYSTORE="${KEYSTORE}"

# Build presets
CCACHE_FLAGS=()
if command -v ccache >/dev/null 2>&1; then
  CCACHE_FLAGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi

mkdir -p "${REPO_ROOT}/dist/android"

cd "${REPO_ROOT}/source"

if [ "${ARCH}" = "universal" ]; then
  PRESETS=("android-armv7-release" "android-arm64-release" "android-x64-release")
  for PRESET in "${PRESETS[@]}"; do
    echo ">>> Configuring ${PRESET}..."
    cmake --preset "${PRESET}" "${CCACHE_FLAGS[@]}"
    echo ">>> Building native target for ${PRESET}..."
    cmake --build --preset "${PRESET}" --target starbound --parallel
  done

  echo ">>> Building universal APK..."
  cmake --build --preset android-universal-release
  OUTPUT_DIR="${REPO_ROOT}/source/application/mobile/android/host/app/build/outputs/apk/release"
else
  PRESET="android-${ARCH}-${SUFFIX}"
  echo ">>> Configuring preset: ${PRESET}..."
  cmake --preset "${PRESET}" "${CCACHE_FLAGS[@]}"
  echo ">>> Building android_apk for ${PRESET}..."
  cmake --build --preset "${PRESET}" --target android_apk --parallel
  OUTPUT_DIR="${REPO_ROOT}/source/application/mobile/android/host/app/build/outputs/apk/${CONFIG_TYPE}"
fi

# Copy built APK(s) to dist/android/
echo ">>> Staging APK(s) to dist/android/..."
cp -vf "${OUTPUT_DIR}"/oSBM-*.apk "${REPO_ROOT}/dist/android/"

# Validate assets
for apk in "${REPO_ROOT}/dist/android"/oSBM-*.apk; do
  if unzip -Z1 "$apk" 2>/dev/null | grep -E '(^|/)packed\.pak$' >/dev/null; then
    echo "Error: Unexpected packed.pak found in $apk!" >&2
    exit 1
  fi
done

echo ""
echo "============================================================"
echo " Build successful! Produced APK(s):"
for apk in "${REPO_ROOT}/dist/android"/oSBM-*.apk; do
  size=$(ls -lh "$apk" | awk '{print $5}')
  sha=$(sha256sum "$apk" | awk '{print $1}')
  echo "  - File:   $apk"
  echo "    Size:   $size"
  echo "    SHA256: $sha"
done
echo "============================================================"
