/* Resumable frames for structured exception handling.
 *
 * Functions that install SEH/C++ EH frames register a RecompFrame on entry.
 * When an exception handler transfers control to a continuation label inside
 * such a function (catch continuation, __except block), the dispatcher finds
 * the innermost active frame owning that label and longjmps into it; the
 * function's landing pad then jumps to the label.
 */
#ifndef RECOMP_SEH_H
#define RECOMP_SEH_H

#include <setjmp.h>

#include "recomp/x86.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompFrame {
    jmp_buf jb;
    const uint32_t* labels; /* sorted block addresses of the owning function */
    uint32_t nlabels;
    uint32_t target;        /* label to resume at after a longjmp */
    struct RecompFrame* prev;
} RecompFrame;

void recomp_frame_enter(RecompFrame* f, const uint32_t* labels, uint32_t n);
void recomp_frame_leave(RecompFrame* f);

/* Resumes at a label of an active frame; returns only if none owns it. */
void recomp_resume_at(uint32_t target);

#ifdef __APPLE__
#define RECOMP_SETJMP(jb) _setjmp(jb)
#define RECOMP_LONGJMP(jb, v) _longjmp(jb, v)
#else
#define RECOMP_SETJMP(jb) _setjmp(jb)
#define RECOMP_LONGJMP(jb, v) _longjmp(jb, v)
#endif

#ifdef __cplusplus
}
#endif

#endif
