// CPU services called from generated code, plus logging.
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "recomp/host.h"

namespace rt {

void log(const char* fmt, ...) {
    // One fprintf per line, with a millisecond timestamp since start, so lines
    // from different threads do not interleave.
    static const auto t0 = std::chrono::steady_clock::now();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[rt %6lld.%03lld] %s\n", ms / 1000, ms % 1000, buf);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[rt] FATAL (thread %u): ", current_thread_id());
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    if (X86* c = current_cpu())
        std::fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X [esp]=%08X\n",
                     c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, c->esp ? rd32(c->esp) : 0);
    if (X86* c = current_cpu()) {
        // Memory around the general registers that look like guest pointers.
        const uint32_t regs[] = {c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->ebp};
        const char* names[] = {"eax", "ecx", "edx", "ebx", "esi", "edi", "ebp"};
        for (int i = 0; i < 7; ++i) {
            const uint32_t r = regs[i];
            if (r < 0x10000 || r >= 0x80000000u) continue;
            std::fprintf(stderr, "  [%s=%08X]:", names[i], r);
            for (int k = 0; k < 8; ++k) std::fprintf(stderr, " %08X", rd32(r + 4 * k));
            std::fprintf(stderr, "\n");
        }
    }
    if (X86* c = current_cpu(); c && c->esp > 0x10000) {
        std::fprintf(stderr, "  stack:");
        for (int k = 0; k < 24; ++k) std::fprintf(stderr, "%s%08X", k % 8 ? " " : "\n    ", rd32(c->esp + 4 * k));
        std::fprintf(stderr, "\n");
    }
    void* frames[32];
    const int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
    dump_watch();
    std::fflush(stderr);
    // Let a stderr tee (the log file on iOS) write the lines above before the process dies.
    usleep(300 * 1000);
    std::abort();
}

namespace {

void crash_handler(int sig, siginfo_t* info, void*) {
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(recomp_mem);
    std::fprintf(stderr, "[rt] CRASH: signal %d at host %p", sig, info->si_addr);
    if (fault >= base && fault - base < (1ull << 32)) std::fprintf(stderr, " (guest 0x%08X)", uint32_t(fault - base));
    std::fprintf(stderr, "\n");
    if (X86* c = current_cpu())
        std::fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n", c->eax,
                     c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    void* frames[48];
    const int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, 2);
    dump_watch();
    _exit(128 + sig);
}

}  // namespace

void install_crash_handler() {
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(s, &sa, nullptr);
}

}  // namespace rt

extern "C" {

void recomp_unimpl(X86* c, uint32_t addr, const char* what) {
    rt::fatal("unimplemented instruction '%s' at 0x%08X", what, addr);
}

void recomp_trap(X86* c, uint32_t addr, const char* what) {
    rt::fatal("trap '%s' at 0x%08X", what, addr);
}

// --check-esp builds: a function reached ret with a different esp than on entry.
// The first few reports per function are logged; the innermost one points at
// the unbalanced callee (usually a shim with the wrong argument count).
void recomp_esp_mismatch(X86* c, uint32_t fn, uint32_t esp0, uint32_t ret_addr) {
    static std::mutex mu;
    static std::unordered_map<uint32_t, int> seen;
    std::lock_guard lk(mu);
    if (seen[fn]++ >= 2) return;
    std::fprintf(stderr, "[rt] esp imbalance in f_%08X at ret 0x%08X: entry %08X, now %08X (%+d) [thread %u]\n", fn, ret_addr,
                 esp0, c->esp, int(c->esp - esp0), rt::current_thread_id());
}

void recomp_div_error(X86* c, uint32_t addr) {
    rt::fatal("integer divide by zero at 0x%08X", addr);
}

void recomp_missing_import(X86* c, const char* dll, const char* name, uint16_t ordinal) {
    rt::fatal("missing import %s!%s (#%u), called from 0x%08X", dll, *name ? name : "?", ordinal, rd32(c->esp));
}

// Reports a Pentium 4-class CPU with SSE/SSE2 (what the game was built for).
void recomp_cpuid(X86* c) {
    switch (c->eax) {
        case 0:
            c->eax = 1;
            std::memcpy(&c->ebx, "Genu", 4);
            std::memcpy(&c->edx, "ineI", 4);
            std::memcpy(&c->ecx, "ntel", 4);
            break;
        case 1:
            c->eax = 0x00000F29;  // family 15
            c->ebx = 0x00040800;  // 4 logical processors
            c->ecx = 0;
            c->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
            break;
        case 0x80000000:
            c->eax = 0x80000000;
            c->ebx = c->ecx = c->edx = 0;
            break;
        default:
            c->eax = c->ebx = c->ecx = c->edx = 0;
            break;
    }
}

uint64_t recomp_rdtsc(void) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// 80-bit extended <-> double.
double recomp_rdf80(uint32_t a) {
    uint64_t mant;
    uint16_t se;
    std::memcpy(&mant, RECOMP_MEM(a), 8);
    std::memcpy(&se, RECOMP_MEM(a + 8), 2);
    const int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = std::ldexp(double(mant), exp - 16383 - 63);
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
        const double f = std::frexp(std::fabs(v), &e);  // f in [0.5, 1)
        mant = uint64_t(std::ldexp(f, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    std::memcpy(RECOMP_MEM(a), &mant, 8);
    std::memcpy(RECOMP_MEM(a + 8), &se, 2);
}

}  // extern "C"
