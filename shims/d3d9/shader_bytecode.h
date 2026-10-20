// D3D9 token-stream sizing, including the fixed operand counts of SM1.
#pragma once
#include <cstddef>
#include <cstdint>
namespace d3d9 {
inline uint32_t shader_bytecode_size(const uint32_t* code, size_t max_words=65536) {
    if(!code || !max_words)return 0;
    const uint32_t version=code[0],type=version>>16,major=(version>>8)&255,minor=version&255;
    if((type!=0xfffe && type!=0xffff) || major<1 || major>3)return 0;
    static constexpr int8_t operands[] = {
        0, 2, 3, 3, 4, 3, 2, 2, 3, 3, 3, 3, 3, 3, 2, 2,
        2, 3, 4, 2, 3, 3, 3, 3, 3, 1, 2, 2, 0, 0, 1, 2,
        3, 3, 4, 2, 2, 4, 1, 0, 1, 2, 0, 0, 0, 2, 2, 2,
        5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, -1, 3, 2, 2, 2,
        4, 5, 2, 2, 2, 2, 2, 1, 4, 3, 4, 2, 2, 5, 3, 3,
        1,
    };
    for(size_t i=1;i<max_words;) {
        const uint32_t token=code[i],opcode=token&0xffff;
        if(token==0x0000ffff)return uint32_t((i+1)*4);
        size_t count;
        if(opcode==0xfffe)count=(token>>16)&0x7fff;
        else if(opcode==0xfffd && major==1 && minor==4 && type==0xffff)count=0;
        else {
            if((token&0x80000000) || opcode>=sizeof(operands) || operands[opcode]<0)return 0;
            count=major>=2?(token>>24)&15:unsigned(operands[opcode]);
            if(major==1 && (opcode==64 || opcode==66))count=minor>=4?2:1;
        }
        if(count>=max_words-i)return 0;
        i+=1+count;
    }
    return 0;
}
} // namespace d3d9
