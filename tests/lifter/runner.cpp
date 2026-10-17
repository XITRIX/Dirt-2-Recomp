// Differential runner: each case from lifter_cases.c runs from identical random
// states in Unicorn (UC_MODE_32) and in its two lifted C variants, and the
// resulting GPRs, flags, DF, memory, XMM registers and x87 TOP are compared.
//
// Case options (third column of cases.txt), space separated:
//   eax=V ... edi=V   V is a number, buf+N (LT_DATA_BASE + N) or LO..HI (inclusive)
//   df=0|1|r          initial direction flag (default 0)
//   skip=of,af,...    flags not compared (architecturally undefined in this form)
//   noflags           compare no arithmetic flags
//   mem=f32|f64       fill scratch memory with "nice" floats instead of bytes
//   xmm=f32|f64       fill XMM registers with nice floats; NaN lanes compare equal
//   f32=OFF,... f64=OFF,...  scratch offsets compared as floats with a tolerance (x87)
//   iters=N           iterations (default 256)
// Defaults: esp = buf+0xF000, ebp = buf+0x8000, other GPRs biased random.
//
// Usage: lifter_difftest [--iters N] [--seed S] [--shard I/N] [name-substring]
#include <unicorn/unicorn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "case_table.h"
#include "recomp/ops.h"

// ---- runtime services referenced by generated code ----

static std::string g_runtime_error;

