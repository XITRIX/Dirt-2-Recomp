#!/bin/bash
# Primary Switch renderer: DX11 -> DXVK -> NVK. Legacy deko3d packaging remains
# available as tools/scripts/package_switch_deko.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
bash tools/scripts/package_switch_dx11.sh --primary --startup-watchdog \
    --no-disk-shader-cache --fresh-exposure --async-exposure \
    --output-dir "${1:-build/package/switch}"
