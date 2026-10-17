/* String instruction helpers used by generated code. */
#ifndef RECOMP_OPS_H
#define RECOMP_OPS_H

#include "recomp/x86.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t recomp_rd_n(uint32_t a, int n) {
    return n == 1 ? rd8(a) : n == 2 ? rd16(a) : rd32(a);
}
static inline void recomp_wr_n(uint32_t a, uint32_t v, int n) {
    if (n == 1) wr8(a, (uint8_t)v);
    else if (n == 2) wr16(a, (uint16_t)v);
    else wr32(a, v);
}

/* rep movs: forward copies that do not overlap destructively become memmove. */
static inline void recomp_movs(X86* c, int n, int rep) {
    if (!rep) {
        recomp_wr_n(c->edi, recomp_rd_n(c->esi, n), n);
        const int32_t d = c->df ? -n : n;
        c->esi += d;
        c->edi += d;
        return;
    }
    const uint32_t bytes = c->ecx * (uint32_t)n;
    if (!c->df && (c->edi <= c->esi || c->edi >= c->esi + bytes)) {
        memmove(RECOMP_MEM(c->edi), RECOMP_MEM(c->esi), bytes);
        c->esi += bytes;
        c->edi += bytes;
        c->ecx = 0;
        return;
    }
    const int32_t d = c->df ? -n : n;
    while (c->ecx) {
        recomp_wr_n(c->edi, recomp_rd_n(c->esi, n), n);
        c->esi += d;
        c->edi += d;
        c->ecx--;
    }
}

static inline void recomp_stos(X86* c, int n, int rep) {
    const uint32_t v = n == 1 ? (uint8_t)c->eax : n == 2 ? (uint16_t)c->eax : c->eax;
    if (!rep) {
        recomp_wr_n(c->edi, v, n);
        c->edi += c->df ? -n : n;
        return;
    }
    if (!c->df && n == 1) {
        memset(RECOMP_MEM(c->edi), (int)v, c->ecx);
        c->edi += c->ecx;
        c->ecx = 0;
        return;
    }
    const int32_t d = c->df ? -n : n;
    while (c->ecx) {
        recomp_wr_n(c->edi, v, n);
        c->edi += d;
        c->ecx--;
    }
}

static inline void recomp_lods(X86* c, int n, int rep) {
    const int32_t d = c->df ? -n : n;
    do {
        if (rep && !c->ecx) break;
        const uint32_t v = recomp_rd_n(c->esi, n);
        if (n == 1) SET_LO8(c->eax, v);
        else if (n == 2) SET_LO16(c->eax, v);
        else c->eax = v;
        c->esi += d;
        if (rep) c->ecx--;
    } while (rep);
}

/* cmps/scas: rep 0 = single, 1 = repe, 2 = repne. Returns 1 if a comparison
 * happened, with its operands in *a and *b for flag computation. */
static inline int recomp_cmps(X86* c, int n, int rep, uint32_t* a, uint32_t* b) {
    const int32_t d = c->df ? -n : n;
    int any = 0;
    for (;;) {
        if (rep && !c->ecx) break;
        *a = recomp_rd_n(c->esi, n);
        *b = recomp_rd_n(c->edi, n);
        any = 1;
        c->esi += d;
        c->edi += d;
        if (!rep) break;
        c->ecx--;
        if (rep == 1 && *a != *b) break;
        if (rep == 2 && *a == *b) break;
    }
    return any;
}

static inline int recomp_scas(X86* c, int n, int rep, uint32_t* a, uint32_t* b) {
    const int32_t d = c->df ? -n : n;
    const uint32_t v = n == 1 ? (uint8_t)c->eax : n == 2 ? (uint16_t)c->eax : c->eax;
    int any = 0;
    for (;;) {
        if (rep && !c->ecx) break;
        *a = v;
        *b = recomp_rd_n(c->edi, n);
        any = 1;
        c->edi += d;
        if (!rep) break;
        c->ecx--;
        if (rep == 1 && *a != *b) break;
        if (rep == 2 && *a == *b) break;
    }
    return any;
}

#ifdef __cplusplus
}
#endif

#endif