extern "C" {
uint8_t* recomp_mem = nullptr;

void recomp_call(X86* c, uint32_t target) {
    char b[64];
    snprintf(b, sizeof b, "recomp_call(0x%08X)", target);
    g_runtime_error = b;
}
void recomp_unimpl(X86* c, uint32_t addr, const char* what) { g_runtime_error = std::string("unimplemented ") + what; }
void recomp_trap(X86* c, uint32_t addr, const char* what) { g_runtime_error = std::string("trap ") + what; }
void recomp_cpuid(X86* c) { g_runtime_error = "cpuid"; }
uint64_t recomp_rdtsc(void) { return 0; }
void recomp_div_error(X86* c, uint32_t addr) { g_runtime_error = "divide error"; }

// Same conversions as runtime/src/cpu.cpp.
double recomp_rdf80(uint32_t a) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, RECOMP_MEM(a), 8);
    memcpy(&se, RECOMP_MEM(a + 8), 2);
    const int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp(double(mant), exp - 16383 - 63);
    return sign ? -v : v;
}
void recomp_wrf80(uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (std::isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (v != 0) {
        int e;
        const double f = frexp(fabs(v), &e);
        mant = uint64_t(ldexp(f, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    memcpy(RECOMP_MEM(a), &mant, 8);
    memcpy(RECOMP_MEM(a + 8), &se, 2);
}
}

namespace {

constexpr uint32_t kFlagBits[6] = {1u << 0, 1u << 2, 1u << 4, 1u << 6, 1u << 7, 1u << 11};
constexpr const char* kFlagNames[6] = {"cf", "pf", "af", "zf", "sf", "of"};
constexpr uint32_t kArith = 0x8D5;
constexpr const char* kRegNames[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
constexpr int kUcRegs[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                            UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};
constexpr int kUcXmm[8] = {UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3,
                           UC_X86_REG_XMM4, UC_X86_REG_XMM5, UC_X86_REG_XMM6, UC_X86_REG_XMM7};

// fninit; fldcw [kResetStub + 0x100] (53-bit precision, MSVC default); stops at +8.
constexpr uint32_t kResetStub = LT_CODE_BASE - 0x1000;
const uint8_t kResetCode[] = {0xDB, 0xE3, 0xD9, 0x2D, 0x00, 0x01, 0x40, 0x00};
constexpr uint16_t kFcw = 0x027F;
constexpr size_t kMaxInsns = 100000;

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return uint32_t(next()); }
    uint32_t below(uint32_t n) { return uint32_t(next() % n); }
    // Values biased towards the edges of 8/16/32-bit arithmetic.
    uint32_t biased() {
        static const uint32_t special[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
                                           0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000001, 31, 32, 33};
        switch (below(8)) {
            case 0: return special[below(sizeof special / sizeof *special)];
            case 1: return below(64);
            case 2: return uint32_t(-int32_t(below(64)));
            case 3: {
                static const uint8_t bytes[] = {0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x0F, 0xF0};
                uint32_t v = 0;
                for (int i = 0; i < 4; ++i) v |= uint32_t(below(3) ? bytes[below(8)] : u32() & 0xFF) << (8 * i);
                return v;
            }
            default: return u32();
        }
    }
    float nice_f32() {
        switch (below(16)) {
            case 0: return 0.0f;
            case 1: return -0.0f;
            case 2: return 1.0f;
            case 3: return -1.0f;
            case 4: return 0.5f;
            case 5: return 1e30f;
            case 6: return -1e-30f;
            case 7: return INFINITY;
            case 8: return float(int32_t(below(2001)) - 1000);
            default: return (float(u32()) / 4294967296.0f - 0.5f) * float(1 << below(20));
        }
    }
    double nice_f64() {
        if (below(4) == 0) return nice_f32();
        return (double(next() >> 11) / 9007199254740992.0 - 0.5) * double(1ull << below(40));
    }
};

struct Options {
    struct RegInit { int mode = 0; uint32_t lo = 0, hi = 0; };  // 0 random, 1 fixed, 2 range
    RegInit reg[8];
    int df = 0;  // 0, 1, or -1 random
    uint32_t skip = 0;
    std::string mem, xmm;
    std::vector<uint32_t> f32, f64;
    int iters = 256;
};

uint32_t parse_value(const std::string& s) {
    if (s.rfind("buf+", 0) == 0) return LT_DATA_BASE + uint32_t(strtoul(s.c_str() + 4, nullptr, 0));
    if (s == "buf") return LT_DATA_BASE;
    return uint32_t(strtoll(s.c_str(), nullptr, 0));
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

bool parse_options(const char* text, Options& o, std::string& err) {
    o.reg[4] = {1, LT_DATA_BASE + 0xF000, 0};
    o.reg[5] = {1, LT_DATA_BASE + 0x8000, 0};
    for (const std::string& tok : split(text, ' ')) {
        const size_t eq = tok.find('=');
        const std::string k = tok.substr(0, eq), v = eq == std::string::npos ? "" : tok.substr(eq + 1);
        int r = -1;
        for (int i = 0; i < 8; ++i)
            if (k == kRegNames[i]) r = i;
        if (r >= 0) {
            if (const size_t dots = v.find(".."); dots != std::string::npos)
                o.reg[r] = {2, parse_value(v.substr(0, dots)), parse_value(v.substr(dots + 2))};
            else
                o.reg[r] = {1, parse_value(v), 0};
        } else if (k == "df") {
            o.df = v == "r" ? -1 : atoi(v.c_str());
        } else if (k == "skip") {
            for (const std::string& f : split(v, ','))
                for (int i = 0; i < 6; ++i)
                    if (f == kFlagNames[i]) o.skip |= kFlagBits[i];
        } else if (k == "noflags") {
            o.skip = kArith;
        } else if (k == "mem") {
            o.mem = v;
        } else if (k == "xmm") {
            o.xmm = v;
        } else if (k == "f32" || k == "f64") {
            for (const std::string& off : split(v, ',')) (k == "f32" ? o.f32 : o.f64).push_back(parse_value(off));
        } else if (k == "iters") {
            o.iters = atoi(v.c_str());
        } else {
            err = "unknown option '" + tok + "'";
            return false;
        }
    }
    return true;
}

struct State {
    uint32_t gpr[8];
    uint32_t eflags;  // arithmetic flags plus DF (bit 10)
    XMM xmm[8];
    uint32_t ftop;
    std::vector<uint8_t> mem;
};

bool same_float(double a, double b, double tol) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == b) return true;
    return std::fabs(a - b) <= tol * std::fmax(std::fabs(a), std::fabs(b));
}

class Harness {
public:
    bool init() {
        host_mem_.assign(LT_DATA_BASE + LT_DATA_SIZE, 0);
        recomp_mem = host_mem_.data();
        return open_engine();
    }

    // Returns 0 ok, 1 skipped (guest faulted, e.g. #DE), -1 harness error.
    int run_unicorn(const LifterCase& lc, const State& in, State& out, std::string& err) {
        // A guest exception leaves the engine unable to run again, so start afresh.
        if (intno_ >= 0) {
            uc_close(uc_);
            if (!open_engine()) {
                err = "unicorn reopen failed";
                return -1;
            }
        }
        intno_ = -1;
        // Both runs use the same instruction limit: toggling it flushes Unicorn's TB cache.
        if (uc_emu_start(uc_, kResetStub, kResetStub + sizeof kResetCode, 0, kMaxInsns) != UC_ERR_OK) {
            err = "reset stub failed";
            return -1;
        }
        for (int i = 0; i < 8; ++i) uc_reg_write(uc_, kUcRegs[i], &in.gpr[i]);
        const uint32_t ef = in.eflags | 0x202u;  // IF is set in user mode (pushfd)
        uc_reg_write(uc_, UC_X86_REG_EFLAGS, &ef);
        for (int i = 0; i < 8; ++i) uc_reg_write(uc_, kUcXmm[i], &in.xmm[i]);
        const uint32_t mxcsr = 0x1F80;
        uc_reg_write(uc_, UC_X86_REG_MXCSR, &mxcsr);
        uc_mem_write(uc_, LT_DATA_BASE, in.mem.data(), LT_DATA_SIZE);
        const uc_err e = uc_emu_start(uc_, lc.addr, lc.addr + lc.len, 0, kMaxInsns);
        if (intno_ >= 0) return 1;
        if (e != UC_ERR_OK) {
            err = std::string("unicorn: ") + uc_strerror(e);
            return -1;
        }
        uint32_t pc = 0;
        uc_reg_read(uc_, UC_X86_REG_EIP, &pc);
        if (pc != lc.addr + lc.len) {
            char b[96];
            snprintf(b, sizeof b, "unicorn stopped early at %08X (instruction limit?)", pc);
            err = b;
            return -1;
        }
        for (int i = 0; i < 8; ++i) uc_reg_read(uc_, kUcRegs[i], &out.gpr[i]);
        uc_reg_read(uc_, UC_X86_REG_EFLAGS, &out.eflags);
        out.eflags &= kArith | 0x400;
        for (int i = 0; i < 8; ++i) uc_reg_read(uc_, kUcXmm[i], &out.xmm[i]);
        uint32_t fpsw = 0;
        uc_reg_read(uc_, UC_X86_REG_FPSW, &fpsw);
        out.ftop = (fpsw >> 11) & 7;
        out.mem.resize(LT_DATA_SIZE);
        uc_mem_read(uc_, LT_DATA_BASE, out.mem.data(), LT_DATA_SIZE);
        return 0;
    }

    void run_lifted(recomp_fn fn, const State& in, State& out) {
        X86 c{};
        memcpy(&c.eax, in.gpr, sizeof in.gpr);
        c.eflags = in.eflags | 2u;
        c.df = (in.eflags >> 10) & 1;
        memcpy(c.xmm, in.xmm, sizeof c.xmm);
        c.mxcsr = 0x1F80;
        c.fcw = kFcw;
        c.ftag = 0xFFFF;
        memcpy(&host_mem_[LT_DATA_BASE], in.mem.data(), LT_DATA_SIZE);
        g_runtime_error.clear();
        fn(&c);
        c.esp -= 4;  // the lifted ret popped the (absent) return address
        memcpy(out.gpr, &c.eax, sizeof out.gpr);
        out.eflags = (c.eflags & kArith) | (c.df << 10);
        memcpy(out.xmm, c.xmm, sizeof c.xmm);
        out.ftop = c.ftop & 7;
        out.mem.assign(host_mem_.begin() + LT_DATA_BASE, host_mem_.end());
    }

private:
    bool open_engine() {
        if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc_) != UC_ERR_OK) return false;
        const uint32_t code_size = (lifter_case_count * LT_CODE_STRIDE + 0x1FFF) & ~0xFFFu;
        uc_mem_map(uc_, kResetStub, code_size + 0x1000, UC_PROT_READ | UC_PROT_EXEC);
        uc_mem_map(uc_, LT_DATA_BASE, LT_DATA_SIZE, UC_PROT_READ | UC_PROT_WRITE);
        uc_mem_write(uc_, kResetStub, kResetCode, sizeof kResetCode);
        uc_mem_write(uc_, kResetStub + 0x100, &kFcw, 2);
        for (uint32_t i = 0; i < lifter_case_count; ++i)
            uc_mem_write(uc_, lifter_cases[i].addr, lifter_cases[i].code, lifter_cases[i].len + 1);
        uint32_t cr0 = 0, cr4 = 0;
        uc_reg_read(uc_, UC_X86_REG_CR0, &cr0);
        uc_reg_read(uc_, UC_X86_REG_CR4, &cr4);
        cr0 = (cr0 & ~4u) | 2u;    // no EM, MP set: x87 and SSE available
        cr4 |= (1u << 9) | (1u << 10);  // OSFXSR, OSXMMEXCPT
        uc_reg_write(uc_, UC_X86_REG_CR0, &cr0);
        uc_reg_write(uc_, UC_X86_REG_CR4, &cr4);
        uc_hook_add(uc_, &hook_, UC_HOOK_INTR, reinterpret_cast<void*>(&Harness::on_intr), this, 1, 0);
        return true;
    }

    static void on_intr(uc_engine* uc, uint32_t intno, void* user) {
        static_cast<Harness*>(user)->intno_ = int(intno);
        uc_emu_stop(uc);
    }
    uc_engine* uc_ = nullptr;
    uc_hook hook_{};
    int intno_ = -1;
    std::vector<uint8_t> host_mem_;
};

