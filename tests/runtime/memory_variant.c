#include "recomp/x86.h"
#ifndef VARIANT
#define VARIANT baseline_memory
#endif
void VARIANT(uint32_t address, unsigned size, const uint8_t* input, uint8_t* output) {
    switch(size) {
    case 1: wr8(address,*input); *output=rd8(address); break;
    case 2: { uint16_t v;memcpy(&v,input,2);wr16(address,v);v=rd16(address);memcpy(output,&v,2);break; }
    case 4: { uint32_t v;memcpy(&v,input,4);wr32(address,v);v=rd32(address);memcpy(output,&v,4);break; }
    case 8: { uint64_t v;memcpy(&v,input,8);wr64(address,v);v=rd64(address);memcpy(output,&v,8);break; }
    case 16: { XMM v;memcpy(&v,input,16);wr128(address,v);v=rd128(address);memcpy(output,&v,16);break; }
    }
}
