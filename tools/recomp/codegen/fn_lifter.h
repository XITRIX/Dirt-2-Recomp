// Per-function C emitter. Split across lift_base.cpp (integer, control
// flow), lift_sse.cpp and lift_x87.cpp.
#pragma once

#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/discovery.h"
#include "codegen/lifter.h"

namespace recomp {

// Flag bits use the x86 EFLAGS layout, matching ZYDIS_CPUFLAG_*.
enum : uint32_t {
    F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4, F_ZF = 1u << 6, F_SF = 1u << 7, F_OF = 1u << 11,
    F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF,
};

enum class FuseKind : uint8_t { None, Cmp, Logic, Result };

struct LInsn {
    Insn in;
    uint32_t reads = 0, writes = 0;  // arithmetic flags tested / defined
    uint32_t live_out = 0;           // flags needed after this instruction
    FuseKind fuse_src = FuseKind::None;  // this writer feeds a fused reader
    int fused_from = -1;             // index of the writer this reader fuses with
};

struct LBlock {
    const Block* b = nullptr;
    std::vector<LInsn> insns;
    uint32_t live_in = 0, live_out = 0;
};

// Condition codes in x86 encoding order.
enum CC : int { CC_O, CC_NO, CC_B, CC_NB, CC_Z, CC_NZ, CC_BE, CC_NBE, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_NL, CC_LE, CC_NLE };
int cc_of(ZydisMnemonic m);  // -1 if not a conditional mnemonic

class FnLifter {
public:
    FnLifter(const PeImage& img, const Disasm& dis, const Discovery& disc, const Function& f,
             const std::set<uint32_t>& lifted, LiftStats& stats, bool resumable = false);
    std::string emit();

    // ---- helpers shared by the lift_* files ----
    template <typename... A>
    void line(std::format_string<A...> fmt, A&&... args) {
        out_ += indent_;
        out_ += std::format(fmt, std::forward<A>(args)...);
        out_ += '\n';
    }
    void raw(const std::string& s) { out_ += indent_ + s + '\n'; }

    static std::string fn_name(uint32_t a) { return std::format("f_{:08X}", a); }
    std::string import_fn(uint32_t iat) const;

    // Operand access. Memory operands use the address cached by begin_mem().
    std::string ea(const ZydisDecodedOperand& op) const;
    std::string rd(const ZydisDecodedOperand& op);
    std::string wr(const ZydisDecodedOperand& op, const std::string& v);
    std::string gpr(ZydisRegister r) const;            // value expression
    std::string set_gpr(ZydisRegister r, const std::string& v) const;
    void begin_mem(const Insn& in);                    // caches operand addresses in ea0..ea2
    static int bits(const ZydisDecodedOperand& op) { return op.size; }
    static std::string ty(int b);
    static std::string sty(int b);
    static std::string hex(uint64_t v) { return std::format("0x{:X}u", v); }

    // Flags.
    void flags_logic(const std::string& r, int b, uint32_t need);
    void flags_add(const std::string& a, const std::string& bb, const std::string& r, int b, uint32_t need,
                   const std::string& carry = "");
    void flags_sub(const std::string& a, const std::string& bb, const std::string& r, int b, uint32_t need,
                   const std::string& borrow = "");
    void flags_szp(const std::string& r, int b, uint32_t need);
    std::string cond(int cc, const LInsn& li) const;

    void unimpl(const Insn& in);

    bool lift_base(LInsn& li);
    bool lift_sse(LInsn& li);
    bool lift_x87(LInsn& li);

    const PeImage& img_;
    const Disasm& dis_;
    const Discovery& disc_;
    const Function& f_;
    const std::set<uint32_t>& lifted_;
    LiftStats& stats_;
    bool resumable_ = false;  // has SEH continuation landing pad
    // Test mode: treat every arithmetic flag as live at `ret` and spill them to
    // c->eflags before returning, so differential tests can observe all flags.
    bool flags_live_out = false;
    std::vector<LBlock> blocks_;
    std::unordered_map<uint32_t, size_t> block_index_;
    std::string out_, indent_ = "    ";
    // Current instruction context.
    LBlock* cur_block_ = nullptr;
    size_t cur_index_ = 0;
    bool mem_cached_[3] = {};
    int cur_width_ = 32;  // operand width of the instruction being lifted (sign-extended immediates)

private:
    void decode_blocks();
    void fuse();
    void liveness();
    void emit_block(size_t bi);
    void emit_terminator(LBlock& lb, size_t bi);
    void emit_exit_to(uint32_t target);  // goto, tail call or dispatch
    void emit_call(const Insn& in);
};

}  // namespace recomp
