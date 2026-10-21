#!/usr/bin/env bash
# Include host, compiler/tool versions, SDK, dependency scripts and gitlinks.
set -euo pipefail
platform=${1:?platform}
{
  uname -sm
  printf '%s\n' "${ImageOS:-container}" "${ImageVersion:-unknown}" "$PWD"
  cmake --version
  ninja --version
  meson --version
  python3 --version
  if [ "$(uname -s)" = Darwin ]; then xcodebuild -version; xcrun clang --version; fi
  if [ "$platform" = android ]; then
    cat "$ANDROID_NDK_HOME/source.properties"
    "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --version
  fi
  if [ "$platform" = switch ]; then /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc --version; fi
  git submodule status --recursive
  git ls-files -s third_party
  # Include uncommitted contents too, making local checks match CI hashing.
  find tools/scripts tools/ci third_party/patches -type f -print | LC_ALL=C sort | while IFS= read -r file; do
    printf '%s\n' "$file"
    cat "$file"
  done
} | shasum -a 256 | cut -d' ' -f1
