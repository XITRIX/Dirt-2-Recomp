#include "codegen/fn_lifter.h"

#include <algorithm>
#include <cctype>

namespace recomp {

int cc_of(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO: return CC_O;
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_CMOVNO: return CC_NO;
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: return CC_B;
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: return CC_NB;
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return CC_Z;
        case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return CC_NZ;
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return CC_BE;
        case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return CC_NBE;
        case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return CC_S;
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return CC_NS;
        case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP: return CC_P;
        case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_CMOVNP: return CC_NP;
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return CC_L;
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return CC_NL;
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return CC_LE;
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return CC_NLE;
        default: return -1;
    }
}

namespace {

FuseKind writer_kind(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_SUB: return FuseKind::Cmp;
        case ZYDIS_MNEMONIC_TEST: case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
            return FuseKind::Logic;
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG:
            return FuseKind::Result;
        default: return FuseKind::None;
    }
}

bool fusible(FuseKind k, int cc) {
    switch (k) {
        case FuseKind::Cmp: return cc != CC_O && cc != CC_NO && cc != CC_P && cc != CC_NP;
        case FuseKind::Logic: return cc != CC_P && cc != CC_NP;
        case FuseKind::Result: return cc == CC_Z || cc == CC_NZ || cc == CC_S || cc == CC_NS;
        default: return false;
    }
}

const char* kGpr32[] = {"c->eax", "c->ecx", "c->edx", "c->ebx", "c->esp", "c->ebp", "c->esi", "c->edi"};

int gpr_index(ZydisRegister r32) {
    switch (r32) {
        case ZYDIS_REGISTER_EAX: return 0;
        case ZYDIS_REGISTER_ECX: return 1;
        case ZYDIS_REGISTER_EDX: return 2;
        case ZYDIS_REGISTER_EBX: return 3;
        case ZYDIS_REGISTER_ESP: return 4;
        case ZYDIS_REGISTER_EBP: return 5;
        case ZYDIS_REGISTER_ESI: return 6;
        case ZYDIS_REGISTER_EDI: return 7;
        default: return -1;
    }
}

bool is_shift(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL:
        case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR: case ZYDIS_MNEMONIC_SHLD:
        case ZYDIS_MNEMONIC_SHRD:
            return true;
        default: return false;
    }
}

bool is_high8(ZydisRegister r) {
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
}

}  // namespace

FnLifter::FnLifter(const PeImage& img, const Disasm& dis, const Discovery& disc, const Function& f,
                   const std::set<uint32_t>& lifted, LiftStats& stats, bool resumable)
    : img_(img), dis_(dis), disc_(disc), f_(f), lifted_(lifted), stats_(stats), resumable_(resumable) {}

std::string FnLifter::ty(int b) {
    switch (b) {
        case 8: return "uint8_t";
        case 16: return "uint16_t";
        case 64: return "uint64_t";
        default: return "uint32_t";
    }
}

std::string FnLifter::sty(int b) {
    switch (b) {
        case 8: return "int8_t";
        case 16: return "int16_t";
        case 64: return "int64_t";
        default: return "int32_t";
    }
}

std::string FnLifter::import_fn(uint32_t iat) const {
    const Import* imp = img_.import_at(iat);
    if (!imp) return {};
    std::string dll = imp->dll;
    if (dll.size() > 4 && dll.ends_with(".dll")) dll.resize(dll.size() - 4);
    std::string s = "imp_" + dll + "_" + (imp->name.empty() ? "ord" + std::to_string(imp->ordinal) : imp->name);
    for (char& ch : s)
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    return s;
}

std::string FnLifter::gpr(ZydisRegister r) const {
    const ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    const int idx = gpr_index(big);
    if (idx < 0) return "0 /*bad reg*/";
    const char* n = kGpr32[idx];
    switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r)) {
        case 32: return n;
        case 16: return std::format("((uint16_t){})", n);
        default: return is_high8(r) ? std::format("((uint8_t)({} >> 8))", n) : std::format("((uint8_t){})", n);
    }
}

std::string FnLifter::set_gpr(ZydisRegister r, const std::string& v) const {
    const ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    const int idx = gpr_index(big);
    if (idx < 0) return "/*bad reg*/;";
    const char* n = kGpr32[idx];
    switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r)) {
        case 32: return std::format("{} = (uint32_t)({});", n, v);
        case 16: return std::format("SET_LO16({}, {});", n, v);
        default: return is_high8(r) ? std::format("SET_HI8({}, {});", n, v) : std::format("SET_LO8({}, {});", n, v);
    }
}

