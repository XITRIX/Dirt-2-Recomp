#!/bin/sh
# Re-lift, rebuild and run the game headless with a time limit. Usage: tools/scripts/relift.sh [seconds] [extra env]
set -e
cd "$(dirname "$0")/../.."
GAME="$PWD/Colin McRae DiRT 2 (v.1.1) [Steam-Rip]/Dirt 2"
cmake --build build/macos-arm64 --target recomp >/dev/null
./build/macos-arm64/tools/recomp/recomp lift "$GAME/dirt2_game.exe" --out generated --entries tools/recomp/config/entries.txt --include-dead | grep -E 'lifted|unimpl'
cmake --build build/macos-arm64 2>&1 | grep -E 'error|undefined' || true
mkdir -p build/run && cd build/run
DIRT2_DATA_DIR=$PWD/data perl -e 'alarm shift; exec @ARGV' "${1:-60}" ../macos-arm64/dirt2 --game "$GAME" > log.txt 2>&1 || echo "exit $?"
