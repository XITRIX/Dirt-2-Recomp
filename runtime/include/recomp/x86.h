/* Guest CPU model shared by the generated code and the runtime.
 *
 * Every guest thread owns one X86 context. General registers, DF, the x87
 * stack and the SSE registers live here; arithmetic flags are locals inside
 * each generated function (they are dead across calls in MSVC code) and are
 * only spilled to `eflags` for pushfd/lahf-style access and live-in reads.
 *
 * Guest memory is one 4 GB reservation: guest address a is host pointer
 * recomp_mem + a. All guest pointers stay 32-bit.
 */
#ifndef RECOMP_X86_H
#define RECOMP_X86_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union XMM {
    float f[4];
    double d[2];
    uint8_t b[16];
    int8_t sb[16];
    uint16_t w[8];
    int16_t sw[8];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
    int64_t sq[2];
} XMM;

typedef struct X86 {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eflags;   /* spilled flags (x86 bit layout) */
    uint32_t df;       /* direction flag, 0 or 1 */
    uint32_t fs_base;  /* guest address of the TEB */
    /* x87: values held as double, which matches MSVC's default 53-bit precision. */
    double st[8];
    uint32_t ftop;
    uint16_t fcw, fsw, ftag;
    XMM xmm[8];
    uint32_t mxcsr;
    void* host;        /* runtime thread data */
} X86;

typedef void (*recomp_fn)(X86*);

extern uint8_t* recomp_mem;

#define RECOMP_MEM(a) (recomp_mem + (uint32_t)(a))

/* Debug write-watch (DIRT2_WATCH build option): every guest store that
 * overlaps [recomp_watch_lo, recomp_watch_hi) is recorded by the runtime. */
#if defined(RECOMP_WATCH)
extern uint32_t recomp_watch_lo, recomp_watch_hi;
void recomp_watch_hit(uint32_t a, uint64_t v, uint32_t size);
#define RECOMP_WATCH_STORE(a, v, n)                                                   \
    do {                                                                              \
        if (__builtin_expect((a) < recomp_watch_hi && (a) + (n) > recomp_watch_lo, 0)) \
            recomp_watch_hit((a), (uint64_t)(v), (n));                                \
    } while (0)
#else
#define RECOMP_WATCH_STORE(a, v, n) ((void)0)
#endif

/* Optional CPU experiment: preserve acquire/release and unaligned fallbacks,
 * but prevent GCC from outlining each tiny guest read/write in hot code. */
#if defined(RECOMP_INLINE_GUEST_MEMORY) && (defined(__GNUC__) || defined(__clang__))
#define RECOMP_MEM_INLINE static inline __attribute__((always_inline))
#else
#define RECOMP_MEM_INLINE static inline
#endif

#if defined(RECOMP_TSO)
/* x86 TSO on a weakly ordered host: every guest load is an acquire and every
 * store a release (ldar/stlr on ARM64), so cross-thread publication patterns
 * written for x86 stay correct. Unaligned accesses (rare) use plain copies
 * with fences, since ldar/stlr require natural alignment. */
#define RECOMP_LD(T, a)                                                              \
    do {                                                                             \
        const uint8_t* p_ = RECOMP_MEM(a);                                           \
        if (__builtin_expect(((uintptr_t)p_ & (sizeof(T) - 1)) == 0, 1))             \
            return __atomic_load_n((const T*)p_, __ATOMIC_ACQUIRE);                   \
        T v_;                                                                        \
        memcpy(&v_, p_, sizeof(T));                                                  \
        __atomic_thread_fence(__ATOMIC_ACQUIRE);                                     \
        return v_;                                                                   \
    } while (0)
#define RECOMP_ST(T, a, v)                                                           \
    do {                                                                             \
        uint8_t* p_ = RECOMP_MEM(a);                                                 \
        if (__builtin_expect(((uintptr_t)p_ & (sizeof(T) - 1)) == 0, 1)) {           \
            __atomic_store_n((T*)p_, (v), __ATOMIC_RELEASE);                          \
            return;                                                                  \
        }                                                                            \
        __atomic_thread_fence(__ATOMIC_RELEASE);                                     \
        const T v_ = (v);                                                            \
        memcpy(p_, &v_, sizeof(T));                                                  \
    } while (0)