std::string FnLifter::ea(const ZydisDecodedOperand& op) const {
    if (op.id < 3 && mem_cached_[op.id]) return std::format("ea{}", op.id);
    std::vector<std::string> parts;
    if (op.mem.base != ZYDIS_REGISTER_NONE) parts.push_back(gpr(op.mem.base));
    if (op.mem.index != ZYDIS_REGISTER_NONE)
        parts.push_back(op.mem.scale > 1 ? std::format("{} * {}", gpr(op.mem.index), int(op.mem.scale))
                                         : gpr(op.mem.index));
    const uint32_t disp = uint32_t(op.mem.disp.value);
    if (disp || parts.empty()) parts.push_back(hex(disp));
    std::string e;
    for (size_t i = 0; i < parts.size(); ++i) e += (i ? " + " : "") + parts[i];
    if (op.mem.segment == ZYDIS_REGISTER_FS) e = "c->fs_base + " + e;
    return parts.size() == 1 && op.mem.segment != ZYDIS_REGISTER_FS ? e : "(uint32_t)(" + e + ")";
}

void FnLifter::begin_mem(const Insn& in) {
    cur_width_ = in.ins.operand_width ? in.ins.operand_width : 32;
    for (int i = 0; i < 3; ++i) mem_cached_[i] = false;
    for (int i = 0; i < in.nops() && i < 3; ++i) {
        const ZydisDecodedOperand& o = in.op(i);
        if (o.type != ZYDIS_OPERAND_TYPE_MEMORY || o.mem.type == ZYDIS_MEMOP_TYPE_AGEN) continue;
        line("const uint32_t ea{} = {};", i, ea(o));
        mem_cached_[i] = true;
    }
}

std::string FnLifter::rd(const ZydisDecodedOperand& op) {
    switch (op.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: {
            const ZydisRegisterClass cls = ZydisRegisterGetClass(op.reg.value);
            if (cls == ZYDIS_REGCLASS_XMM) return std::format("c->xmm[{}]", op.reg.value - ZYDIS_REGISTER_XMM0);
            return gpr(op.reg.value);
        }
        case ZYDIS_OPERAND_TYPE_MEMORY:
            switch (op.size) {
                case 8: return std::format("rd8({})", ea(op));
                case 16: return std::format("rd16({})", ea(op));
                case 64: return std::format("rd64({})", ea(op));
                case 128: return std::format("rd128({})", ea(op));
                default: return std::format("rd32({})", ea(op));
            }
        case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
            // Immediates are sign-extended to the operation width (push imm8, add r32, imm8).
            const int w = std::max<int>(op.size, cur_width_);
            const uint64_t mask = w >= 64 ? ~0ull : ((1ull << w) - 1);
            return hex(op.imm.value.u & mask);
        }
        default: return "0 /*operand*/";
    }
}

std::string FnLifter::wr(const ZydisDecodedOperand& op, const std::string& v) {
    switch (op.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: {
            const ZydisRegisterClass cls = ZydisRegisterGetClass(op.reg.value);
            if (cls == ZYDIS_REGCLASS_XMM)
                return std::format("c->xmm[{}] = {};", op.reg.value - ZYDIS_REGISTER_XMM0, v);
            return set_gpr(op.reg.value, v);
        }
        case ZYDIS_OPERAND_TYPE_MEMORY:
            switch (op.size) {
                case 8: return std::format("wr8({}, {});", ea(op), v);
                case 16: return std::format("wr16({}, {});", ea(op), v);
                case 64: return std::format("wr64({}, {});", ea(op), v);
                case 128: return std::format("wr128({}, {});", ea(op), v);
                default: return std::format("wr32({}, {});", ea(op), v);
            }
        default: return "/*bad write*/;";
    }
}

// ---- flags ----

void FnLifter::flags_szp(const std::string& r, int b, uint32_t need) {
    if (need & F_ZF) line("zf = ({})({}) == 0;", ty(b), r);
    if (need & F_SF) line("sf = (({})({}) >> {}) & 1;", ty(b), r, b - 1);
    if (need & F_PF) line("pf = recomp_parity({});", r);
}

