// SSE / SSE2 instructions, emitted as portable lane-wise C on the XMM union.
// Clang vectorises most of these back to NEON on ARM64.
#include "codegen/fn_lifter.h"

namespace recomp {

namespace {

bool is_xmm(const ZydisDecodedOperand& o) {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o.reg.value) == ZYDIS_REGCLASS_XMM;
}
bool is_mmx(const ZydisDecodedOperand& o) {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o.reg.value) == ZYDIS_REGCLASS_MMX;
}
int xr(const ZydisDecodedOperand& o) { return o.reg.value - ZYDIS_REGISTER_XMM0; }

}  // namespace

bool FnLifter::lift_sse(LInsn& li) {
    const Insn& in = li.in;
    const ZydisMnemonic m = in.mnem();
    const int n = in.nops();
    if (n < 1) {
        if (m == ZYDIS_MNEMONIC_EMMS) return true;
        return false;
    }
    for (int i = 0; i < n; ++i)
        if (is_mmx(in.op(i))) return false;  // MMX is not used by the game; treat as unimplemented
    const uint32_t need = li.live_out & li.writes;
    const ZydisDecodedOperand& o0 = in.op(0);
    const ZydisDecodedOperand* o1 = n > 1 ? &in.op(1) : nullptr;
    const uint32_t imm = n > 2 && in.op(2).type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? uint32_t(in.op(2).imm.value.u)
                         : (o1 && o1->type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? uint32_t(o1->imm.value.u) : 0);

    auto D = [&]() { return std::format("c->xmm[{}]", xr(o0)); };
    // Full 128-bit source (register or m128).
    auto src128 = [&](const ZydisDecodedOperand& o) {
        return is_xmm(o) ? std::format("c->xmm[{}]", xr(o)) : std::format("rd128({})", ea(o));
    };
    // Source whose memory form is narrower than 128 bits (low lanes only).
    auto src_lo = [&](const ZydisDecodedOperand& o) {
        if (is_xmm(o)) return std::format("c->xmm[{}]", xr(o));
        if (o.size == 64) return std::format("(XMM){{ .q = {{ rd64({}), 0 }} }}", ea(o));
        if (o.size == 32) return std::format("(XMM){{ .u = {{ rd32({}), 0, 0, 0 }} }}", ea(o));
        return std::format("rd128({})", ea(o));
    };
    auto packed = [&](const char* lane, int lanes, const std::string& expr_tmpl) {
        // expr_tmpl uses {0} for d lane, {1} for s lane.
        line("const XMM s = {}; XMM d = {};", src128(*o1), D());
        for (int k = 0; k < lanes; ++k) {
            const std::string dl = std::format("d.{}[{}]", lane, k), sl = std::format("s.{}[{}]", lane, k);
            line("{} = {};", dl, std::vformat(expr_tmpl, std::make_format_args(dl, sl)));
        }
        line("{} = d;", D());
    };
    auto scalar_ss = [&](const std::string& expr_tmpl) {
        const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].f[0]", xr(*o1)) : std::format("rdf32({})", ea(*o1));
        const std::string d = std::format("c->xmm[{}].f[0]", xr(o0));
        line("{} = {};", d, std::vformat(expr_tmpl, std::make_format_args(d, s)));
    };
    auto scalar_sd = [&](const std::string& expr_tmpl) {
        const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].d[0]", xr(*o1)) : std::format("rdf64({})", ea(*o1));
        const std::string d = std::format("c->xmm[{}].d[0]", xr(o0));
        line("{} = {};", d, std::vformat(expr_tmpl, std::make_format_args(d, s)));
    };
    auto comis = [&](bool dbl) {
        const char* l = dbl ? "d" : "f";
        const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].{}[0]", xr(*o1), l)
                                          : std::format("{}({})", dbl ? "rdf64" : "rdf32", ea(*o1));
        line("const double a = c->xmm[{}].{}[0], b = {};", xr(o0), l, s);
        line("const uint32_t un = (a != a) || (b != b);");
        if (need & F_ZF) line("zf = un || a == b;");
        if (need & F_PF) line("pf = un;");
        if (need & F_CF) line("cf = un || a < b;");
        if (need & F_OF) line("of = 0;");
        if (need & F_SF) line("sf = 0;");
        if (need & F_AF) line("af = 0;");
    };

    switch (m) {
        // ---- moves ----
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVAPD:
        case ZYDIS_MNEMONIC_MOVUPD: case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU:
        case ZYDIS_MNEMONIC_LDDQU: case ZYDIS_MNEMONIC_MOVNTPS: case ZYDIS_MNEMONIC_MOVNTDQ:
            raw(wr(o0, is_xmm(*o1) ? rd(*o1) : std::format("rd128({})", ea(*o1))));
            return true;
        case ZYDIS_MNEMONIC_MOVSS:
            if (!is_xmm(o0)) line("wrf32({}, c->xmm[{}].f[0]);", ea(o0), xr(*o1));
            else if (is_xmm(*o1)) line("c->xmm[{}].u[0] = c->xmm[{}].u[0];", xr(o0), xr(*o1));
            else line("{} = (XMM){{ .u = {{ rd32({}), 0, 0, 0 }} }};", D(), ea(*o1));
            return true;
        case ZYDIS_MNEMONIC_MOVSD:
            if (!is_xmm(o0)) line("wr64({}, c->xmm[{}].q[0]);", ea(o0), xr(*o1));
            else if (is_xmm(*o1)) line("c->xmm[{}].q[0] = c->xmm[{}].q[0];", xr(o0), xr(*o1));
            else line("{} = (XMM){{ .q = {{ rd64({}), 0 }} }};", D(), ea(*o1));
            return true;
        case ZYDIS_MNEMONIC_MOVQ:
            if (!is_xmm(o0)) line("wr64({}, c->xmm[{}].q[0]);", ea(o0), xr(*o1));
            else if (is_xmm(*o1)) line("{} = (XMM){{ .q = {{ c->xmm[{}].q[0], 0 }} }};", D(), xr(*o1));
            else line("{} = (XMM){{ .q = {{ rd64({}), 0 }} }};", D(), ea(*o1));
            return true;
        case ZYDIS_MNEMONIC_MOVD:
            if (is_xmm(o0)) line("{} = (XMM){{ .u = {{ {}, 0, 0, 0 }} }};", D(), rd(*o1));
            else raw(wr(o0, std::format("c->xmm[{}].u[0]", xr(*o1))));
            return true;
        case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVLPD:
            if (is_xmm(o0)) line("c->xmm[{}].q[0] = rd64({});", xr(o0), ea(*o1));
            else line("wr64({}, c->xmm[{}].q[0]);", ea(o0), xr(*o1));
            return true;
        case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVHPD:
            if (is_xmm(o0)) line("c->xmm[{}].q[1] = rd64({});", xr(o0), ea(*o1));
            else line("wr64({}, c->xmm[{}].q[1]);", ea(o0), xr(*o1));
            return true;
        case ZYDIS_MNEMONIC_MOVLHPS:
            line("c->xmm[{}].q[1] = c->xmm[{}].q[0];", xr(o0), xr(*o1));
            return true;
        case ZYDIS_MNEMONIC_MOVHLPS:
            line("c->xmm[{}].q[0] = c->xmm[{}].q[1];", xr(o0), xr(*o1));
            return true;

        // ---- packed float arithmetic ----
        case ZYDIS_MNEMONIC_ADDPS: packed("f", 4, "{0} + {1}"); return true;
        case ZYDIS_MNEMONIC_SUBPS: packed("f", 4, "{0} - {1}"); return true;
        case ZYDIS_MNEMONIC_MULPS: packed("f", 4, "{0} * {1}"); return true;
        case ZYDIS_MNEMONIC_DIVPS: packed("f", 4, "{0} / {1}"); return true;
        case ZYDIS_MNEMONIC_MINPS: packed("f", 4, "sse_minf({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_MAXPS: packed("f", 4, "sse_maxf({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_SQRTPS: packed("f", 4, "sqrtf({1})"); return true;
        case ZYDIS_MNEMONIC_RSQRTPS: packed("f", 4, "1.0f / sqrtf({1})"); return true;
        case ZYDIS_MNEMONIC_RCPPS: packed("f", 4, "1.0f / {1}"); return true;
        case ZYDIS_MNEMONIC_ADDPD: packed("d", 2, "{0} + {1}"); return true;
        case ZYDIS_MNEMONIC_SUBPD: packed("d", 2, "{0} - {1}"); return true;
        case ZYDIS_MNEMONIC_MULPD: packed("d", 2, "{0} * {1}"); return true;
        case ZYDIS_MNEMONIC_DIVPD: packed("d", 2, "{0} / {1}"); return true;
        case ZYDIS_MNEMONIC_MINPD: packed("d", 2, "sse_mind({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_MAXPD: packed("d", 2, "sse_maxd({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_SQRTPD: packed("d", 2, "sqrt({1})"); return true;

        // ---- scalar float ----
        case ZYDIS_MNEMONIC_ADDSS: scalar_ss("{0} + {1}"); return true;
        case ZYDIS_MNEMONIC_SUBSS: scalar_ss("{0} - {1}"); return true;
        case ZYDIS_MNEMONIC_MULSS: scalar_ss("{0} * {1}"); return true;
        case ZYDIS_MNEMONIC_DIVSS: scalar_ss("{0} / {1}"); return true;
        case ZYDIS_MNEMONIC_MINSS: scalar_ss("sse_minf({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_MAXSS: scalar_ss("sse_maxf({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_SQRTSS: scalar_ss("sqrtf({1})"); return true;
        case ZYDIS_MNEMONIC_RSQRTSS: scalar_ss("1.0f / sqrtf({1})"); return true;
        case ZYDIS_MNEMONIC_RCPSS: scalar_ss("1.0f / {1}"); return true;
        case ZYDIS_MNEMONIC_ADDSD: scalar_sd("{0} + {1}"); return true;
        case ZYDIS_MNEMONIC_SUBSD: scalar_sd("{0} - {1}"); return true;
        case ZYDIS_MNEMONIC_MULSD: scalar_sd("{0} * {1}"); return true;
        case ZYDIS_MNEMONIC_DIVSD: scalar_sd("{0} / {1}"); return true;
        case ZYDIS_MNEMONIC_MINSD: scalar_sd("sse_mind({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_MAXSD: scalar_sd("sse_maxd({0}, {1})"); return true;
        case ZYDIS_MNEMONIC_SQRTSD: scalar_sd("sqrt({1})"); return true;

        // ---- bitwise ----
        case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD: case ZYDIS_MNEMONIC_PAND:
            packed("u", 4, "{0} & {1}"); return true;
        case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_ANDNPD: case ZYDIS_MNEMONIC_PANDN:
            packed("u", 4, "~{0} & {1}"); return true;
        case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD: case ZYDIS_MNEMONIC_POR:
            packed("u", 4, "{0} | {1}"); return true;
        case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD: case ZYDIS_MNEMONIC_PXOR:
            if (is_xmm(*o1) && o1->reg.value == o0.reg.value) {
                line("{} = (XMM){{ .q = {{ 0, 0 }} }};", D());
                return true;
            }
            packed("u", 4, "{0} ^ {1}"); return true;

        // ---- shuffles ----
        case ZYDIS_MNEMONIC_SHUFPS:
            line("const XMM s = {}, d = {};", src128(*o1), D());
            line("{} = (XMM){{ .u = {{ d.u[{}], d.u[{}], s.u[{}], s.u[{}] }} }};", D(), imm & 3, (imm >> 2) & 3,
                 (imm >> 4) & 3, (imm >> 6) & 3);
            return true;
        case ZYDIS_MNEMONIC_SHUFPD:
            line("const XMM s = {}, d = {};", src128(*o1), D());
            line("{} = (XMM){{ .q = {{ d.q[{}], s.q[{}] }} }};", D(), imm & 1, (imm >> 1) & 1);
            return true;
        case ZYDIS_MNEMONIC_PSHUFD:
            line("const XMM s = {};", src128(*o1));
            line("{} = (XMM){{ .u = {{ s.u[{}], s.u[{}], s.u[{}], s.u[{}] }} }};", D(), imm & 3, (imm >> 2) & 3,
                 (imm >> 4) & 3, (imm >> 6) & 3);
            return true;
        case ZYDIS_MNEMONIC_PSHUFLW:
            line("const XMM s = {}; XMM d = s;", src128(*o1));
            for (int k = 0; k < 4; ++k) line("d.w[{}] = s.w[{}];", k, (imm >> (2 * k)) & 3);
            line("{} = d;", D());
            return true;
        case ZYDIS_MNEMONIC_PSHUFHW:
            line("const XMM s = {}; XMM d = s;", src128(*o1));
            for (int k = 0; k < 4; ++k) line("d.w[{}] = s.w[{}];", 4 + k, 4 + ((imm >> (2 * k)) & 3));
            line("{} = d;", D());
            return true;
        case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_PUNPCKLDQ:
            line("const XMM s = {}, d = {};", src_lo(*o1), D());
            line("{} = (XMM){{ .u = {{ d.u[0], s.u[0], d.u[1], s.u[1] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_UNPCKHPS: case ZYDIS_MNEMONIC_PUNPCKHDQ:
            line("const XMM s = {}, d = {};", src128(*o1), D());
            line("{} = (XMM){{ .u = {{ d.u[2], s.u[2], d.u[3], s.u[3] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_UNPCKLPD: case ZYDIS_MNEMONIC_PUNPCKLQDQ:
            line("const XMM s = {}, d = {};", src_lo(*o1), D());
            line("{} = (XMM){{ .q = {{ d.q[0], s.q[0] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_UNPCKHPD: case ZYDIS_MNEMONIC_PUNPCKHQDQ:
            line("const XMM s = {}, d = {};", src128(*o1), D());
            line("{} = (XMM){{ .q = {{ d.q[1], s.q[1] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_PUNPCKLBW: case ZYDIS_MNEMONIC_PUNPCKHBW:
        case ZYDIS_MNEMONIC_PUNPCKLWD: case ZYDIS_MNEMONIC_PUNPCKHWD: {
            const bool bytes = m == ZYDIS_MNEMONIC_PUNPCKLBW || m == ZYDIS_MNEMONIC_PUNPCKHBW;
            const bool hi = m == ZYDIS_MNEMONIC_PUNPCKHBW || m == ZYDIS_MNEMONIC_PUNPCKHWD;
            const int lanes = bytes ? 16 : 8;
            const char* l = bytes ? "b" : "w";
            line("const XMM s = {}, d = {}; XMM r;", hi ? src128(*o1) : src_lo(*o1), D());
            for (int k = 0; k < lanes / 2; ++k) {
                const int from = (hi ? lanes / 2 : 0) + k;
                line("r.{0}[{1}] = d.{0}[{2}]; r.{0}[{3}] = s.{0}[{2}];", l, 2 * k, from, 2 * k + 1);
            }
            line("{} = r;", D());
            return true;
        }
        case ZYDIS_MNEMONIC_PACKUSWB:
            line("const XMM s = {}, d = {}; XMM r;", src128(*o1), D());
            line("for (int k = 0; k < 8; ++k) {{ int16_t v = d.sw[k]; r.b[k] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }}");
            line("for (int k = 0; k < 8; ++k) {{ int16_t v = s.sw[k]; r.b[8 + k] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }}");
            line("{} = r;", D());
            return true;
        case ZYDIS_MNEMONIC_PACKSSDW:
            line("const XMM s = {}, d = {}; XMM r;", src128(*o1), D());
            line("for (int k = 0; k < 4; ++k) {{ int32_t v = d.i[k]; r.sw[k] = v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }}");
            line("for (int k = 0; k < 4; ++k) {{ int32_t v = s.i[k]; r.sw[4 + k] = v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }}");
            line("{} = r;", D());
            return true;
        case ZYDIS_MNEMONIC_PACKSSWB:
            line("const XMM s = {}, d = {}; XMM r;", src128(*o1), D());
            line("for (int k = 0; k < 8; ++k) {{ int16_t v = d.sw[k]; r.sb[k] = v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }}");
            line("for (int k = 0; k < 8; ++k) {{ int16_t v = s.sw[k]; r.sb[8 + k] = v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }}");
            line("{} = r;", D());
            return true;

        // ---- compares ----
        case ZYDIS_MNEMONIC_CMPPS:
            line("const XMM s = {}; XMM d = {};", src128(*o1), D());
            for (int k = 0; k < 4; ++k) line("d.u[{0}] = sse_cmp({1}, d.f[{0}], s.f[{0}]);", k, imm & 7);
            line("{} = d;", D());
            return true;
        case ZYDIS_MNEMONIC_CMPPD:
            line("const XMM s = {}; XMM d = {};", src128(*o1), D());
            for (int k = 0; k < 2; ++k) line("d.q[{0}] = sse_cmp({1}, d.d[{0}], s.d[{0}]) ? ~0ull : 0;", k, imm & 7);
            line("{} = d;", D());
            return true;
        case ZYDIS_MNEMONIC_CMPSS: {
            // The result is an integer mask, so it is written through .u.
            const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].f[0]", xr(*o1)) : std::format("rdf32({})", ea(*o1));
            line("c->xmm[{0}].u[0] = sse_cmp({1}, c->xmm[{0}].f[0], {2});", xr(o0), imm & 7, s);
            return true;
        }
        case ZYDIS_MNEMONIC_CMPSD:
            if (in.ins.meta.isa_ext != ZYDIS_ISA_EXT_SSE2) return false;
            {
                const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].d[0]", xr(*o1)) : std::format("rdf64({})", ea(*o1));
                line("c->xmm[{0}].q[0] = sse_cmp({1}, c->xmm[{0}].d[0], {2}) ? ~0ull : 0;", xr(o0), imm & 7, s);
            }
            return true;
        case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS: comis(false); return true;
        case ZYDIS_MNEMONIC_COMISD: case ZYDIS_MNEMONIC_UCOMISD: comis(true); return true;
        case ZYDIS_MNEMONIC_PCMPEQB: packed("b", 16, "{0} == {1} ? 0xFF : 0"); return true;
        case ZYDIS_MNEMONIC_PCMPEQW: packed("w", 8, "{0} == {1} ? 0xFFFF : 0"); return true;
        case ZYDIS_MNEMONIC_PCMPEQD: packed("u", 4, "{0} == {1} ? 0xFFFFFFFFu : 0"); return true;
        case ZYDIS_MNEMONIC_PCMPGTB: packed("sb", 16, "{0} > {1} ? -1 : 0"); return true;
        case ZYDIS_MNEMONIC_PCMPGTW: packed("sw", 8, "{0} > {1} ? -1 : 0"); return true;
        case ZYDIS_MNEMONIC_PCMPGTD: packed("i", 4, "{0} > {1} ? -1 : 0"); return true;

        // ---- integer arithmetic ----
        case ZYDIS_MNEMONIC_PADDB: packed("b", 16, "(uint8_t)({0} + {1})"); return true;
        case ZYDIS_MNEMONIC_PADDW: packed("w", 8, "(uint16_t)({0} + {1})"); return true;
        case ZYDIS_MNEMONIC_PADDD: packed("u", 4, "{0} + {1}"); return true;
        case ZYDIS_MNEMONIC_PADDQ: packed("q", 2, "{0} + {1}"); return true;
        case ZYDIS_MNEMONIC_PSUBB: packed("b", 16, "(uint8_t)({0} - {1})"); return true;
        case ZYDIS_MNEMONIC_PSUBW: packed("w", 8, "(uint16_t)({0} - {1})"); return true;
        case ZYDIS_MNEMONIC_PSUBD: packed("u", 4, "{0} - {1}"); return true;
        case ZYDIS_MNEMONIC_PSUBQ: packed("q", 2, "{0} - {1}"); return true;
        case ZYDIS_MNEMONIC_PADDSW:
            packed("sw", 8, "(int16_t)((int32_t){0} + {1} > 32767 ? 32767 : (int32_t){0} + {1} < -32768 ? -32768 : {0} + {1})");
            return true;
        case ZYDIS_MNEMONIC_PMAXSW: packed("sw", 8, "{0} > {1} ? {0} : {1}"); return true;
        case ZYDIS_MNEMONIC_PMINSW: packed("sw", 8, "{0} < {1} ? {0} : {1}"); return true;
        case ZYDIS_MNEMONIC_PMULLW: packed("w", 8, "(uint16_t)({0} * {1})"); return true;

        // ---- shifts ----
        case ZYDIS_MNEMONIC_PSRLDQ: case ZYDIS_MNEMONIC_PSLLDQ: {
            const uint32_t k = imm > 16 ? 16 : imm;
            line("const XMM s = {}; XMM r = (XMM){{ .q = {{ 0, 0 }} }};", D());
            if (m == ZYDIS_MNEMONIC_PSRLDQ) line("memcpy(r.b, s.b + {0}, 16 - {0});", k);
            else line("memcpy(r.b + {0}, s.b, 16 - {0});", k);
            line("{} = r;", D());
            return true;
        }
        case ZYDIS_MNEMONIC_PSRLQ: case ZYDIS_MNEMONIC_PSLLQ: case ZYDIS_MNEMONIC_PSRLD: case ZYDIS_MNEMONIC_PSLLD:
        case ZYDIS_MNEMONIC_PSRAD: case ZYDIS_MNEMONIC_PSRLW: case ZYDIS_MNEMONIC_PSLLW: case ZYDIS_MNEMONIC_PSRAW: {
            if (o1->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) line("const uint64_t k = {}u;", imm);
            else line("const uint64_t k = {}.q[0];", src128(*o1));
            line("XMM d = {};", D());
            switch (m) {
                case ZYDIS_MNEMONIC_PSRLQ: line("for (int i = 0; i < 2; ++i) d.q[i] = k > 63 ? 0 : d.q[i] >> k;"); break;
                case ZYDIS_MNEMONIC_PSLLQ: line("for (int i = 0; i < 2; ++i) d.q[i] = k > 63 ? 0 : d.q[i] << k;"); break;
                case ZYDIS_MNEMONIC_PSRLD: line("for (int i = 0; i < 4; ++i) d.u[i] = k > 31 ? 0 : d.u[i] >> k;"); break;
                case ZYDIS_MNEMONIC_PSLLD: line("for (int i = 0; i < 4; ++i) d.u[i] = k > 31 ? 0 : d.u[i] << k;"); break;
                case ZYDIS_MNEMONIC_PSRAD: line("for (int i = 0; i < 4; ++i) d.i[i] = d.i[i] >> (k > 31 ? 31 : k);"); break;
                case ZYDIS_MNEMONIC_PSRLW: line("for (int i = 0; i < 8; ++i) d.w[i] = k > 15 ? 0 : d.w[i] >> k;"); break;
                case ZYDIS_MNEMONIC_PSLLW: line("for (int i = 0; i < 8; ++i) d.w[i] = k > 15 ? 0 : (uint16_t)(d.w[i] << k);"); break;
                default: line("for (int i = 0; i < 8; ++i) d.sw[i] = d.sw[i] >> (k > 15 ? 15 : k);"); break;
            }
            line("{} = d;", D());
            return true;
        }

        // ---- conversions ----
        case ZYDIS_MNEMONIC_CVTSI2SS:
            line("c->xmm[{}].f[0] = (float)(int32_t){};", xr(o0), rd(*o1));
            return true;
        case ZYDIS_MNEMONIC_CVTSI2SD:
            line("c->xmm[{}].d[0] = (double)(int32_t){};", xr(o0), rd(*o1));
            return true;
        case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI: {
            const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].f[0]", xr(*o1)) : std::format("rdf32({})", ea(*o1));
            raw(wr(o0, m == ZYDIS_MNEMONIC_CVTTSS2SI ? std::format("(uint32_t)cvt_f2i32({})", s)
                                                      : std::format("(uint32_t)cvt_round_i32(c, {})", s)));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_CVTSD2SI: {
            const std::string s = is_xmm(*o1) ? std::format("c->xmm[{}].d[0]", xr(*o1)) : std::format("rdf64({})", ea(*o1));
            raw(wr(o0, m == ZYDIS_MNEMONIC_CVTTSD2SI ? std::format("(uint32_t)cvt_f2i32({})", s)
                                                      : std::format("(uint32_t)cvt_round_i32(c, {})", s)));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTSS2SD:
            line("c->xmm[{}].d[0] = (double){};", xr(o0),
                 is_xmm(*o1) ? std::format("c->xmm[{}].f[0]", xr(*o1)) : std::format("rdf32({})", ea(*o1)));
            return true;
        case ZYDIS_MNEMONIC_CVTSD2SS:
            line("c->xmm[{}].f[0] = (float){};", xr(o0),
                 is_xmm(*o1) ? std::format("c->xmm[{}].d[0]", xr(*o1)) : std::format("rdf64({})", ea(*o1)));
            return true;
        case ZYDIS_MNEMONIC_CVTPS2PD:
            line("const XMM s = {};", src_lo(*o1));
            line("{} = (XMM){{ .d = {{ (double)s.f[0], (double)s.f[1] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_CVTPD2PS:
            line("const XMM s = {};", src128(*o1));
            line("{} = (XMM){{ .f = {{ (float)s.d[0], (float)s.d[1], 0.0f, 0.0f }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_CVTDQ2PS:
            line("const XMM s = {};", src128(*o1));
            line("{} = (XMM){{ .f = {{ (float)s.i[0], (float)s.i[1], (float)s.i[2], (float)s.i[3] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_CVTDQ2PD:
            line("const XMM s = {};", src_lo(*o1));
            line("{} = (XMM){{ .d = {{ (double)s.i[0], (double)s.i[1] }} }};", D());
            return true;
        case ZYDIS_MNEMONIC_CVTTPS2DQ: case ZYDIS_MNEMONIC_CVTPS2DQ: {
            const char* f = m == ZYDIS_MNEMONIC_CVTTPS2DQ ? "cvt_f2i32(" : "cvt_round_i32(c, ";
            line("const XMM s = {};", src128(*o1));
            line("{0} = (XMM){{ .i = {{ {1}s.f[0]), {1}s.f[1]), {1}s.f[2]), {1}s.f[3]) }} }};", D(), f);
            return true;
        }
        case ZYDIS_MNEMONIC_CVTTPD2DQ: case ZYDIS_MNEMONIC_CVTPD2DQ: {
            const char* f = m == ZYDIS_MNEMONIC_CVTTPD2DQ ? "cvt_f2i32(" : "cvt_round_i32(c, ";
            line("const XMM s = {};", src128(*o1));
            line("{0} = (XMM){{ .i = {{ {1}s.d[0]), {1}s.d[1]), 0, 0 }} }};", D(), f);
            return true;
        }

        // ---- masks and lane access ----
        case ZYDIS_MNEMONIC_MOVMSKPS:
            line("const XMM s = c->xmm[{}];", xr(*o1));
            raw(wr(o0, "(s.u[0] >> 31) | ((s.u[1] >> 31) << 1) | ((s.u[2] >> 31) << 2) | ((s.u[3] >> 31) << 3)"));
            return true;
        case ZYDIS_MNEMONIC_MOVMSKPD:
            line("const XMM s = c->xmm[{}];", xr(*o1));
            raw(wr(o0, "(uint32_t)((s.q[0] >> 63) | ((s.q[1] >> 63) << 1))"));
            return true;
        case ZYDIS_MNEMONIC_PMOVMSKB:
            line("const XMM s = c->xmm[{}]; uint32_t r = 0;", xr(*o1));
            line("for (int k = 0; k < 16; ++k) r |= (uint32_t)(s.b[k] >> 7) << k;");
            raw(wr(o0, "r"));
            return true;
        case ZYDIS_MNEMONIC_PEXTRW:
            raw(wr(o0, std::format("c->xmm[{}].w[{}]", xr(*o1), imm & 7)));
            return true;
        case ZYDIS_MNEMONIC_PINSRW:
            line("c->xmm[{}].w[{}] = (uint16_t){};", xr(o0), imm & 7, rd(*o1));
            return true;

        // ---- control ----
        case ZYDIS_MNEMONIC_LDMXCSR:
            line("c->mxcsr = rd32({});", ea(o0));
            return true;
        case ZYDIS_MNEMONIC_STMXCSR:
            line("wr32({}, c->mxcsr);", ea(o0));
            return true;
        default:
            return false;
    }
}

}  // namespace recomp
