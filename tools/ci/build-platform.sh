#!/usr/bin/env bash
# Shared matrix entry point; target outputs remain isolated from shared host tools.
set -euo pipefail
cd "$(dirname "$0")/../.."
platform=${1:?platform}
stage=${2:?libraries or game}
export DIRT2_GAME_DIR="$PWD/build/ci-game"
case "$platform:$stage" in
  macos:libraries|ios:libraries)
    target=macos
    [ "$platform" != ios ] || target=iphoneos
    bash tools/scripts/build_deps.sh "$target"
    bash tools/scripts/build_graphics.sh "$target"
    bash tools/scripts/build_llvm.sh "$target"
    bash tools/scripts/build_dxmt.sh "$target"
    ;;
  macos:game)
    cmake --preset macos-arm64 -DDIRT2_BUILD_TOOLS=OFF -DDIRT2_GAME_DIR="$DIRT2_GAME_DIR"
    cmake --build build/macos-arm64 -j"${DIRT2_BUILD_JOBS:-6}"
    bash tools/scripts/package_macos.sh build/package/macos "$DIRT2_GAME_DIR"
    ;;
  ios:game)
    cmake --preset ios -DDIRT2_GAME_DIR="$DIRT2_GAME_DIR"
    cmake --build build/ios -j"${DIRT2_BUILD_JOBS:-6}"
    bash tools/scripts/package_ios.sh unsigned
    ;;
  android:libraries|android:game) bash tools/scripts/build_android.sh "$stage" ;;
  switch:libraries|switch:game) bash tools/scripts/build_switch_dx11.sh "$stage" ;;
  *) echo "Unsupported platform/stage: $platform/$stage" >&2; exit 2 ;;
esac
