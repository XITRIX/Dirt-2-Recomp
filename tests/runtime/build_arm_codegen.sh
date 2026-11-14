#!/bin/bash
# Build small GCC/Clang AArch64 ELFs for test_arm_codegen.py (Unicorn host tool).
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${DEVKITPRO:?Set DEVKITPRO to the Switch 2 SDK root}"
export PATH="$DEVKITPRO/devkitA64/bin:$PATH"
OUT=build/host-tests
mkdir -p "$OUT"
FLAGS=(-O2 -std=gnu11 -mcpu=cortex-a78c+crypto -D__SWITCH__ -D__SWITCH2__
       -DRECOMP_TSO=1 -DRECOMP_INLINE_GUEST_MEMORY=1 -fno-strict-aliasing
       -ffp-contract=off -fPIC -Iruntime/include)
aarch64-none-elf-gcc "${FLAGS[@]}" -c tests/runtime/arm_codegen_fixture.c -o "$OUT/arm-fixture-gcc.o"
clang --target=aarch64-none-elf --sysroot="$(aarch64-none-elf-gcc -print-sysroot)" \
    "${FLAGS[@]}" -fno-math-errno -mno-outline-atomics \
    -c tests/runtime/arm_codegen_fixture.c -o "$OUT/arm-fixture-clang.o"
for compiler in gcc clang; do
    aarch64-none-elf-gcc -nostdlib -Wl,-Ttext=0x100000 -Wl,-Tdata=0x200000 \
        -Wl,--entry=arm_codegen_fixture -Wl,--build-id=none \
        "$OUT/arm-fixture-$compiler.o" -o "$OUT/arm-fixture-$compiler.elf"
done