void FnLifter::flags_logic(const std::string& r, int b, uint32_t need) {
    if (need & F_CF) line("cf = 0;");
    if (need & F_OF) line("of = 0;");
    if (need & F_AF) line("af = 0;");
    flags_szp(r, b, need);
}

void FnLifter::flags_add(const std::string& a, const std::string& bb, const std::string& r, int b, uint32_t need,
                         const std::string& carry) {
    if (need & F_CF) {
        if (carry.empty()) line("cf = ({})({}) < ({})({});", ty(b), r, ty(b), a);
        else line("cf = (uint32_t)(((uint64_t)({}) + ({}) + ({})) >> {}) & 1;", a, bb, carry, b);
    }
    if (need & F_OF) line("of = ((({0} ^ {2}) & ({1} ^ {2})) >> {3}) & 1;", a, bb, r, b - 1);
    if (need & F_AF) line("af = (({} ^ {} ^ {}) >> 4) & 1;", a, bb, r);
    flags_szp(r, b, need);
}

void FnLifter::flags_sub(const std::string& a, const std::string& bb, const std::string& r, int b, uint32_t need,
                         const std::string& borrow) {
    if (need & F_CF) {
        if (borrow.empty()) line("cf = ({})({}) < ({})({});", ty(b), a, ty(b), bb);
        else line("cf = (uint64_t)({}) < (uint64_t)({}) + ({});", a, bb, borrow);
    }
    if (need & F_OF) line("of = ((({0} ^ {1}) & ({0} ^ {2})) >> {3}) & 1;", a, bb, r, b - 1);
    if (need & F_AF) line("af = (({} ^ {} ^ {}) >> 4) & 1;", a, bb, r);
    flags_szp(r, b, need);
}

std::string FnLifter::cond(int cc, const LInsn& li) const {
    if (li.fused_from >= 0) {
        const LInsn& w = cur_block_->insns[size_t(li.fused_from)];
        const int b = w.in.op(0).size;
        const std::string s = sty(b), u = ty(b);
        switch (w.fuse_src) {
            case FuseKind::Cmp: {
                const std::string a = std::format("({})fa", u), bb = std::format("({})fb", u);
                const std::string sa = std::format("({})fa", s), sb = std::format("({})fb", s);
                switch (cc) {
                    case CC_Z: return a + " == " + bb;
                    case CC_NZ: return a + " != " + bb;
                    case CC_B: return a + " < " + bb;
                    case CC_NB: return a + " >= " + bb;
                    case CC_BE: return a + " <= " + bb;
                    case CC_NBE: return a + " > " + bb;
                    case CC_L: return sa + " < " + sb;
                    case CC_NL: return sa + " >= " + sb;
                    case CC_LE: return sa + " <= " + sb;
                    case CC_NLE: return sa + " > " + sb;
                    case CC_S: return std::format("({})(fa - fb) < 0", s);
                    case CC_NS: return std::format("({})(fa - fb) >= 0", s);
                }
                break;
            }
            case FuseKind::Logic: {
                const std::string r = std::format("({})fr", s);
                switch (cc) {
                    case CC_Z: case CC_BE: return r + " == 0";
                    case CC_NZ: case CC_NBE: return r + " != 0";
                    case CC_S: case CC_L: return r + " < 0";
                    case CC_NS: case CC_NL: return r + " >= 0";
                    case CC_LE: return r + " <= 0";
                    case CC_NLE: return r + " > 0";
                    case CC_B: case CC_O: return "0";
                    case CC_NB: case CC_NO: return "1";
                }
                break;
            }
            case FuseKind::Result: {
                const std::string r = std::format("({})fr", s);
                switch (cc) {
                    case CC_Z: return r + " == 0";
                    case CC_NZ: return r + " != 0";
                    case CC_S: return r + " < 0";
                    case CC_NS: return r + " >= 0";
                }
                break;
            }
            default: break;
        }
    }
    switch (cc) {
        case CC_O: return "of";
        case CC_NO: return "!of";
        case CC_B: return "cf";
        case CC_NB: return "!cf";
        case CC_Z: return "zf";
        case CC_NZ: return "!zf";
        case CC_BE: return "(cf | zf)";
        case CC_NBE: return "!(cf | zf)";
        case CC_S: return "sf";
        case CC_NS: return "!sf";
        case CC_P: return "pf";
        case CC_NP: return "!pf";
        case CC_L: return "(sf != of)";
        case CC_NL: return "(sf == of)";
        case CC_LE: return "(zf | (sf != of))";
        default: return "(!zf & (sf == of))";
    }
}