RECOMP_MEM_INLINE uint8_t rd8(uint32_t a) { return __atomic_load_n(RECOMP_MEM(a), __ATOMIC_ACQUIRE); }
RECOMP_MEM_INLINE uint16_t rd16(uint32_t a) { RECOMP_LD(uint16_t, a); }
RECOMP_MEM_INLINE uint32_t rd32(uint32_t a) { RECOMP_LD(uint32_t, a); }
RECOMP_MEM_INLINE uint64_t rd64(uint32_t a) { RECOMP_LD(uint64_t, a); }
RECOMP_MEM_INLINE void wr8(uint32_t a, uint8_t v) { RECOMP_WATCH_STORE(a, v, 1); __atomic_store_n(RECOMP_MEM(a), v, __ATOMIC_RELEASE); }
RECOMP_MEM_INLINE void wr16(uint32_t a, uint16_t v) { RECOMP_WATCH_STORE(a, v, 2); RECOMP_ST(uint16_t, a, v); }
RECOMP_MEM_INLINE void wr32(uint32_t a, uint32_t v) { RECOMP_WATCH_STORE(a, v, 4); RECOMP_ST(uint32_t, a, v); }
RECOMP_MEM_INLINE void wr64(uint32_t a, uint64_t v) { RECOMP_WATCH_STORE(a, v, 8); RECOMP_ST(uint64_t, a, v); }
RECOMP_MEM_INLINE XMM rd128(uint32_t a) {
    XMM v;
    memcpy(&v, RECOMP_MEM(a), 16);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return v;
}
RECOMP_MEM_INLINE void wr128(uint32_t a, XMM v) {
    RECOMP_WATCH_STORE(a, v.q[0], 16);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(RECOMP_MEM(a), &v, 16);
}
#else
RECOMP_MEM_INLINE uint8_t rd8(uint32_t a) { return *RECOMP_MEM(a); }
RECOMP_MEM_INLINE uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, RECOMP_MEM(a), 2); return v; }
RECOMP_MEM_INLINE uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, RECOMP_MEM(a), 4); return v; }
RECOMP_MEM_INLINE uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, RECOMP_MEM(a), 8); return v; }
RECOMP_MEM_INLINE XMM rd128(uint32_t a) { XMM v; memcpy(&v, RECOMP_MEM(a), 16); return v; }
RECOMP_MEM_INLINE void wr8(uint32_t a, uint8_t v) { RECOMP_WATCH_STORE(a, v, 1); *RECOMP_MEM(a) = v; }
RECOMP_MEM_INLINE void wr16(uint32_t a, uint16_t v) { RECOMP_WATCH_STORE(a, v, 2); memcpy(RECOMP_MEM(a), &v, 2); }
RECOMP_MEM_INLINE void wr32(uint32_t a, uint32_t v) { RECOMP_WATCH_STORE(a, v, 4); memcpy(RECOMP_MEM(a), &v, 4); }
RECOMP_MEM_INLINE void wr64(uint32_t a, uint64_t v) { RECOMP_WATCH_STORE(a, v, 8); memcpy(RECOMP_MEM(a), &v, 8); }
RECOMP_MEM_INLINE void wr128(uint32_t a, XMM v) { RECOMP_WATCH_STORE(a, v.q[0], 16); memcpy(RECOMP_MEM(a), &v, 16); }
#endif
RECOMP_MEM_INLINE float rdf32(uint32_t a) { const uint32_t u = rd32(a); float v; memcpy(&v, &u, 4); return v; }
RECOMP_MEM_INLINE double rdf64(uint32_t a) { const uint64_t u = rd64(a); double v; memcpy(&v, &u, 8); return v; }
RECOMP_MEM_INLINE void wrf32(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); wr32(a, u); }
RECOMP_MEM_INLINE void wrf64(uint32_t a, double v) { uint64_t u; memcpy(&u, &v, 8); wr64(a, u); }

#undef RECOMP_MEM_INLINE

/* xchg with a memory operand is implicitly locked on x86. */
#define RECOMP_XCHG(T, name)                                                          \
    static inline T name(uint32_t a, T v) {                                           \
        T* p = (T*)RECOMP_MEM(a);                                                     \
        RECOMP_WATCH_STORE(a, v, sizeof(T));                                          \
        if (__builtin_expect(((uintptr_t)p & (sizeof(T) - 1)) == 0, 1))               \
            return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);                       \
        T old;                                                                        \
        memcpy(&old, p, sizeof(T));                                                   \
        memcpy(p, &v, sizeof(T));                                                     \
        return old;                                                                   \
    }
RECOMP_XCHG(uint8_t, xchg8)
RECOMP_XCHG(uint16_t, xchg16)
RECOMP_XCHG(uint32_t, xchg32)

/* Partial register writes. */
#define SET_LO8(r, v) ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET_HI8(r, v) ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

static inline uint8_t recomp_parity(uint32_t r) { return (uint8_t)!__builtin_parity(r & 0xFF); }

