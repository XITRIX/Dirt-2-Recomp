#!/bin/sh
# Builds the graphics dependencies for macOS: MoltenVK (Vulkan on Metal) and
# DXVK native d3d9, with the project's patches applied.
set -e
cd "$(dirname "$0")/../.."
tools/scripts/apply_patches.sh
if [ ! -f third_party/MoltenVK/Package/Release/MoltenVK/dylib/macOS/libMoltenVK.dylib ]; then
    (cd third_party/MoltenVK && ./fetchDependencies --macos && make macos)
fi
if [ ! -d build/dxvk-macos ]; then
    (cd third_party/dxvk && meson setup ../../build/dxvk-macos --buildtype=release \
        -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_d3d9=true \
        -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled)
fi
ninja -C build/dxvk-macos