void random_state(Rng& rng, const Options& o, State& s) {
    for (int i = 0; i < 8; ++i) {
        const Options::RegInit& r = o.reg[i];
        s.gpr[i] = r.mode == 1 ? r.lo : r.mode == 2 ? r.lo + rng.below(r.hi - r.lo + 1) : rng.biased();
    }
    s.eflags = 0;
    for (uint32_t f : kFlagBits)
        if (rng.below(2)) s.eflags |= f;
    if (o.df == 1 || (o.df < 0 && rng.below(2))) s.eflags |= 0x400;
    for (XMM& x : s.xmm) {
        for (int k = 0; k < 4; ++k) {
            if (o.xmm == "f32") x.f[k] = rng.nice_f32();
            else x.u[k] = rng.below(4) ? rng.u32() : rng.biased();
        }
        if (o.xmm == "f64")
            for (int k = 0; k < 2; ++k) x.d[k] = rng.nice_f64();
    }
    s.ftop = 0;
    s.mem.resize(LT_DATA_SIZE);
    if (o.mem == "f32") {
        for (uint32_t a = 0; a < LT_DATA_SIZE; a += 4) {
            const float f = rng.nice_f32();
            memcpy(&s.mem[a], &f, 4);
        }
    } else if (o.mem == "f64") {
        for (uint32_t a = 0; a < LT_DATA_SIZE; a += 8) {
            const double d = rng.nice_f64();
            memcpy(&s.mem[a], &d, 8);
        }
    } else {
        // Raw random bits with an edge-case value every few dwords (the fill dominates run time).
        for (uint32_t a = 0; a < LT_DATA_SIZE; a += 8) {
            uint64_t v = rng.next();
            if ((v & 3) == 0) v = (v & ~0xFFFFFFFFull) | rng.biased();
            memcpy(&s.mem[a], &v, 8);
        }
    }
}