static inline void push32(X86* c, uint32_t v) { c->esp -= 4; wr32(c->esp, v); }
static inline uint32_t pop32(X86* c) { uint32_t v = rd32(c->esp); c->esp += 4; return v; }

/* Runtime services used by generated code. */
void recomp_call(X86* c, uint32_t target);  /* indirect call/jump through the dispatch table */
void recomp_unimpl(X86* c, uint32_t addr, const char* what);
void recomp_trap(X86* c, uint32_t addr, const char* what);
void recomp_cpuid(X86* c);
uint64_t recomp_rdtsc(void);
void recomp_div_error(X86* c, uint32_t addr);
void recomp_esp_mismatch(X86* c, uint32_t fn, uint32_t esp0, uint32_t ret_addr);

/* Rebuilds eflags from individual flag values. Bit 1 is always set. */
static inline uint32_t recomp_pack_flags(uint32_t cf, uint32_t pf, uint32_t af, uint32_t zf, uint32_t sf,
                                         uint32_t of, uint32_t df) {
    return 0x2u | cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (df << 10) | (of << 11);
}

/* ---- x87 ---- */
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(X86* c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline double fpop(X86* c) { double v = c->st[c->ftop]; c->ftop = (c->ftop + 1) & 7; return v; }
double recomp_rdf80(uint32_t a);
void recomp_wrf80(uint32_t a, double v);
/* Rounds per the x87 control word's RC field. */
static inline double recomp_fround(X86* c, double v) {
    switch ((c->fcw >> 10) & 3) {
        case 0: return nearbyint(v);
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}
/* Sets C0/C2/C3 in the status word from comparing a with b. */
static inline void fcom_sw(X86* c, double a, double b) {
    uint16_t sw = c->fsw & ~0x4500;
    if (a != a || b != b) sw |= 0x4500;
    else if (a < b) sw |= 0x0100;
    else if (a == b) sw |= 0x4000;
    c->fsw = sw;
}
/* fprem (truncating) / fprem1 (round to nearest) as one complete reduction:
 * C2 = 0 and C0, C3, C1 = quotient bits 2, 1, 0. */
static inline void recomp_fprem(X86* c, int nearest) {
    const double x = ST(0), y = ST(1);
    int qn = 0;
    const double r1 = remquo(x, y, &qn);
    uint32_t q = (uint32_t)(qn < 0 ? -qn : qn);
    double r = r1;
    if (!nearest) {
        r = fmod(x, y);
        if (r != r1) q -= 1; /* the nearest quotient rounded away from zero */
    }
    ST(0) = r;
    c->fsw = (uint16_t)((c->fsw & ~0x4700) | ((q & 4) ? 0x100 : 0) | ((q & 1) ? 0x200 : 0) | ((q & 2) ? 0x4000 : 0));
}
static inline uint16_t fstsw(X86* c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)); }

/* x86 integer conversions: out-of-range and NaN produce the "integer indefinite" value. */
static inline int32_t cvt_f2i32(double v) {
    return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v;
}
static inline int64_t cvt_f2i64(double v) {
    return (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
}
static inline int16_t cvt_f2i16(double v) {
    return (v != v || v >= 32768.0 || v < -32768.0) ? INT16_MIN : (int16_t)v;
}
/* SSE conversions round with MXCSR.RC; MSVC code only uses round-to-nearest here. */
static inline int32_t cvt_round_i32(X86* c, double v) {
    switch ((c->mxcsr >> 13) & 3) {
        case 0: v = nearbyint(v); break;
        case 1: v = floor(v); break;
        case 2: v = ceil(v); break;
        default: v = trunc(v); break;
    }
    return cvt_f2i32(v);
}

/* SSE min/max: return the second operand when either is NaN or both are zero. */
static inline float sse_minf(float a, float b) { return a < b ? a : b; }
static inline float sse_maxf(float a, float b) { return a > b ? a : b; }
static inline double sse_mind(double a, double b) { return a < b ? a : b; }
static inline double sse_maxd(double a, double b) { return a > b ? a : b; }

/* cmpps/cmpss predicate evaluation (imm8 0..7). */
static inline uint32_t sse_cmp(int pred, double a, double b) {
    int r;
    switch (pred & 7) {
        case 0: r = a == b; break;
        case 1: r = a < b; break;
        case 2: r = a <= b; break;
        case 3: r = (a != a) || (b != b); break;
        case 4: r = !(a == b); break;
        case 5: r = !(a < b); break;
        case 6: r = !(a <= b); break;
        default: r = (a == a) && (b == b); break;
    }
    return r ? 0xFFFFFFFFu : 0;
}

#ifdef __cplusplus
}
#endif

#endif
