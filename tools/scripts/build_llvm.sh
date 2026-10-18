#!/bin/bash
# Builds LLVM 15.0.7 (the version DXMT's airconv requires) as static libraries,
# per platform, from the pinned release tarballs:
#   build/llvm/<platform>/{include,lib}   for DXMT (-Dnative_llvm_path=...)
# Only what airconv links: no target backends, no tools beyond llvm-tblgen and
# llvm-config, no zlib/zstd/terminfo/libxml2, no RTTI (airconv builds -fno-rtti).
# Cross builds (iphoneos, iphonesimulator) use the macOS build's llvm-tblgen.
# Platforms: macos, iphoneos, iphonesimulator. Usage: build_llvm.sh [platform ...]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
VER=15.0.7
MIN_IOS=16.0
SRC=$ROOT/build/llvm-src
URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER
# file and SHA-256, one pair per line (macOS's bash 3.2 has no associative arrays)
TARBALLS="llvm-$VER.src.tar.xz 4ad8b2cc8003c86d0078d15d987d84e3a739f24aae9033865c027abae93ee7a4
cmake-$VER.src.tar.xz 8986f29b634fdaa9862eedda78513969fe9788301c9f2d938f4c10a3e7a3e7ea"

mkdir -p "$SRC" "$ROOT/build/llvm"
while read -r f sha; do
    [ -f "$SRC/$f" ] || curl -fsSL -o "$SRC/$f" "$URL/$f"
    echo "$sha  $SRC/$f" | shasum -a 256 -c --quiet || { echo "error: $f checksum mismatch" >&2; exit 1; }
done <<< "$TARBALLS"
if [ ! -d "$SRC/llvm" ]; then
    tar -xJf "$SRC/llvm-$VER.src.tar.xz" -C "$SRC" && mv "$SRC/llvm-$VER.src" "$SRC/llvm"
    tar -xJf "$SRC/cmake-$VER.src.tar.xz" -C "$SRC" && mv "$SRC/cmake-$VER.src" "$SRC/cmake"
fi

for PLATFORM in ${*:-macos}; do
    OUT=$ROOT/build/llvm/$PLATFORM
    WORK=$ROOT/build/llvm/work-$PLATFORM
    [ -f "$OUT/lib/libLLVMCore.a" ] && { echo "== LLVM $PLATFORM: up to date"; continue; }
    ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT"
        -DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_PROJECTS= -DLLVM_ENABLE_RTTI=OFF
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_INCLUDE_TOOLS=ON
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
        -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF
        -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF
        -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF
        -DCMAKE_OSX_ARCHITECTURES=arm64)
    case $PLATFORM in
        macos) ARGS+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0) ;;
        iphoneos | iphonesimulator)
            TBLGEN=$ROOT/build/llvm/work-macos/bin/llvm-tblgen
            [ -x "$TBLGEN" ] || { echo "error: build LLVM for macos first (host llvm-tblgen)" >&2; exit 1; }
            ARGS+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$PLATFORM" -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS
                -DLLVM_TABLEGEN="$TBLGEN" -DLLVM_HOST_TRIPLE=arm64-apple-ios
                -DLLVM_ENABLE_PIC=ON -DLLVM_BUILD_LLVM_DYLIB=OFF) ;;
        *) echo "error: unknown platform $PLATFORM" >&2; exit 1 ;;
    esac
    echo "== LLVM $VER for $PLATFORM"
    cmake -S "$SRC/llvm" -B "$WORK" "${ARGS[@]}" > "$WORK.configure.log" 2>&1 || { tail -30 "$WORK.configure.log" >&2; exit 1; }
    cmake --build "$WORK" --target install > "$WORK.build.log" 2>&1 || { grep -m20 -E "error|FAILED" "$WORK.build.log" >&2; exit 1; }
    echo "LLVM ($PLATFORM): $OUT"
done