std::string describe(const State& s) {
    std::string o;
    char b[160];
    for (int i = 0; i < 8; ++i) {
        snprintf(b, sizeof b, "%s=%08X ", kRegNames[i], s.gpr[i]);
        o += b;
    }
    o += "flags=";
    for (int i = 0; i < 6; ++i)
        if (s.eflags & kFlagBits[i]) o += std::string(kFlagNames[i]) + " ";
    if (s.eflags & 0x400) o += "df ";
    return o;
}

// Compares lifted output against Unicorn's; returns a list of differences.
std::string compare(const LifterCase& lc, const Options& o, bool live, const State& want, const State& got) {
    std::string d;
    char b[200];
    for (int i = 0; i < 8; ++i)
        if (want.gpr[i] != got.gpr[i]) {
            snprintf(b, sizeof b, "  %s: unicorn %08X lifted %08X\n", kRegNames[i], want.gpr[i], got.gpr[i]);
            d += b;
        }
    const uint32_t fmask = live ? (kArith & ~lc.undef_flags & ~o.skip) : 0;
    for (int i = 0; i < 6; ++i)
        if ((fmask & kFlagBits[i]) && (want.eflags & kFlagBits[i]) != (got.eflags & kFlagBits[i])) {
            snprintf(b, sizeof b, "  %s: unicorn %d lifted %d\n", kFlagNames[i], !!(want.eflags & kFlagBits[i]),
                     !!(got.eflags & kFlagBits[i]));
            d += b;
        }
    if ((want.eflags ^ got.eflags) & 0x400) d += "  df differs\n";
    if (want.ftop != got.ftop) {
        snprintf(b, sizeof b, "  x87 top: unicorn %u lifted %u\n", want.ftop, got.ftop);
        d += b;
    }
    for (int i = 0; i < 8; ++i) {
        bool same = true;
        if (o.xmm == "f32") {
            for (int k = 0; k < 4; ++k) {
                const float a = want.xmm[i].f[k], g = got.xmm[i].f[k];
                same &= (std::isnan(a) && std::isnan(g)) || want.xmm[i].u[k] == got.xmm[i].u[k];
            }
        } else if (o.xmm == "f64") {
            for (int k = 0; k < 2; ++k) {
                const double a = want.xmm[i].d[k], g = got.xmm[i].d[k];
                same &= (std::isnan(a) && std::isnan(g)) || want.xmm[i].q[k] == got.xmm[i].q[k];
            }
        } else {
            same = memcmp(&want.xmm[i], &got.xmm[i], 16) == 0;
        }
        if (!same) {
            snprintf(b, sizeof b, "  xmm%d: unicorn %08X %08X %08X %08X lifted %08X %08X %08X %08X\n", i,
                     want.xmm[i].u[3], want.xmm[i].u[2], want.xmm[i].u[1], want.xmm[i].u[0], got.xmm[i].u[3],
                     got.xmm[i].u[2], got.xmm[i].u[1], got.xmm[i].u[0]);
            d += b;
        }
    }
    if (o.f32.empty() && o.f64.empty() && memcmp(want.mem.data(), got.mem.data(), LT_DATA_SIZE) == 0) return d;
    // Float slots first, then the rest byte for byte.
    std::vector<bool> fuzzy(LT_DATA_SIZE, false);
    auto float_slot = [&](uint32_t off, int size) {
        const uint32_t a = off - LT_DATA_BASE;
        if (a + size > LT_DATA_SIZE) return;
        for (int k = 0; k < size; ++k) fuzzy[a + k] = true;
        double w, g;
        if (size == 4) {
            float fw, fg;
            memcpy(&fw, &want.mem[a], 4);
            memcpy(&fg, &got.mem[a], 4);
            w = fw, g = fg;
        } else {
            memcpy(&w, &want.mem[a], 8);
            memcpy(&g, &got.mem[a], 8);
        }
        if (!same_float(w, g, size == 4 ? 1e-6 : 1e-12)) {
            snprintf(b, sizeof b, "  f%d [%08X]: unicorn %.17g lifted %.17g\n", size * 8, off, w, g);
            d += b;
        }
    };
    for (uint32_t off : o.f32) float_slot(off, 4);
    for (uint32_t off : o.f64) float_slot(off, 8);
    int shown = 0;
    for (uint32_t a = 0; a < LT_DATA_SIZE; ++a)
        if (!fuzzy[a] && want.mem[a] != got.mem[a] && shown++ < 8) {
            snprintf(b, sizeof b, "  mem [%08X]: unicorn %02X lifted %02X\n", LT_DATA_BASE + a, want.mem[a], got.mem[a]);
            d += b;
        }
    return d;
}