void FnLifter::unimpl(const Insn& in) {
    const char* m = ZydisMnemonicGetString(in.mnem());
    line("recomp_unimpl(c, {}, \"{}\");", hex(in.addr), m);
    ++stats_.unimplemented;
    ++stats_.unimpl_by_mnemonic[m];
}

// ---- analysis passes ----

void FnLifter::decode_blocks() {
    for (auto& [s, b] : f_.blocks) {
        LBlock lb;
        lb.b = &b;
        for (uint32_t a = b.start; a < b.end;) {
            LInsn li;
            if (!dis_.decode(a, li.in)) break;
            if (li.in.ins.cpu_flags) {
                const ZydisAccessedFlags* fl = li.in.ins.cpu_flags;
                li.reads = fl->tested & F_ARITH;
                li.writes = (fl->modified | fl->set_0 | fl->set_1 | fl->undefined) & F_ARITH;
            }
            if (is_shift(li.in.mnem()) && li.in.nops() >= 2) {
                // A masked count of 0 leaves every flag untouched: an immediate 0
                // defines nothing, and a CL count may pass the old flags through.
                const ZydisDecodedOperand& cnt = li.in.op(li.in.nops() - 1);
                if (cnt.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (cnt.imm.value.u & 31) == 0) li.reads = li.writes = 0;
                else if (cnt.type == ZYDIS_OPERAND_TYPE_REGISTER) li.reads |= li.writes;
            }
            const Flow fw = classify(li.in);
            if (fw == Flow::Call) {
                li.reads = 0;
                li.writes = F_ARITH;
            }
            a = li.in.next();
            lb.insns.push_back(li);
        }
        block_index_[s] = blocks_.size();
        blocks_.push_back(std::move(lb));
    }
}

void FnLifter::fuse() {
    for (LBlock& lb : blocks_) {
        int last = -1;
        for (size_t i = 0; i < lb.insns.size(); ++i) {
            LInsn& li = lb.insns[i];
            const int cc = cc_of(li.in.mnem());
            if (cc >= 0 && li.reads && last >= 0) {
                LInsn& w = lb.insns[size_t(last)];
                const FuseKind k = writer_kind(w.in.mnem());
                if ((li.reads & ~w.writes) == 0 && fusible(k, cc) && w.in.nops() >= 1 &&
                    w.in.op(0).size >= 8 && w.in.op(0).size <= 32) {
                    w.fuse_src = k;
                    li.fused_from = last;
                    li.reads = 0;
                }
            }
            if (li.writes) last = writer_kind(li.in.mnem()) != FuseKind::None ? int(i) : -1;
        }
    }
}

void FnLifter::liveness() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t bi = blocks_.size(); bi-- > 0;) {
            LBlock& lb = blocks_[bi];
            uint32_t out = flags_live_out && lb.b->kind == BlockEnd::Ret ? F_ARITH : 0;
            for (uint32_t s : lb.b->succ)
                if (auto it = block_index_.find(s); it != block_index_.end()) out |= blocks_[it->second].live_in;
            uint32_t live = out;
            for (size_t i = lb.insns.size(); i-- > 0;) {
                lb.insns[i].live_out = live;
                live = (live & ~lb.insns[i].writes) | lb.insns[i].reads;
            }
            if (out != lb.live_out || live != lb.live_in) {
                lb.live_out = out;
                lb.live_in = live;
                changed = true;
            }
        }
    }
}

// ---- emission ----

void FnLifter::emit_exit_to(uint32_t t) {
    if (block_index_.count(t)) line("goto L_{:08X};", t);
    else if (lifted_.count(t)) line("{}(c); return;", fn_name(t));
    else line("recomp_call(c, {}); return;", hex(t));
}

