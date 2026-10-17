#include "analysis/disasm.h"

namespace recomp {

std::optional<uint32_t> Insn::branch_target() const {
    if (ins.operand_count_visible < 1) return std::nullopt;
    const ZydisDecodedOperand& o = ops[0];
    if (o.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !o.imm.is_relative) return std::nullopt;
    ZyanU64 abs = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &o, addr, &abs))) return std::nullopt;
    return uint32_t(abs);
}

std::optional<uint32_t> Insn::abs_mem(int i) const {
    if (i >= ins.operand_count_visible) return std::nullopt;
    const ZydisDecodedOperand& o = ops[i];
    if (o.type != ZYDIS_OPERAND_TYPE_MEMORY || o.mem.base != ZYDIS_REGISTER_NONE ||
        o.mem.index != ZYDIS_REGISTER_NONE)
        return std::nullopt;
    return uint32_t(o.mem.disp.value);
}

Disasm::Disasm(const PeImage& img) : img_(img) {
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatterInit(&fmt_, ZYDIS_FORMATTER_STYLE_INTEL);
}

bool Disasm::decode(uint32_t addr, Insn& out) const {
    const uint32_t left = img_.bytes_left(addr);
    if (!left) return false;
    out.addr = addr;
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec_, img_.ptr(addr), left > 15 ? 15 : left,
                                               &out.ins, out.ops));
}

void Disasm::format(const Insn& in, char* buf, size_t len) const {
    ZydisFormatterFormatInstruction(&fmt_, &in.ins, in.ops, in.ins.operand_count_visible, buf, len,
                                    in.addr, nullptr);
}

Flow classify(const Insn& in) {
    switch (in.ins.meta.category) {
        case ZYDIS_CATEGORY_CALL: return Flow::Call;
        case ZYDIS_CATEGORY_UNCOND_BR: return Flow::Jump;
        case ZYDIS_CATEGORY_COND_BR: return Flow::CondJump;
        case ZYDIS_CATEGORY_RET: return Flow::Ret;
        default: break;
    }
    switch (in.ins.mnemonic) {
        case ZYDIS_MNEMONIC_INT3:
        case ZYDIS_MNEMONIC_HLT:
        case ZYDIS_MNEMONIC_UD2:
        case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD:
            return Flow::Stop;
        default: return Flow::Normal;
    }
}

}  // namespace recomp
