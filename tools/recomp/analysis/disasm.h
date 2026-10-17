// Thin wrapper around Zydis for 32-bit x86 decoding against a PeImage.
#pragma once

#include <Zydis/Zydis.h>

#include <cstdint>
#include <optional>

#include "pe/pe_image.h"

namespace recomp {

struct Insn {
    uint32_t addr = 0;
    ZydisDecodedInstruction ins{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};

    uint32_t next() const { return addr + ins.length; }
    ZydisMnemonic mnem() const { return ins.mnemonic; }
    const ZydisDecodedOperand& op(int i) const { return ops[i]; }
    int nops() const { return ins.operand_count_visible; }

    // Absolute target of a relative branch/call, if the first operand is one.
    std::optional<uint32_t> branch_target() const;
    // Absolute address of a [disp32] memory operand with no base/index, e.g. call [iat].
    std::optional<uint32_t> abs_mem(int i) const;
};

class Disasm {
public:
    explicit Disasm(const PeImage& img);
    bool decode(uint32_t addr, Insn& out) const;
    // Formats in Intel syntax for debugging and comments.
    void format(const Insn& in, char* buf, size_t len) const;

private:
    const PeImage& img_;
    ZydisDecoder dec_{};
    ZydisFormatter fmt_{};
};

enum class Flow { Normal, Call, Jump, CondJump, Ret, Stop };
Flow classify(const Insn& in);

}  // namespace recomp
