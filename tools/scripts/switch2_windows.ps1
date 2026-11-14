param(
    [ValidateSet('libraries','game','cpu-hot-inline','cpu-hot-clang','cpu-render-clang','probe','all','host','lift')][string]$Target = 'probe',
    [string]$GameDirectory,
    [string]$MsysRoot = "$env:USERPROFILE/.dirt2-tools/msys64",
    [string]$DevkitProDirectory = "$PSScriptRoot/../../../devkitPro"
)
$ErrorActionPreference = 'Stop'
$bash = Join-Path $MsysRoot 'usr/bin/bash.exe'
if (!(Test-Path -LiteralPath $bash)) { throw "MSYS2 bash is missing at $bash" }
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
$env:DIRT2_PROJECT_WIN = (Resolve-Path "$PSScriptRoot/../..").Path
$env:DIRT2_DEVKIT_WIN = (Resolve-Path $DevkitProDirectory).Path
$env:DIRT2_BUILD_TARGET = $Target
if ($GameDirectory) { $env:DIRT2_GAME_WIN = (Resolve-Path -LiteralPath $GameDirectory).Path }
else { Remove-Item Env:DIRT2_GAME_WIN -ErrorAction SilentlyContinue }
& $bash -lc '
set -euo pipefail
cd "$(cygpath -u "$DIRT2_PROJECT_WIN")"
export DEVKITPRO=$(cygpath -u "$DIRT2_DEVKIT_WIN")
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:/ucrt64/bin:$PATH"
case "$DIRT2_BUILD_TARGET" in
    host|lift)
        cmake -S . -B build/host-msys -G Ninja -DDIRT2_TOOLS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
        cmake --build build/host-msys --target recomp -j4
        if [ "$DIRT2_BUILD_TARGET" = lift ]; then
            [ -n "${DIRT2_GAME_WIN:-}" ] || { echo "Use -GameDirectory with your PC v1.1 game folder" >&2; exit 1; }
            game=$(cygpath -u "$DIRT2_GAME_WIN")
            build/host-msys/tools/recomp/recomp lift "$game/dirt2_game.exe" --out generated                 --entries tools/recomp/config/entries.txt --hooks tools/recomp/config/hooks.txt --include-dead
        fi ;;
    *) bash tools/scripts/build_switch2.sh "$DIRT2_BUILD_TARGET" ;;
esac
'
if ($LASTEXITCODE -ne 0) { throw "Switch 2 $Target failed with exit $LASTEXITCODE" }
