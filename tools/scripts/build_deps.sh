#!/bin/bash
# Builds the third-party libraries from pinned sources as static libraries, the
# same way for every platform (arm64): macos, iphoneos (device), iphonesimulator,
# switch (Nintendo Switch homebrew: devkitA64 + libnx, DEVKITPRO, default /opt/devkitpro).
#   SDL3, OpenAL Soft, libogg, libvorbis, and FFmpeg cut down to Bink.
# On switch SDL3 is built from a copy with third_party/patches/sdl3 applied (the
# devkitPro SDL3 Switch port, see third_party/patches/README.md).
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
JOBS=${DIRT2_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}

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

# Android uses the same pinned inputs with a separate NDK build script.
if [ "${1:-}" = "--fetch-only" ]; then exit 0; fi

for SDK in $SDKS; do
    PREFIX=$ROOT/build/deps/$SDK
    BLD=$ROOT/build/deps/$SDK-work
    mkdir -p "$PREFIX" "$BLD"
    SDL_SRC=$SRC/SDL3-$SDL_VER
    OPENAL_ARGS=()
    case $SDK in
        macos) TRIPLE=arm64-apple-macos$MIN_MACOS XSDK=macosx
            PLATFORM_ARGS=(-DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_MACOS) ;;
        iphoneos) TRIPLE=arm64-apple-ios$MIN_IOS XSDK=iphoneos
            PLATFORM_ARGS=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS) ;;
        iphonesimulator) TRIPLE=arm64-apple-ios$MIN_IOS-simulator XSDK=iphonesimulator
            PLATFORM_ARGS=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS) ;;
        switch) XSDK=
            export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
            [ -f "$DEVKITPRO/cmake/Switch.cmake" ] || { echo "devkitPro with libnx not found in $DEVKITPRO" >&2; exit 1; }
            PLATFORM_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake")
            # Output through SDL3's audio (audren); no RTKit/dbus on Horizon. GCC's
            # C++20 module scan emits one OpenAL source twice, so no modules.
            OPENAL_ARGS=(-DALSOFT_BACKEND_SDL3=ON -DALSOFT_REQUIRE_SDL3=ON -DALSOFT_RTKIT=OFF -DALSOFT_BACKEND_WAVE=OFF
                -DALSOFT_ENABLE_MODULES=OFF) ;;
        *) echo "unknown platform $SDK" >&2; exit 1 ;;
    esac
    if [ -n "$XSDK" ]; then
        SYSROOT=$(xcrun --sdk "$XSDK" --show-sdk-path)
        PLATFORM_ARGS+=(-DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_FIND_FRAMEWORK=LAST)
        # Only this platform's own libraries are visible to configure steps: nothing
        # from the host (Homebrew, /usr/local) may leak in.
        export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=
    else
        # libnx's portlibs (EGL, Mesa's GL, zlib) are the platform's SDK here.
        export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$DEVKITPRO/portlibs/switch/lib/pkgconfig PKG_CONFIG_PATH=
        # SDL3 with the Switch port: a copy of the pinned source, patched;
        # prepared again when the patches change.
        hash=$(cat "$ROOT"/third_party/patches/sdl3/*.patch | shasum | cut -d' ' -f1)
        SDL_SRC=$BLD/SDL3-src
        if [ "$(cat "$SDL_SRC.hash" 2>/dev/null)" != "$hash" ]; then
            rm -rf "$SDL_SRC" "$BLD/sdl3" "$BLD/sdl3.done" && cp -R "$SRC/SDL3-$SDL_VER" "$SDL_SRC"
            for p in "$ROOT"/third_party/patches/sdl3/*.patch; do patch -s -p1 -d "$SDL_SRC" < "$p"; done
            echo "$hash" > "$SDL_SRC.hash"
        fi
    fi

    cmake_lib() {  # name srcdir [cmake args...]
        local name=$1 src=$2
        shift 2
        [ -f "$BLD/$name.done" ] && return
        echo "== $name ($SDK)"
        cmake -S "$src" -B "$BLD/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            "${PLATFORM_ARGS[@]}" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
            -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
            -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DBUILD_SHARED_LIBS=OFF "$@" > "$BLD/$name.log"
        cmake --build "$BLD/$name" --target install >> "$BLD/$name.log"
        touch "$BLD/$name.done"
    }

    cmake_lib sdl3 "$SDL_SRC" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
    cmake_lib openal "$SRC/openal-soft-$OPENAL_VER" -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
        -DALSOFT_TESTS=OFF -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF \
        -DHAVE_WFUNCTION_EFFECTS=OFF `# new clang flags coreaudio.cpp, and OpenAL makes it an error` \
        "${OPENAL_ARGS[@]}"
    cmake_lib ogg "$SRC/libogg-$OGG_VER" -DINSTALL_DOCS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake_lib vorbis "$SRC/libvorbis-$VORBIS_VER" -DCMAKE_POLICY_VERSION_MINIMUM=3.5

    if [ ! -f "$BLD/ffmpeg.done" ]; then
        echo "== ffmpeg ($SDK)"
        rm -rf "$BLD/ffmpeg" && mkdir -p "$BLD/ffmpeg"
        if [ -n "$XSDK" ]; then
            FF_TARGET=(--target-os=darwin --cc="xcrun --sdk $XSDK clang" --sysroot="$SYSROOT"
                --extra-cflags="-target $TRIPLE" --extra-ldflags="-target $TRIPLE")
        else
            # libnx's compiler flags (switch_rules); configure links test programs with its specs.
            NX_ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
            FF_TARGET=(--target-os=none --cross-prefix="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-"
                --extra-cflags="-D__SWITCH__ $NX_ARCH -ftls-model=local-exec -isystem $DEVKITPRO/libnx/include"
                --extra-ldflags="-specs=$DEVKITPRO/libnx/switch.specs $NX_ARCH -L$DEVKITPRO/libnx/lib" --extra-libs="-lnx")
        fi
        (cd "$BLD/ffmpeg" && "$SRC/ffmpeg-$FFMPEG_VER/configure" --prefix="$PREFIX" \
            --enable-cross-compile --arch=aarch64 "${FF_TARGET[@]}" \
            --enable-static --disable-shared --enable-pic --disable-debug --disable-autodetect \
            --disable-programs --disable-doc --disable-everything --disable-network \
            --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
            --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-demuxer=bink --enable-protocol=file \
            > "$BLD/ffmpeg.log" && make -j"$JOBS" install >> "$BLD/ffmpeg.log" 2>&1)
        touch "$BLD/ffmpeg.done"
    fi
    echo "$SDK: $(ls "$PREFIX/lib"/*.a | wc -l | tr -d ' ') static libraries in $PREFIX/lib"
done
