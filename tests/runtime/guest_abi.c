#include "recomp/seh.h"
#include <stddef.h>
// Cross-compiler ABI fixture; compare the emitted bytes with each target C compiler.
const uint32_t guest_abi[] = {
    sizeof(X86), _Alignof(X86), offsetof(X86,eax), offsetof(X86,esp),
    offsetof(X86,fs_base), offsetof(X86,st), offsetof(X86,ftop),
    offsetof(X86,fcw), offsetof(X86,xmm), offsetof(X86,mxcsr), offsetof(X86,host),
    sizeof(XMM), _Alignof(XMM), sizeof(jmp_buf), sizeof(RecompFrame),
    offsetof(RecompFrame,labels),offsetof(RecompFrame,target),offsetof(RecompFrame,prev)
};
