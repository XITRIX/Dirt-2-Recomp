#include "recomp/x86.h"
#include "recomp/seh.h"
#include <stddef.h>

uint8_t* recomp_mem;
const unsigned arm_codegen_layout[] = {
    sizeof(X86), _Alignof(X86), offsetof(X86, st), offsetof(X86, xmm),
    offsetof(X86, mxcsr), offsetof(X86, host), sizeof(XMM), _Alignof(XMM),
    sizeof(RecompFrame), offsetof(RecompFrame, labels), sizeof(jmp_buf)
};

// Representative lifted operations: guest memory (including unaligned),
// separate SSE arithmetic, signed-zero/NaN-sensitive min/max and predicates.
// This is a compiler/ABI check, not a substitute for the lifter's x86 oracle.
void arm_codegen_fixture(X86* c) {
    const uint32_t a = 32 + (c->eax & 63);
    const uint32_t v = rd32(a);
    wr8(a + 128, rd8(a) ^ (uint8_t)c->ecx);
    wr16(a + 132, rd16(a + 1) ^ (uint16_t)c->edx);
    wr32(a + 140, v ^ c->ebx);
    wr64(a + 148, rd64(a + 3) ^ ((uint64_t)c->esi << 32 | c->edi));
    wr128(a + 164, rd128(a + 5));
    c->eax = rd32(a + 140);
    c->ecx = rd16(a + 132);
    for (unsigned i = 0; i < 4; ++i) {
        const float x = c->xmm[5].f[i], y = c->xmm[6].f[i], z = c->xmm[7].f[i];
        c->xmm[0].f[i] = x * y;
        c->xmm[0].f[i] = c->xmm[0].f[i] + z;
        c->xmm[1].f[i] = sse_minf(x, y);
        c->xmm[2].f[i] = sse_maxf(x, y);
        c->xmm[3].u[i] = sse_cmp(i, x, y);
        c->xmm[4].u[i] = (c->xmm[5].u[i] >> (i + 1)) ^ c->xmm[6].u[i];
    }
}
