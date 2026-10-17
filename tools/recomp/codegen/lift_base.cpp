// Integer, stack, string and system instructions.
#include "codegen/fn_lifter.h"

namespace recomp {

namespace {

int string_size(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_LODSB:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_SCASB: return 1;
        case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_LODSW:
        case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_SCASW: return 2;
        default: return 4;
    }
}

}  // namespace

bool FnLifter::lift_base(LInsn& li) {
    const Insn& in = li.in;
    const uint32_t need = li.live_out & li.writes;
    const ZydisMnemonic m = in.mnem();
    const bool base_isa = in.ins.meta.isa_ext != ZYDIS_ISA_EXT_SSE2 && in.ins.meta.isa_ext != ZYDIS_ISA_EXT_SSE;
    const int n = in.nops();
    const ZydisDecodedOperand* o0 = n > 0 ? &in.op(0) : nullptr;
    const ZydisDecodedOperand* o1 = n > 1 ? &in.op(1) : nullptr;
    const int b = o0 ? o0->size : 32;
    const std::string T = ty(b), S = sty(b);
    const uint64_t signbit = 1ull << (b - 1);

    switch (m) {
        case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_PREFETCHT0:
        case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2: case ZYDIS_MNEMONIC_PREFETCHNTA:
        case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_SFENCE:
            return true;
        case ZYDIS_MNEMONIC_MFENCE:
            line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
            return true;

        case ZYDIS_MNEMONIC_MOV:
            if (o1->type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o1->reg.value) == ZYDIS_REGCLASS_SEGMENT) {
                raw(wr(*o0, "0x2Bu"));
                return true;
            }
            if (o0->type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o0->reg.value) == ZYDIS_REGCLASS_SEGMENT)
                return true;
            raw(wr(*o0, rd(*o1)));
            return true;
        case ZYDIS_MNEMONIC_MOVZX:
            raw(wr(*o0, std::format("({})({})", T, rd(*o1))));
            return true;
        case ZYDIS_MNEMONIC_MOVSX:
            raw(wr(*o0, std::format("({})({})({})", T, S, std::format("({}){}", sty(o1->size), rd(*o1)))));
            return true;
        case ZYDIS_MNEMONIC_LEA:
            raw(wr(*o0, ea(*o1)));
            return true;

        case ZYDIS_MNEMONIC_PUSH:
            if (o0->type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o0->reg.value) == ZYDIS_REGCLASS_SEGMENT) {
                line("push32(c, 0x2Bu);");
                return true;
            }
            if (b == 16) {
                line("const uint16_t v = {}; c->esp -= 2; wr16(c->esp, v);", rd(*o0));
                return true;
            }
            line("const uint32_t v = {}; push32(c, v);", rd(*o0));
            return true;
        case ZYDIS_MNEMONIC_POP:
            if (o0->type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o0->reg.value) == ZYDIS_REGCLASS_SEGMENT) {
                line("c->esp += 4;");
                return true;
            }
            line("const uint32_t v = pop32(c);");
            mem_cached_[0] = false;  // [esp+x] destinations see the incremented esp
            raw(wr(*o0, "v"));
            return true;
        case ZYDIS_MNEMONIC_PUSHAD:
            line("const uint32_t s = c->esp;");
            line("push32(c, c->eax); push32(c, c->ecx); push32(c, c->edx); push32(c, c->ebx);");
            line("push32(c, s); push32(c, c->ebp); push32(c, c->esi); push32(c, c->edi);");
            return true;
        case ZYDIS_MNEMONIC_POPAD:
            line("c->edi = pop32(c); c->esi = pop32(c); c->ebp = pop32(c); c->esp += 4;");
            line("c->ebx = pop32(c); c->edx = pop32(c); c->ecx = pop32(c); c->eax = pop32(c);");
            return true;
        case ZYDIS_MNEMONIC_PUSHFD:
            line("push32(c, recomp_pack_flags(cf, pf, af, zf, sf, of, c->df) | 0x200u);");
            return true;
        case ZYDIS_MNEMONIC_POPFD:
            line("const uint32_t v = pop32(c);");
            line("cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1;");
            line("of = (v >> 11) & 1; c->df = (v >> 10) & 1;");
            return true;
        case ZYDIS_MNEMONIC_LEAVE:
            line("c->esp = c->ebp; c->ebp = pop32(c);");
            return true;
        case ZYDIS_MNEMONIC_XCHG:
            if (o0->type == ZYDIS_OPERAND_TYPE_MEMORY || o1->type == ZYDIS_OPERAND_TYPE_MEMORY) {
                // Atomic like the locked x86 instruction.
                const ZydisDecodedOperand& mem = o0->type == ZYDIS_OPERAND_TYPE_MEMORY ? *o0 : *o1;
                const ZydisDecodedOperand& reg = o0->type == ZYDIS_OPERAND_TYPE_MEMORY ? *o1 : *o0;
                line("const {} old = xchg{}({}, {});", T, b, ea(mem), rd(reg));
                raw(wr(reg, "old"));
                return true;
            }
            line("const {0} a = {1}, b = {2};", T, rd(*o0), rd(*o1));
            raw(wr(*o0, "b"));
            raw(wr(*o1, "a"));
            return true;
        case ZYDIS_MNEMONIC_BSWAP:
            raw(wr(*o0, std::format("__builtin_bswap32({})", rd(*o0))));
            return true;

        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_ADC:
            line("const {0} a = {1}, b = {2};", T, rd(*o0), rd(*o1));
            line("const {} r = ({})(a + b{});", T, T, m == ZYDIS_MNEMONIC_ADC ? " + cf" : "");
            raw(wr(*o0, "r"));
            flags_add("a", "b", "r", b, need, m == ZYDIS_MNEMONIC_ADC ? "cf" : "");
            return true;
        case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP:
            line("const {0} a = {1}, b = {2};", T, rd(*o0), rd(*o1));
            line("const {} r = ({})(a - b{});", T, T, m == ZYDIS_MNEMONIC_SBB ? " - cf" : "");
            if (m != ZYDIS_MNEMONIC_CMP) raw(wr(*o0, "r"));
            flags_sub("a", "b", "r", b, need, m == ZYDIS_MNEMONIC_SBB ? "cf" : "");
            return true;
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST: {
            const char* op = m == ZYDIS_MNEMONIC_OR ? "|" : m == ZYDIS_MNEMONIC_XOR ? "^" : "&";
            if (m == ZYDIS_MNEMONIC_XOR && o1->type == ZYDIS_OPERAND_TYPE_REGISTER &&
                o0->type == ZYDIS_OPERAND_TYPE_REGISTER && o0->reg.value == o1->reg.value) {
                line("const {} r = 0;", T);
            } else {
                line("const {0} a = {1}, b = {2};", T, rd(*o0), rd(*o1));
                line("const {} r = ({})(a {} b);", T, T, op);
            }
            if (m != ZYDIS_MNEMONIC_TEST) raw(wr(*o0, "r"));
            flags_logic("r", b, need);
            return true;
        }
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: {
            const bool inc = m == ZYDIS_MNEMONIC_INC;
            line("const {0} a = {1}; const {0} r = ({0})(a {2} 1);", T, rd(*o0), inc ? "+" : "-");
            raw(wr(*o0, "r"));
            if (need & F_OF) line("of = {} == {};", inc ? "r" : "a", hex(signbit));
            if (need & F_AF) line("af = ((a ^ 1 ^ r) >> 4) & 1;");
            flags_szp("r", b, need);
            return true;
        }
        case ZYDIS_MNEMONIC_NEG:
            line("const {0} a = {1}; const {0} r = ({0})(0 - a);", T, rd(*o0));
            raw(wr(*o0, "r"));
            if (need & F_CF) line("cf = a != 0;");
            if (need & F_OF) line("of = a == {};", hex(signbit));
            if (need & F_AF) line("af = ((a ^ r) >> 4) & 1;");
            flags_szp("r", b, need);
            return true;
        case ZYDIS_MNEMONIC_NOT:
            raw(wr(*o0, std::format("({})~{}", T, rd(*o0))));
            return true;

        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
        case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR: {
            std::string cnt;
            if (o1->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                const uint32_t k = uint32_t(o1->imm.value.u) & 31;
                if (!k) return true;
                cnt = std::to_string(k);
                line("const uint32_t n = {};", cnt);
            } else {
                line("const uint32_t n = c->ecx & 31;");
            }
            line("if (n) {{");
            line("    const {0} a = {1};", T, rd(*o0));
            switch (m) {
                case ZYDIS_MNEMONIC_SHL:
                    line("    const {0} r = ({0})((uint32_t)a << n);", T);
                    if (need & F_CF) line("    cf = n <= {} ? ((uint32_t)a >> ({} - n)) & 1 : 0;", b, b);
                    if (need & F_OF) line("    of = ((r >> {}) & 1) ^ (n <= {} ? ((uint32_t)a >> ({} - n)) & 1 : 0);", b - 1, b, b);
                    break;
                case ZYDIS_MNEMONIC_SHR:
                    line("    const {0} r = ({0})((uint32_t)a >> n);", T);
                    if (need & F_CF) line("    cf = ((uint32_t)a >> (n - 1)) & 1;");
                    if (need & F_OF) line("    of = (a >> {}) & 1;", b - 1);
                    break;
                case ZYDIS_MNEMONIC_SAR:
                    line("    const {0} r = ({0})((int32_t)({1})a >> n);", T, S);
                    if (need & F_CF) line("    cf = ((int32_t)({})a >> (n - 1)) & 1;", S);
                    if (need & F_OF) line("    of = 0;");
                    break;
                case ZYDIS_MNEMONIC_ROL:
                    line("    const uint32_t k = n % {};", b);
                    line("    const {0} r = ({0})(k ? (((uint32_t)a << k) | ((uint32_t)a >> ({1} - k))) : a);", T, b);
                    if (need & F_CF) line("    cf = r & 1;");
                    if (need & F_OF) line("    of = ((r >> {}) & 1) ^ (r & 1);", b - 1);
                    break;
                case ZYDIS_MNEMONIC_ROR:
                    line("    const uint32_t k = n % {};", b);
                    line("    const {0} r = ({0})(k ? (((uint32_t)a >> k) | ((uint32_t)a << ({1} - k))) : a);", T, b);
                    if (need & F_CF) line("    cf = (r >> {}) & 1;", b - 1);
                    if (need & F_OF) line("    of = ((r >> {}) ^ (r >> {})) & 1;", b - 1, b - 2);
                    break;
                case ZYDIS_MNEMONIC_RCL:
                    line("    {} r = a; uint32_t cy = cf;", T);
                    line("    for (uint32_t i = 0; i < n; ++i) {{ const uint32_t hi = (r >> {}) & 1; r = ({})((r << 1) | cy); cy = hi; }}", b - 1, T);
                    line("    cf = cy; of = ((r >> {}) & 1) ^ cy;", b - 1);
                    break;
                default:  // RCR
                    line("    {} r = a; uint32_t cy = cf;", T);
                    line("    of = ((a >> {}) & 1) ^ cy;", b - 1);
                    line("    for (uint32_t i = 0; i < n; ++i) {{ const uint32_t lo = r & 1; r = ({})((r >> 1) | (cy << {})); cy = lo; }}", T, b - 1);
                    line("    cf = cy;");
                    break;
            }
            indent_ += "    ";
            raw(wr(*o0, "r"));
            if (m == ZYDIS_MNEMONIC_SHL || m == ZYDIS_MNEMONIC_SHR || m == ZYDIS_MNEMONIC_SAR) {
                if (need & F_AF) line("af = 0;");
                flags_szp("r", b, need);
            }
            indent_.resize(indent_.size() - 4);
            line("}}");
            return true;
        }
        case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD: {
            const ZydisDecodedOperand& o2 = in.op(2);
            if (o2.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) line("const uint32_t n = {}u;", uint32_t(o2.imm.value.u) & 31);
            else line("const uint32_t n = c->ecx & 31;");
            line("if (n) {{");
            line("    const uint32_t a = {}, s = {};", rd(*o0), rd(*o1));
            if (b == 16) {
                // Counts above 16 are undefined; shift the a:s (or s:a) concatenation.
                if (m == ZYDIS_MNEMONIC_SHLD) {
                    line("    const uint32_t x = (a << 16) | s;");
                    line("    const uint32_t r = (uint16_t)((x << n) >> 16);");
                    if (need & F_CF) line("    cf = (x >> (32 - n)) & 1;");
                } else {
                    line("    const uint32_t x = (s << 16) | a;");
                    line("    const uint32_t r = (uint16_t)(x >> n);");
                    if (need & F_CF) line("    cf = (x >> (n - 1)) & 1;");
                }
            } else if (m == ZYDIS_MNEMONIC_SHLD) {
                line("    const uint32_t r = (a << n) | (s >> (32 - n));");
                if (need & F_CF) line("    cf = (a >> (32 - n)) & 1;");
            } else {
                line("    const uint32_t r = (a >> n) | (s << (32 - n));");
                if (need & F_CF) line("    cf = (a >> (n - 1)) & 1;");
            }
            if (need & F_OF) line("    of = ((a ^ r) >> {}) & 1;", b - 1);
            indent_ += "    ";
            raw(wr(*o0, "r"));
            flags_szp("r", b, need);
            indent_.resize(indent_.size() - 4);
            line("}}");
            return true;
        }

        case ZYDIS_MNEMONIC_IMUL:
            if (n == 1) {
                if (b == 32) {
                    line("const int64_t p = (int64_t)(int32_t)c->eax * (int32_t){};", rd(*o0));
                    line("c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32);");
                    if (need & (F_CF | F_OF)) line("cf = of = p != (int64_t)(int32_t)p;");
                } else if (b == 16) {
                    line("const int32_t p = (int32_t)(int16_t)c->eax * (int16_t){};", rd(*o0));
                    line("SET_LO16(c->eax, p); SET_LO16(c->edx, (uint32_t)p >> 16);");
                    if (need & (F_CF | F_OF)) line("cf = of = p != (int32_t)(int16_t)p;");
                } else {
                    line("const int32_t p = (int32_t)(int8_t)c->eax * (int8_t){};", rd(*o0));
                    line("SET_LO16(c->eax, p);");
                    if (need & (F_CF | F_OF)) line("cf = of = p != (int32_t)(int8_t)p;");
                }
            } else {
                const std::string x = n == 3 ? rd(*o1) : rd(*o0);
                const std::string y = n == 3 ? rd(in.op(2)) : rd(*o1);
                line("const int64_t p = (int64_t)({0})({1}) * (int64_t)({0})({2});", S, x, y);
                raw(wr(*o0, std::format("({})p", T)));
                if (need & (F_CF | F_OF)) line("cf = of = p != (int64_t)({})p;", S);
            }
            if (need & F_ZF) line("zf = 0;");
            if (need & F_SF) line("sf = 0;");
            if (need & F_PF) line("pf = 0;");
            if (need & F_AF) line("af = 0;");
            return true;
        case ZYDIS_MNEMONIC_MUL:
            if (b == 32) {
                line("const uint64_t p = (uint64_t)c->eax * {};", rd(*o0));
                line("c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32);");
                if (need & (F_CF | F_OF)) line("cf = of = c->edx != 0;");
            } else if (b == 16) {
                line("const uint32_t p = (uint32_t)(uint16_t)c->eax * {};", rd(*o0));
                line("SET_LO16(c->eax, p); SET_LO16(c->edx, p >> 16);");
                if (need & (F_CF | F_OF)) line("cf = of = (p >> 16) != 0;");
            } else {
                line("const uint32_t p = (uint32_t)(uint8_t)c->eax * {};", rd(*o0));
                line("SET_LO16(c->eax, p);");
                if (need & (F_CF | F_OF)) line("cf = of = (p >> 8) != 0;");
            }
            if (need & F_ZF) line("zf = 0;");
            if (need & F_SF) line("sf = 0;");
            if (need & F_PF) line("pf = 0;");
            return true;
        case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: {
            const bool s = m == ZYDIS_MNEMONIC_IDIV;
            line("const {} d = ({}){};", s ? S : T, s ? S : T, rd(*o0));
            line("if (d == 0) {{ recomp_div_error(c, {}); return; }}", hex(in.addr));
            if (b == 32) {
                if (s) {
                    line("const int64_t x = (int64_t)(((uint64_t)c->edx << 32) | c->eax);");
                    line("c->eax = (uint32_t)(int32_t)(x / d); c->edx = (uint32_t)(int32_t)(x % d);");
                } else {
                    line("const uint64_t x = ((uint64_t)c->edx << 32) | c->eax;");
                    line("c->eax = (uint32_t)(x / d); c->edx = (uint32_t)(x % d);");
                }
            } else if (b == 16) {
                line("const {} x = ({})(((uint32_t)(uint16_t)c->edx << 16) | (uint16_t)c->eax);", s ? "int32_t" : "uint32_t",
                     s ? "int32_t" : "uint32_t");
                line("SET_LO16(c->eax, x / d); SET_LO16(c->edx, x % d);");
            } else {
                line("const {} x = ({})(uint16_t)c->eax;", s ? "int32_t" : "uint32_t", s ? "int16_t" : "uint32_t");
                line("SET_LO8(c->eax, x / d); SET_HI8(c->eax, x % d);");
            }
            return true;
        }
        case ZYDIS_MNEMONIC_CDQ:
            line("c->edx = (uint32_t)((int32_t)c->eax >> 31);");
            return true;
        case ZYDIS_MNEMONIC_CWDE:
            line("c->eax = (uint32_t)(int32_t)(int16_t)c->eax;");
            return true;
        case ZYDIS_MNEMONIC_CBW:
            line("SET_LO16(c->eax, (int16_t)(int8_t)c->eax);");
            return true;
        case ZYDIS_MNEMONIC_CWD:
            line("SET_LO16(c->edx, (uint16_t)((int16_t)c->eax >> 15));");
            return true;

        case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETNB:
        case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE:
        case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_SETNP:
        case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_SETNLE:
            raw(wr(*o0, std::format("({}) ? 1 : 0", cond(cc_of(m), li))));
            return true;
        case ZYDIS_MNEMONIC_CMOVO: case ZYDIS_MNEMONIC_CMOVNO: case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_CMOVNB:
        case ZYDIS_MNEMONIC_CMOVZ: case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_CMOVNBE:
        case ZYDIS_MNEMONIC_CMOVS: case ZYDIS_MNEMONIC_CMOVNS: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_CMOVNP:
        case ZYDIS_MNEMONIC_CMOVL: case ZYDIS_MNEMONIC_CMOVNL: case ZYDIS_MNEMONIC_CMOVLE: case ZYDIS_MNEMONIC_CMOVNLE:
            line("if ({}) {{", cond(cc_of(m), li));
            line("    {}", wr(*o0, rd(*o1)));
            line("}}");
            return true;

        case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC: {
            if (o0->type == ZYDIS_OPERAND_TYPE_MEMORY && o1->type == ZYDIS_OPERAND_TYPE_REGISTER) {
                // The bit offset is a signed value of the operand size.
                line("const int32_t off = ({}){};", sty(o1->size), rd(*o1));
                line("const uint32_t at = {} + (uint32_t)((off >> 5) * 4); const uint32_t k = (uint32_t)off & 31;", ea(*o0));
                line("const uint32_t a = rd32(at);");
            } else {
                line("const uint32_t k = (uint32_t)({}) % {};", rd(*o1), b);
                line("const uint32_t a = {};", rd(*o0));
            }
            if (need & F_CF) line("cf = (a >> k) & 1;");
            std::string r;
            if (m == ZYDIS_MNEMONIC_BTS) r = "a | (1u << k)";
            if (m == ZYDIS_MNEMONIC_BTR) r = "a & ~(1u << k)";
            if (m == ZYDIS_MNEMONIC_BTC) r = "a ^ (1u << k)";
            if (!r.empty()) {
                if (o0->type == ZYDIS_OPERAND_TYPE_MEMORY && o1->type == ZYDIS_OPERAND_TYPE_REGISTER)
                    line("wr32(at, {});", r);
                else
                    raw(wr(*o0, r));
            }
            return true;
        }
        case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR:
            line("const uint32_t s = {};", rd(*o1));
            if (need & F_ZF) line("zf = s == 0;");
            line("if (s) {{");
            line("    {}", wr(*o0, m == ZYDIS_MNEMONIC_BSF ? "__builtin_ctz(s)" : "31 - __builtin_clz(s)"));
            line("}}");
            return true;

        case ZYDIS_MNEMONIC_LAHF:
            line("SET_HI8(c->eax, (sf << 7) | (zf << 6) | (af << 4) | (pf << 2) | 2 | cf);");
            return true;
        case ZYDIS_MNEMONIC_SAHF:
            line("const uint32_t h = (c->eax >> 8) & 0xFF;");
            line("cf = h & 1; pf = (h >> 2) & 1; af = (h >> 4) & 1; zf = (h >> 6) & 1; sf = (h >> 7) & 1;");
            return true;
        case ZYDIS_MNEMONIC_CLD: line("c->df = 0;"); return true;
        case ZYDIS_MNEMONIC_STD: line("c->df = 1;"); return true;
        case ZYDIS_MNEMONIC_CLC: line("cf = 0;"); return true;
        case ZYDIS_MNEMONIC_STC: line("cf = 1;"); return true;
        case ZYDIS_MNEMONIC_CMC: line("cf = !cf;"); return true;

        case ZYDIS_MNEMONIC_CPUID: line("recomp_cpuid(c);"); return true;
        case ZYDIS_MNEMONIC_RDTSC:
            line("const uint64_t t = recomp_rdtsc(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32);");
            return true;
        case ZYDIS_MNEMONIC_XLAT:
            line("SET_LO8(c->eax, rd8(c->ebx + (uint8_t)c->eax));");
            return true;
        case ZYDIS_MNEMONIC_INT: case ZYDIS_MNEMONIC_INTO: case ZYDIS_MNEMONIC_INT1:
            line("recomp_trap(c, {}, \"int\");", hex(in.addr));
            return true;

        default: break;
    }

    // String instructions. MOVSD/CMPSD share mnemonics with SSE2.
    if (base_isa) {
        const int sz = string_size(m);
        const uint64_t attrs = in.ins.attributes;
        const bool rep = attrs & ZYDIS_ATTRIB_HAS_REP;
        const int cmp_rep = (attrs & ZYDIS_ATTRIB_HAS_REPE) ? 1 : (attrs & ZYDIS_ATTRIB_HAS_REPNE) ? 2 : 0;
        switch (m) {
            case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
                line("recomp_movs(c, {}, {});", sz, rep ? 1 : 0);
                return true;
            case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
                line("recomp_stos(c, {}, {});", sz, rep ? 1 : 0);
                return true;
            case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
                line("recomp_lods(c, {}, {});", sz, rep ? 1 : 0);
                return true;
            case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
            case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: {
                const bool cmps = m == ZYDIS_MNEMONIC_CMPSB || m == ZYDIS_MNEMONIC_CMPSW || m == ZYDIS_MNEMONIC_CMPSD;
                const std::string t = ty(sz * 8);
                line("uint32_t xa = 0, xb = 0;");
                line("if (recomp_{}(c, {}, {}, &xa, &xb)) {{", cmps ? "cmps" : "scas", sz, cmp_rep);
                line("    const {0} a = ({0})xa, b = ({0})xb, r = ({0})(a - b);", t);
                indent_ += "    ";
                flags_sub("a", "b", "r", sz * 8, need);
                indent_.resize(indent_.size() - 4);
                line("}}");
                return true;
            }
            default: break;
        }
    }
    return false;
}

}  // namespace recomp
