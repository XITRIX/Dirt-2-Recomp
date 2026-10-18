#!/bin/bash
# Builds the third-party libraries from pinned sources as static libraries, the
# same way for every platform (arm64): macos, iphoneos (device), iphonesimulator.
#   SDL3, OpenAL Soft, libogg, libvorbis, and FFmpeg cut down to Bink.
# Output: build/deps/<platform>/{include,lib,lib/pkgconfig,lib/cmake}, built in
# build/deps/<platform>-work. Platforms never share output or work folders, so
# they can build at the same time; sources (build/deps/src) are fetched once
# under a lock and only read afterwards.
# Usage: build_deps.sh [macos|iphoneos|iphonesimulator ...]   (default: all three)
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SRC=$ROOT/build/deps/src
MIN_IOS=16.0
MIN_MACOS=12.0
SDKS=${*:-macos iphoneos iphonesimulator}
JOBS=$(sysctl -n hw.ncpu)

SDL_VER=3.4.18
OPENAL_VER=1.25.2
OGG_VER=1.3.5
VORBIS_VER=1.3.7
FFMPEG_VER=7.1.1

fetch() {  # url dir [tar flags]
    [ -d "$SRC/$2" ] && return
    echo "fetching $1"
    curl -fsSL "$1" | tar -x -C "$SRC.tmp" ${3:-}
    mv "$SRC.tmp/$2" "$SRC/$2"
}
mkdir -p "$SRC" "$SRC.tmp"
until mkdir "$SRC.lock" 2>/dev/null; do sleep 2; done
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz" "SDL3-$SDL_VER"
fetch "https://github.com/kcat/openal-soft/archive/refs/tags/$OPENAL_VER.tar.gz" "openal-soft-$OPENAL_VER"
fetch "https://downloads.xiph.org/releases/ogg/libogg-$OGG_VER.tar.gz" "libogg-$OGG_VER"
fetch "https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS_VER.tar.gz" "libvorbis-$VORBIS_VER"
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" "ffmpeg-$FFMPEG_VER" -J
rmdir "$SRC.lock"

for SDK in $SDKS; do
    PREFIX=$ROOT/build/deps/$SDK
    BLD=$ROOT/build/deps/$SDK-work
    mkdir -p "$PREFIX" "$BLD"
    case $SDK in
        macos) TRIPLE=arm64-apple-macos$MIN_MACOS XSDK=macosx
            PLATFORM_ARGS=(-DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_MACOS) ;;
        iphoneos) TRIPLE=arm64-apple-ios$MIN_IOS XSDK=iphoneos
            PLATFORM_ARGS=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS) ;;
        iphonesimulator) TRIPLE=arm64-apple-ios$MIN_IOS-simulator XSDK=iphonesimulator
            PLATFORM_ARGS=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS) ;;
        *) echo "unknown platform $SDK" >&2; exit 1 ;;
    esac
    SYSROOT=$(xcrun --sdk "$XSDK" --show-sdk-path)
    # Only this platform's own libraries are visible to configure steps: nothing
    # from the host (Homebrew, /usr/local) may leak in.
    export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=

    cmake_lib() {  # name srcdir [cmake args...]
        local name=$1 src=$2
        shift 2
        [ -f "$BLD/$name.done" ] && return
        echo "== $name ($SDK)"
        cmake -S "$src" -B "$BLD/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            "${PLATFORM_ARGS[@]}" -DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_FRAMEWORK=LAST \
            -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
            -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DBUILD_SHARED_LIBS=OFF "$@" > "$BLD/$name.log"
        cmake --build "$BLD/$name" --target install >> "$BLD/$name.log"
        touch "$BLD/$name.done"
    }

    cmake_lib sdl3 "$SRC/SDL3-$SDL_VER" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
    cmake_lib openal "$SRC/openal-soft-$OPENAL_VER" -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
        -DALSOFT_TESTS=OFF -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF \
        -DHAVE_WFUNCTION_EFFECTS=OFF  # new clang flags coreaudio.cpp, and OpenAL makes it an error
    cmake_lib ogg "$SRC/libogg-$OGG_VER" -DINSTALL_DOCS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake_lib vorbis "$SRC/libvorbis-$VORBIS_VER" -DCMAKE_POLICY_VERSION_MINIMUM=3.5

    if [ ! -f "$BLD/ffmpeg.done" ]; then
        echo "== ffmpeg ($SDK)"
        rm -rf "$BLD/ffmpeg" && mkdir -p "$BLD/ffmpeg"
        (cd "$BLD/ffmpeg" && "$SRC/ffmpeg-$FFMPEG_VER/configure" --prefix="$PREFIX" \
            --enable-cross-compile --target-os=darwin --arch=aarch64 \
            --cc="xcrun --sdk $XSDK clang" --sysroot="$SYSROOT" \
            --extra-cflags="-target $TRIPLE" --extra-ldflags="-target $TRIPLE" \
            --enable-static --disable-shared --enable-pic --disable-debug --disable-autodetect \
            --disable-programs --disable-doc --disable-everything --disable-network \
            --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
            --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-demuxer=bink --enable-protocol=file \
            > "$BLD/ffmpeg.log" && make -j"$JOBS" install >> "$BLD/ffmpeg.log" 2>&1)
        touch "$BLD/ffmpeg.done"
    fi
    echo "$SDK: $(ls "$PREFIX/lib"/*.a | wc -l | tr -d ' ') static libraries in $PREFIX/lib"
done
