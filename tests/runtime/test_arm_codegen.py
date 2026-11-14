"""Compare GCC/Clang AArch64 fixture ELFs using Unicorn (optional host tool)."""
import argparse
import io
import json
import random
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_FPCR, UC_ARM64_REG_CPACR_EL1, UC_CPU_ARM64_MAX, UC_ARM64_REG_PC

CTX, GUEST, STACK, STOP = 0x300000, 0x400000, 0x800000, 0x900000

def load(path):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    pages = set()
    for segment in segments:
        lo = segment['p_vaddr'] & ~4095
        hi = (segment['p_vaddr'] + segment['p_memsz'] + 4095) & ~4095
        pages.update(range(lo, hi, 4096))
    for page in sorted(pages):
        uc.mem_map(page, 4096)
    for segment in segments:
        uc.mem_write(segment['p_vaddr'], segment.data())
    symbols = {s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    for address in (CTX, GUEST, STACK, STOP):
        uc.mem_map(address, 65536)
    uc.mem_write(symbols['recomp_mem']['st_value'], struct.pack('<Q', GUEST))
    symbol = symbols['arm_codegen_layout']
    raw = uc.mem_read(symbol['st_value'], symbol['st_size'])
    layout = struct.unpack('<' + 'I'*(len(raw)//4), raw)
    return uc, symbols['arm_codegen_fixture']['st_value'], layout

def execute(engine, address, context, memory, fpcr):
    engine.mem_write(CTX, bytes(context))
    engine.mem_write(GUEST, memory)
    engine.reg_write(UC_ARM64_REG_X0, CTX)
    engine.reg_write(UC_ARM64_REG_SP, STACK + 0xF000)
    engine.reg_write(UC_ARM64_REG_LR, STOP)
    engine.reg_write(UC_ARM64_REG_FPCR, fpcr)
    engine.emu_start(address, STOP, count=20000)
    assert engine.reg_read(UC_ARM64_REG_PC) == STOP, "Fixture did not return"
    return bytearray(engine.mem_read(CTX, len(context))), bytes(engine.mem_read(GUEST, len(memory)))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('gcc', type=Path);ap.add_argument('clang', type=Path)
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    first, entry1, layout = load(args.gcc)
    second, entry2, other = load(args.clang)
    assert layout == other, (layout, other)
    rng = random.Random(0xD172)
    edge = [0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
            0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000,
            0x7fc00001, 0x7fa00002, 0x3f800000, 0xbf800000]
    trials = 0
    # All rounding modes, each with flush-to-zero/default-NaN combinations.
    modes = [rounding << 22 | flags for rounding in range(4)
             for flags in (0, 1 << 24, 1 << 25, (1 << 24) | (1 << 25))]
    for fpcr in modes:
        for trial in range(1024):
            context = bytearray(rng.randbytes(layout[0]))
            struct.pack_into('<I', context, 0, trial & 63)
            for register in (5, 6, 7):
                for lane in range(4):
                    bits = edge[(trial + lane + register) % len(edge)] if trial < 256 else rng.getrandbits(32)
                    struct.pack_into('<I', context, layout[3] + register*16 + lane*4, bits)
            memory = rng.randbytes(512)
            a, am = execute(first, entry1, context, memory, fpcr)
            b, bm = execute(second, entry2, context, memory, fpcr)
            # IEEE arithmetic may select different NaN payloads. Canonicalize
            # only floating output lanes; integer masks and memory stay exact.
            for offset in range(layout[3], layout[3] + 3*16, 4):
                x, y = struct.unpack_from('<I', a, offset)[0], struct.unpack_from('<I', b, offset)[0]
                if all((v & 0x7f800000) == 0x7f800000 and (v & 0x7fffff) for v in (x,y)):
                    a[offset:offset+4] = b[offset:offset+4]
            assert a == b and am == bm, f'GCC/Clang mismatch, FPCR {fpcr:X}, trial {trial}'
            trials += 1
    result = {'passed':True, 'cases':trials, 'layout':layout,
              'coverage':'AArch64 aligned/unaligned guest memory, FP special values, SSE min/max/predicates, FPCR modes',
              'limitation':'Compiler differential fixture; not full gameplay or full x86 instruction-oracle coverage'}
    if args.output: args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__ == '__main__': main()