void FnLifter::emit_call(const Insn& in) {
    const std::string next = hex(in.next());
    if (auto t = in.branch_target()) {
        if (lifted_.count(*t)) line("push32(c, {}); {}(c);", next, fn_name(*t));
        else line("push32(c, {}); recomp_call(c, {});", next, hex(*t));
        return;
    }
    if (auto m = in.abs_mem(0); m && img_.import_at(*m)) {
        line("push32(c, {}); {}(c);", next, import_fn(*m));
        return;
    }
    line("{{");
    indent_ += "    ";
    begin_mem(in);
    line("const uint32_t t = {};", rd(in.op(0)));
    line("push32(c, {}); recomp_call(c, t);", next);
    indent_.resize(indent_.size() - 4);
    line("}}");
}

void FnLifter::emit_terminator(LBlock& lb, size_t bi) {
    const Block& b = *lb.b;
    const uint32_t next_block = bi + 1 < blocks_.size() ? blocks_[bi + 1].b->start : 0;
    auto fall = [&](uint32_t t) {
        if (t != next_block || !block_index_.count(t)) emit_exit_to(t);
    };
    const LInsn* last = lb.insns.empty() ? nullptr : &lb.insns.back();
    switch (b.kind) {
        case BlockEnd::Fallthrough:
            fall(b.end);
            break;
        case BlockEnd::TailCall:
            if (b.succ.empty()) line("recomp_trap(c, {}, \"tailcall without target\"); return;", hex(b.end));
            else fall(b.succ[0]);
            break;
        case BlockEnd::Jump:
            fall(b.succ[0]);
            break;
        case BlockEnd::CondJump: {
            const int cc = cc_of(last->in.mnem());
            const uint32_t taken = b.succ[0];
            std::string c;
            if (cc >= 0) {
                c = cond(cc, *last);
            } else if (last->in.mnem() == ZYDIS_MNEMONIC_JECXZ) {
                c = "c->ecx == 0";
            } else if (last->in.mnem() == ZYDIS_MNEMONIC_JCXZ) {
                c = "(uint16_t)c->ecx == 0";
            } else if (last->in.mnem() == ZYDIS_MNEMONIC_LOOP) {
                line("c->ecx -= 1;");
                c = "c->ecx != 0";
            } else if (last->in.mnem() == ZYDIS_MNEMONIC_LOOPE) {
                line("c->ecx -= 1;");
                c = "c->ecx != 0 && zf";
            } else if (last->in.mnem() == ZYDIS_MNEMONIC_LOOPNE) {
                line("c->ecx -= 1;");
                c = "c->ecx != 0 && !zf";
            } else {
                unimpl(last->in);
                c = "0";
            }
            if (block_index_.count(taken)) {
                line("if ({}) goto L_{:08X};", c, taken);
            } else {
                line("if ({}) {{", c);
                indent_ += "    ";
                emit_exit_to(taken);
                indent_.resize(indent_.size() - 4);
                line("}}");
            }
            fall(b.succ.size() > 1 ? b.succ[1] : last->in.next());
            break;
        }
        case BlockEnd::JumpTable: {
            line("{{");
            begin_mem(last->in);
            line("const uint32_t t = {};", rd(last->in.op(0)));
            line("switch (t) {{");
            std::set<uint32_t> seen;
            for (uint32_t t : b.succ)
                if (seen.insert(t).second && block_index_.count(t)) line("case {}: goto L_{:08X};", hex(t), t);
            line("default: recomp_call(c, t); return;");
            line("}}");
            line("}}");
            break;
        }
        case BlockEnd::IndirectJump:
            line("{{");
            begin_mem(last->in);
            line("recomp_call(c, {}); return;", rd(last->in.op(0)));
            line("}}");
            break;
        case BlockEnd::ImportJump:
            line("{}(c); return;", import_fn(*last->in.abs_mem(0)));
            break;
        case BlockEnd::Ret: {
            const uint32_t pop = last->in.nops() ? uint32_t(last->in.op(0).imm.value.u) : 0;
            if (flags_live_out) line("c->eflags = recomp_pack_flags(cf, pf, af, zf, sf, of, c->df);");
            line("c->esp += {}; return;", 4 + pop);
            break;
        }
        case BlockEnd::Stop:
            line("recomp_trap(c, {}, \"{}\"); return;", hex(last->in.addr), ZydisMnemonicGetString(last->in.mnem()));
            break;
        case BlockEnd::NoReturnCall:
            emit_call(last->in);
            line("recomp_trap(c, {}, \"noreturn call returned\"); return;", hex(last->in.addr));
            break;
        case BlockEnd::Invalid:
            line("recomp_trap(c, {}, \"undecodable\"); return;", hex(b.end - 1));
            break;
    }
}