uint64_t fnv(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) h = (h ^ uint8_t(*s)) * 1099511628211ull;
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* filter = nullptr;
    int iters_override = 0;
    uint64_t seed = 0;
    uint32_t shard = 0, shards = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iters" && i + 1 < argc) iters_override = atoi(argv[++i]);
        else if (a == "--shard" && i + 1 < argc) sscanf(argv[++i], "%u/%u", &shard, &shards);
        else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 0);
        else filter = argv[i];
    }
    Harness h;
    if (!h.init()) {
        fprintf(stderr, "unicorn init failed\n");
        return 1;
    }
    int failed = 0, ran = 0;
    for (uint32_t ci = 0; ci < lifter_case_count; ++ci) {
        const LifterCase& lc = lifter_cases[ci];
        if (filter && !strstr(lc.name, filter)) continue;
        if (shards > 1 && ci % shards != shard) continue;
        ++ran;
        Options o;
        std::string err;
        if (!parse_options(lc.options, o, err)) {
            printf("FAIL %s: %s\n", lc.name, err.c_str());
            ++failed;
            continue;
        }
        Rng rng{fnv(lc.name) ^ seed};
        const int iters = iters_override ? iters_override : o.iters;
        int valid = 0;
        bool bad = false;
        for (int it = 0; it < iters && !bad; ++it) {
            State in, want, got;
            random_state(rng, o, in);
            const int r = h.run_unicorn(lc, in, want, err);
            if (r < 0) {
                printf("FAIL %s: %s\n", lc.name, err.c_str());
                bad = true;
                break;
            }
            if (r > 0) continue;
            ++valid;
            for (int live = 1; live >= 0 && !bad; --live) {
                h.run_lifted(live ? lc.live : lc.dead, in, got);
                std::string diff = g_runtime_error.empty() ? compare(lc, o, live != 0, want, got)
                                                           : "  runtime: " + g_runtime_error + "\n";
                if (!diff.empty()) {
                    printf("FAIL %s [%s, iteration %d]\n  asm: %s\n  in:  %s\n%s", lc.name, live ? "flags live" : "flags dead",
                           it, lc.asm_text, describe(in).c_str(), diff.c_str());
                    bad = true;
                }
            }
        }
        if (!bad && valid == 0) {
            printf("FAIL %s: every iteration faulted in Unicorn\n", lc.name);
            bad = true;
        }
        failed += bad;
    }
    printf("%d/%d cases passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
