#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/29.0.14206865}
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo 'Set ANDROID_NDK_HOME to an installed NDK' >&2; exit 1; }
SRC=$ROOT/build/deps/src
PREFIX=$ROOT/build/deps/android-arm64
WORK=$ROOT/build/deps/android-arm64-work
JOBS=${DIRT2_BUILD_JOBS:-6}
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=
export CCACHE_DISABLE=1
mkdir -p "$WORK" "$PREFIX"
if [ ! -d "$SRC/ffmpeg-7.1.1" ] || [ ! -d "$SRC/SDL3-3.4.18" ] || [ ! -d "$SRC/openal-soft-1.25.2" ] || [ ! -d "$SRC/libogg-1.3.5" ] || [ ! -d "$SRC/libvorbis-1.3.7" ]; then
 bash tools/scripts/build_deps.sh --fetch-only
fi
for source in SDL3-3.4.18 openal-soft-1.25.2 libogg-1.3.5 libvorbis-1.3.7 ffmpeg-7.1.1; do
 [ -d "$SRC/$source" ] || { echo "Missing $SRC/$source; fetch pinned sources with build_deps.sh first" >&2; exit 1; }
done
cmake_lib() {
 local name=$1 source=$2
 shift 2
 cmake -S "$SRC/$source" -B "$WORK/$name" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DSDL3_DIR="$PREFIX/lib/cmake/SDL3" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
 cmake --build "$WORK/$name" --target install -j"$JOBS"
}
cmake_lib sdl3 SDL3-3.4.18 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
 -DSDL_VULKAN=ON -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_GPU=OFF -DSDL_RENDER_GPU=OFF
cmake_lib openal openal-soft-1.25.2 -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
 -DALSOFT_TESTS=OFF -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_BACKEND_SDL3=ON -DALSOFT_REQUIRE_SDL3=ON
cmake_lib ogg libogg-1.3.5 -DINSTALL_DOCS=OFF
cmake_lib vorbis libvorbis-1.3.7
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64
if [ ! -f "$WORK/ffmpeg.done" ]; then
 mkdir -p "$WORK/ffmpeg"
 (cd "$WORK/ffmpeg" && "$SRC/ffmpeg-7.1.1/configure" --prefix="$PREFIX" \
  --enable-cross-compile --target-os=android --arch=aarch64 \
  --cc="$TOOLCHAIN/bin/aarch64-linux-android28-clang" \
  --cxx="$TOOLCHAIN/bin/aarch64-linux-android28-clang++" \
  --ar="$TOOLCHAIN/bin/llvm-ar" --ranlib="$TOOLCHAIN/bin/llvm-ranlib" \
  --enable-static --disable-shared --enable-pic --disable-debug --disable-autodetect \
  --disable-programs --disable-doc --disable-everything --disable-network \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-demuxer=bink --enable-protocol=file \
  && make -j"$JOBS" install)
 touch "$WORK/ffmpeg.done"
fi