void FnLifter::emit_block(size_t bi) {
    LBlock& lb = blocks_[bi];
    cur_block_ = &lb;
    out_ += std::format("L_{:08X}:;\n", lb.b->start);
    // Terminator instructions are emitted by emit_terminator; plain fallthrough
    // and fall-into-function tail calls end on an ordinary instruction.
    size_t body = lb.insns.size();
    if (!lb.insns.empty()) {
        const Flow fw = classify(lb.insns.back().in);
        const BlockEnd k = lb.b->kind;
        if (k != BlockEnd::Fallthrough && !(k == BlockEnd::TailCall && fw != Flow::Jump) && k != BlockEnd::Invalid)
            --body;
    }
    for (size_t i = 0; i < body; ++i) {
        LInsn& li = lb.insns[i];
        cur_index_ = i;
        char text[128];
        dis_.format(li.in, text, sizeof text);
        std::string comment = text;
        for (char& ch : comment)
            if (ch == '*' || ch == '\\') ch = ' ';
        line("/* {:08X}: {} */", li.in.addr, comment);
        ++stats_.instructions;
        if (classify(li.in) == Flow::Call) {
            emit_call(li.in);
            continue;
        }
        line("{{");
        indent_ += "    ";
        begin_mem(li.in);
        if (!lift_base(li) && !lift_sse(li) && !lift_x87(li)) unimpl(li.in);
        if (li.fuse_src != FuseKind::None && li.in.nops() >= 2) {
            // Capture operands for fused conditional readers.
            if (li.fuse_src == FuseKind::Cmp) line("fa = a; fb = b;");
            else line("fr = r;");
        } else if (li.fuse_src != FuseKind::None) {
            line("fr = r;");
        }
        indent_.resize(indent_.size() - 4);
        line("}}");
    }
    if (!lb.insns.empty()) {
        char text[128];
        dis_.format(lb.insns.back().in, text, sizeof text);
        if (body < lb.insns.size()) {
            std::string comment = text;
            for (char& ch : comment)
                if (ch == '*' || ch == '\\') ch = ' ';
            line("/* {:08X}: {} */", lb.insns.back().in.addr, comment);
            ++stats_.instructions;
            cur_index_ = lb.insns.size() - 1;
        }
    }
    emit_terminator(lb, bi);
}

std::string FnLifter::emit() {
    decode_blocks();
    fuse();
    liveness();
    out_ += std::format("void {}(X86* c) {{\n", fn_name(f_.start));
    line("uint32_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0, fa = 0, fb = 0, fr = 0;");
    const size_t entry = block_index_.at(f_.start);
    if (const uint32_t in = blocks_[entry].live_in) {
        ++stats_.live_in_flags;
        line("/* flags live on entry: {:#x} */", in);
        line("cf = c->eflags & 1; pf = (c->eflags >> 2) & 1; af = (c->eflags >> 4) & 1;");
        line("zf = (c->eflags >> 6) & 1; sf = (c->eflags >> 7) & 1; of = (c->eflags >> 11) & 1;");
    }
    if (resumable_) {
        // Landing pad for exception continuations into this function's labels.
        std::string tab;
        for (size_t i = 0; i < blocks_.size(); ++i) tab += std::format("{}0x{:08X}u", i ? ", " : "", blocks_[i].b->start);
        line("static const uint32_t labels_[] = {{ {} }};", tab);
        line("RecompFrame fr_ __attribute__((cleanup(recomp_frame_leave)));");
        line("recomp_frame_enter(&fr_, labels_, {});", blocks_.size());
        line("if (RECOMP_SETJMP(fr_.jb)) {{");
        line("    switch (fr_.target) {{");
        for (const LBlock& lb : blocks_) line("    case 0x{0:08X}u: goto L_{0:08X};", lb.b->start);
        line("    }}");
        line("    recomp_trap(c, fr_.target, \"bad resume target\"); return;");
        line("}}");
        ++stats_.resumable;
    }
    if (entry != 0) line("goto L_{:08X};", f_.start);
    for (size_t bi = 0; bi < blocks_.size(); ++bi) emit_block(bi);
    out_ += "}\n\n";
    return std::move(out_);
}

}  // namespace recomp
