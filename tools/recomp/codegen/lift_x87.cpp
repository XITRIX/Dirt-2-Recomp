// x87 FPU instructions. The register stack is modelled as eight doubles plus
// TOP; MSVC runs the FPU at 53-bit precision, so double matches its results.
#include "codegen/fn_lifter.h"

namespace recomp {

namespace {

int st_index(const ZydisDecodedOperand& o) {
    if (o.type != ZYDIS_OPERAND_TYPE_REGISTER) return -1;
    const int i = o.reg.value - ZYDIS_REGISTER_ST0;
    return i >= 0 && i < 8 ? i : -1;
}

}  // namespace

bool FnLifter::lift_x87(LInsn& li) {
    const Insn& in = li.in;
    if (in.ins.meta.isa_ext != ZYDIS_ISA_EXT_X87) return false;
    const ZydisMnemonic m = in.mnem();
    const uint32_t need = li.live_out & li.writes;

    // Explicit operands: at most one memory operand and up to two stack registers.
    const ZydisDecodedOperand* mem = nullptr;
    int st[2] = {-1, -1};
    int nst = 0;
    for (int i = 0; i < in.nops(); ++i) {
        const ZydisDecodedOperand& o = in.op(i);
        if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) mem = &o;
        else if (st_index(o) >= 0 && nst < 2) st[nst++] = st_index(o);
    }
    auto load_mem = [&](bool integer) -> std::string {
        if (integer) {
            switch (mem->size) {
                case 16: return std::format("(double)(int16_t)rd16({})", ea(*mem));
                case 64: return std::format("(double)(int64_t)rd64({})", ea(*mem));
                default: return std::format("(double)(int32_t)rd32({})", ea(*mem));
            }
        }
        switch (mem->size) {
            case 32: return std::format("(double)rdf32({})", ea(*mem));
            case 80: return std::format("recomp_rdf80({})", ea(*mem));
            default: return std::format("rdf64({})", ea(*mem));
        }
    };

    // Two-operand arithmetic: ST(d) = ST(d) op src, or reversed.
    auto arith = [&](const char* op, bool reverse, bool pop, bool integer) {
        if (mem) {
            line("const double s = {};", load_mem(integer));
            line("ST(0) = {};", reverse ? std::format("s {} ST(0)", op) : std::format("ST(0) {} s", op));
        } else {
            const int d = nst >= 1 ? st[0] : 1, s = nst >= 2 ? st[1] : 0;
            line("ST({0}) = {1};", d, reverse ? std::format("ST({}) {} ST({})", s, op, d) : std::format("ST({}) {} ST({})", d, op, s));
        }
        if (pop) line("fpop(c);");
    };

