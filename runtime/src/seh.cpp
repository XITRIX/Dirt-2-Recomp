// Win32 SEH on the guest fs:[0] chain, plus resumable host frames.
#include "recomp/seh.h"

#include <algorithm>
#include <cstring>

#include "recomp/host.h"

namespace {

thread_local RecompFrame* t_top = nullptr;

constexpr uint32_t kEnd = 0xFFFFFFFF;
constexpr uint32_t EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4;
constexpr uint32_t kRecordSize = 80, kContextSize = 0x2CC;

void fill_context(X86* c, uint32_t ctx, uint32_t eip, uint32_t esp) {
    std::memset(rt::host_ptr(ctx), 0, kContextSize);
    wr32(ctx + 0x00, 0x1003F);  // CONTEXT_ALL
    wr32(ctx + 0x9C, c->edi);
    wr32(ctx + 0xA0, c->esi);
    wr32(ctx + 0xA4, c->ebx);
    wr32(ctx + 0xA8, c->edx);
    wr32(ctx + 0xAC, c->ecx);
    wr32(ctx + 0xB0, c->eax);
    wr32(ctx + 0xB4, c->ebp);
    wr32(ctx + 0xB8, eip);
    wr32(ctx + 0xBC, 0x1B);
    wr32(ctx + 0xC0, c->eflags | 0x202);
    wr32(ctx + 0xC4, esp);
    wr32(ctx + 0xC8, 0x23);
}

}  // namespace

extern "C" {

void recomp_frame_enter(RecompFrame* f, const uint32_t* labels, uint32_t n) {
    f->labels = labels;
    f->nlabels = n;
    f->target = 0;
    f->prev = t_top;
    t_top = f;
}

void recomp_frame_leave(RecompFrame* f) {
    // Frames above f were abandoned by a longjmp past them; drop them too.
    if (t_top == f || !t_top) t_top = f->prev;
    else {
        for (RecompFrame* p = t_top; p; p = p->prev)
            if (p == f) {
                t_top = f->prev;
                return;
            }
    }
}

void recomp_resume_at(uint32_t target) {
    for (RecompFrame* f = t_top; f; f = f->prev) {
        if (std::binary_search(f->labels, f->labels + f->nlabels, target)) {
            t_top = f;
            f->target = target;
            RECOMP_LONGJMP(f->jb, 1);
        }
    }
}

}  // extern "C"

namespace rt {

// RaiseException: dispatches along the guest handler chain.
void raise_exception(X86* c, uint32_t code, uint32_t flags, uint32_t nparams, uint32_t params) {
    const uint32_t esp0 = c->esp;  // points at RaiseException's return address
    const uint32_t ret = rd32(esp0);
    c->esp = (esp0 - kRecordSize - kContextSize - 64) & ~15u;
    const uint32_t rec = c->esp + 32, ctx = rec + kRecordSize;
    std::memset(host_ptr(rec), 0, kRecordSize);
    wr32(rec + 0, code);
    wr32(rec + 4, flags & EXCEPTION_NONCONTINUABLE);
    wr32(rec + 12, ret);
    nparams = std::min<uint32_t>(nparams, 15);
    wr32(rec + 16, nparams);
    for (uint32_t i = 0; i < nparams; ++i) wr32(rec + 20 + 4 * i, rd32(params + 4 * i));
    fill_context(c, ctx, ret, esp0 + 4 + 16);

    const uint32_t teb = current_teb();
    for (uint32_t reg = rd32(teb); reg != kEnd && reg;) {
        const uint32_t handler = rd32(reg + 4);
        const uint32_t disp = invoke(c, handler, {rec, reg, ctx, 0}, false);
        if (disp == 0) {  // ExceptionContinueExecution
            if (flags & EXCEPTION_NONCONTINUABLE) fatal("handler continued a noncontinuable exception 0x%08X", code);
            c->eax = rd32(ctx + 0xB0);
            c->esp = esp0;
            return;
        }
        if (disp != 1) fatal("exception handler returned %u for 0x%08X", disp, code);
        reg = rd32(reg);
    }
    fatal("unhandled exception 0x%08X raised from 0x%08X (params %u: %08X %08X %08X)", code, ret, nparams,
          nparams > 0 ? rd32(rec + 20) : 0, nparams > 1 ? rd32(rec + 24) : 0, nparams > 2 ? rd32(rec + 28) : 0);
}

// RtlUnwind: calls unwind handlers for frames above target and unlinks them.
void rtl_unwind(X86* c, uint32_t target, uint32_t rec, uint32_t retval) {
    const uint32_t esp0 = c->esp;
    c->esp = (esp0 - kRecordSize - kContextSize - 64) & ~15u;
    const uint32_t ctx = c->esp + 32 + kRecordSize;
    if (!rec) {
        rec = c->esp + 32;
        std::memset(host_ptr(rec), 0, kRecordSize);
        wr32(rec + 0, 0xC0000027);  // STATUS_UNWIND
    }
    wr32(rec + 4, rd32(rec + 4) | EXCEPTION_UNWINDING | (target ? 0 : EXCEPTION_EXIT_UNWIND));
    fill_context(c, ctx, rd32(esp0), esp0 + 4 + 16);
    const uint32_t teb = current_teb();
    for (uint32_t reg = rd32(teb); reg != kEnd && reg && reg != target;) {
        const uint32_t handler = rd32(reg + 4);
        invoke(c, handler, {rec, reg, ctx, 0}, false);
        reg = rd32(reg);
        wr32(teb, reg);
    }
    c->esp = esp0;
    c->eax = retval;
}

}  // namespace rt
