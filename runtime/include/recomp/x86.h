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

static inline uint8_t rd8(uint32_t a) { return *RECOMP_MEM(a); }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, RECOMP_MEM(a), 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, RECOMP_MEM(a), 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, RECOMP_MEM(a), 8); return v; }
static inline float rdf32(uint32_t a) { float v; memcpy(&v, RECOMP_MEM(a), 4); return v; }
static inline double rdf64(uint32_t a) { double v; memcpy(&v, RECOMP_MEM(a), 8); return v; }
static inline XMM rd128(uint32_t a) { XMM v; memcpy(&v, RECOMP_MEM(a), 16); return v; }
static inline void wr8(uint32_t a, uint8_t v) { *RECOMP_MEM(a) = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(RECOMP_MEM(a), &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(RECOMP_MEM(a), &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(RECOMP_MEM(a), &v, 8); }
static inline void wrf32(uint32_t a, float v) { memcpy(RECOMP_MEM(a), &v, 4); }
static inline void wrf64(uint32_t a, double v) { memcpy(RECOMP_MEM(a), &v, 8); }
static inline void wr128(uint32_t a, XMM v) { memcpy(RECOMP_MEM(a), &v, 16); }

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
