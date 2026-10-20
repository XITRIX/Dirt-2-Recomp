#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
extern "C" {
uint8_t* recomp_mem;
void baseline_memory(uint32_t,unsigned,const uint8_t*,uint8_t*);
void inline_memory(uint32_t,unsigned,const uint8_t*,uint8_t*);
}
int main() {
    alignas(64) std::array<uint8_t,256> a,b;
    std::array<uint8_t,16> input,x,y;
    std::mt19937 rng(123);
    for(unsigned trial=0;trial<200000;++trial) {
        for(auto& v:a) v=uint8_t(rng());
        b=a; for(auto& v:input) v=uint8_t(rng());
        for(unsigned size:{1u,2u,4u,8u,16u}) {
            const unsigned address=32+(trial%64);
            x.fill(0);y.fill(0);
            recomp_mem=a.data();baseline_memory(address,size,input.data(),x.data());
            recomp_mem=b.data();inline_memory(address,size,input.data(),y.data());
            assert(x==y && a==b);
            for(unsigned i=0;i<size;++i) assert(x[i]==input[i]);
        }
    }
    std::puts("PASS: one million aligned/unaligned memory operations, identical bytes and surrounding memory");
}