    switch (m) {
        case ZYDIS_MNEMONIC_FLD:
            if (mem) line("fpush(c, {});", load_mem(false));
            else line("{{ const double v = ST({}); fpush(c, v); }}", st[0] >= 0 ? st[0] : 0);
            return true;
        case ZYDIS_MNEMONIC_FILD:
            line("fpush(c, {});", load_mem(true));
            return true;
        case ZYDIS_MNEMONIC_FLD1: line("fpush(c, 1.0);"); return true;
        case ZYDIS_MNEMONIC_FLDZ: line("fpush(c, 0.0);"); return true;
        case ZYDIS_MNEMONIC_FLDPI: line("fpush(c, 3.14159265358979323846);"); return true;
        case ZYDIS_MNEMONIC_FLDL2E: line("fpush(c, 1.44269504088896340736);"); return true;
        case ZYDIS_MNEMONIC_FLDL2T: line("fpush(c, 3.32192809488736234787);"); return true;
        case ZYDIS_MNEMONIC_FLDLG2: line("fpush(c, 0.30102999566398119521);"); return true;
        case ZYDIS_MNEMONIC_FLDLN2: line("fpush(c, 0.69314718055994530942);"); return true;

        case ZYDIS_MNEMONIC_FST: case ZYDIS_MNEMONIC_FSTP:
            if (mem) {
                switch (mem->size) {
                    case 32: line("wrf32({}, (float)ST(0));", ea(*mem)); break;
                    case 80: line("recomp_wrf80({}, ST(0));", ea(*mem)); break;
                    default: line("wrf64({}, ST(0));", ea(*mem)); break;
                }
            } else {
                line("ST({}) = ST(0);", st[0] >= 0 ? st[0] : 1);
            }
            if (m == ZYDIS_MNEMONIC_FSTP) line("fpop(c);");
            return true;
        case ZYDIS_MNEMONIC_FIST: case ZYDIS_MNEMONIC_FISTP: case ZYDIS_MNEMONIC_FISTTP: {
            const std::string v = m == ZYDIS_MNEMONIC_FISTTP ? "trunc(ST(0))" : "recomp_fround(c, ST(0))";
            switch (mem->size) {
                case 16: line("wr16({}, (uint16_t)cvt_f2i16({}));", ea(*mem), v); break;
                case 64: line("wr64({}, (uint64_t)cvt_f2i64({}));", ea(*mem), v); break;
                default: line("wr32({}, (uint32_t)cvt_f2i32({}));", ea(*mem), v); break;
            }
            if (m != ZYDIS_MNEMONIC_FIST) line("fpop(c);");
            return true;
        }

        case ZYDIS_MNEMONIC_FADD: arith("+", false, false, false); return true;
        case ZYDIS_MNEMONIC_FADDP: arith("+", false, true, false); return true;
        case ZYDIS_MNEMONIC_FIADD: arith("+", false, false, true); return true;
        case ZYDIS_MNEMONIC_FSUB: arith("-", false, false, false); return true;
        case ZYDIS_MNEMONIC_FSUBP: arith("-", false, true, false); return true;
        case ZYDIS_MNEMONIC_FISUB: arith("-", false, false, true); return true;
        case ZYDIS_MNEMONIC_FSUBR: arith("-", true, false, false); return true;
        case ZYDIS_MNEMONIC_FSUBRP: arith("-", true, true, false); return true;
        case ZYDIS_MNEMONIC_FISUBR: arith("-", true, false, true); return true;
        case ZYDIS_MNEMONIC_FMUL: arith("*", false, false, false); return true;
        case ZYDIS_MNEMONIC_FMULP: arith("*", false, true, false); return true;
        case ZYDIS_MNEMONIC_FIMUL: arith("*", false, false, true); return true;
        case ZYDIS_MNEMONIC_FDIV: arith("/", false, false, false); return true;
        case ZYDIS_MNEMONIC_FDIVP: arith("/", false, true, false); return true;
        case ZYDIS_MNEMONIC_FIDIV: arith("/", false, false, true); return true;
        case ZYDIS_MNEMONIC_FDIVR: arith("/", true, false, false); return true;
        case ZYDIS_MNEMONIC_FDIVRP: arith("/", true, true, false); return true;
        case ZYDIS_MNEMONIC_FIDIVR: arith("/", true, false, true); return true;

        case ZYDIS_MNEMONIC_FCHS: line("ST(0) = -ST(0);"); return true;
        case ZYDIS_MNEMONIC_FABS: line("ST(0) = fabs(ST(0));"); return true;
        case ZYDIS_MNEMONIC_FSQRT: line("ST(0) = sqrt(ST(0));"); return true;
        case ZYDIS_MNEMONIC_FSIN: line("ST(0) = sin(ST(0)); c->fsw &= ~0x400;"); return true;
        case ZYDIS_MNEMONIC_FCOS: line("ST(0) = cos(ST(0)); c->fsw &= ~0x400;"); return true;
        case ZYDIS_MNEMONIC_FSINCOS:
            line("{{ const double x = ST(0); ST(0) = sin(x); fpush(c, cos(x)); c->fsw &= ~0x400; }}");
            return true;
        case ZYDIS_MNEMONIC_FPTAN: line("ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~0x400;"); return true;
        case ZYDIS_MNEMONIC_FPATAN: line("ST(1) = atan2(ST(1), ST(0)); fpop(c);"); return true;
        case ZYDIS_MNEMONIC_FYL2X: line("ST(1) = ST(1) * log2(ST(0)); fpop(c);"); return true;
        case ZYDIS_MNEMONIC_FYL2XP1: line("ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c);"); return true;
        case ZYDIS_MNEMONIC_F2XM1: line("ST(0) = exp2(ST(0)) - 1.0;"); return true;
        case ZYDIS_MNEMONIC_FSCALE: line("ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"); return true;
        case ZYDIS_MNEMONIC_FRNDINT: line("ST(0) = recomp_fround(c, ST(0));"); return true;
        case ZYDIS_MNEMONIC_FPREM: line("recomp_fprem(c, 0);"); return true;
        case ZYDIS_MNEMONIC_FPREM1: line("recomp_fprem(c, 1);"); return true;
        case ZYDIS_MNEMONIC_FXCH: {
            const int i = nst >= 2 ? (st[0] == 0 ? st[1] : st[0]) : (nst == 1 ? st[0] : 1);
            line("{{ const double t = ST(0); ST(0) = ST({0}); ST({0}) = t; }}", i);
            return true;
        }
        case ZYDIS_MNEMONIC_FINCSTP: line("c->ftop = (c->ftop + 1) & 7;"); return true;
        case ZYDIS_MNEMONIC_FDECSTP: line("c->ftop = (c->ftop - 1) & 7;"); return true;
        case ZYDIS_MNEMONIC_FFREE: case ZYDIS_MNEMONIC_FNOP: case ZYDIS_MNEMONIC_FWAIT:
            return true;

        case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP:
        case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP: {
            const bool integer = m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
            std::string src;
            if (mem) src = load_mem(integer);
            else src = std::format("ST({})", nst >= 2 ? st[1] : (nst == 1 && st[0] != 0 ? st[0] : 1));
            line("fcom_sw(c, ST(0), {});", src);
            if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP || m == ZYDIS_MNEMONIC_FICOMP) line("fpop(c);");
            return true;
        }
        case ZYDIS_MNEMONIC_FCOMPP: case ZYDIS_MNEMONIC_FUCOMPP:
            line("fcom_sw(c, ST(0), ST(1)); fpop(c); fpop(c);");
            return true;
        case ZYDIS_MNEMONIC_FTST:
            line("fcom_sw(c, ST(0), 0.0);");
            return true;
        case ZYDIS_MNEMONIC_FXAM:
            line("{{ const double x = ST(0); uint16_t sw = c->fsw & ~0x4700;");
            line("  if (signbit(x)) sw |= 0x200;");
            line("  if (x != x) sw |= 0x100; else if (isinf(x)) sw |= 0x500; else if (x == 0) sw |= 0x4000;");
            line("  else if (fpclassify(x) == FP_SUBNORMAL) sw |= 0x4400; else sw |= 0x400;");
            line("  c->fsw = sw; }}");
            return true;
        case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
            const int s = nst >= 2 ? st[1] : 1;
            line("const double a = ST(0), b = ST({});", s);
            line("const uint32_t un = (a != a) || (b != b);");
            if (need & F_ZF) line("zf = un || a == b;");
            if (need & F_PF) line("pf = un;");
            if (need & F_CF) line("cf = un || a < b;");
            if (need & F_OF) line("of = 0;");
            if (need & F_SF) line("sf = 0;");
            if (need & F_AF) line("af = 0;");
            if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) line("fpop(c);");
            return true;
        }

        case ZYDIS_MNEMONIC_FNSTSW:
            if (mem) line("wr16({}, fstsw(c));", ea(*mem));
            else line("SET_LO16(c->eax, fstsw(c));");
            return true;
        case ZYDIS_MNEMONIC_FNSTCW:
            line("wr16({}, c->fcw);", ea(*mem));
            return true;
        case ZYDIS_MNEMONIC_FLDCW:
            line("c->fcw = rd16({});", ea(*mem));
            return true;
        case ZYDIS_MNEMONIC_FNCLEX:
            line("c->fsw &= 0x7F00;");
            return true;
        case ZYDIS_MNEMONIC_FNINIT:
            line("c->fcw = 0x37F; c->fsw = 0; c->ftop = 0; c->ftag = 0xFFFF;");
            return true;
        default:
            return false;
    }
}

}  // namespace recomp
