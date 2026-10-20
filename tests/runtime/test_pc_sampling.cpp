#include "recomp/pc_sampling.h"
#include <array>
#include <cassert>
#include <cstdio>
int main() {
    for(unsigned pause_error:{0u,1u}) for(unsigned read_error:{0u,2u}) for(unsigned resume_error:{0u,3u}) {
        bool paused=false; unsigned n=0; std::array<unsigned,3> order{};
        auto result=rt::sampling::capture(
            [&] {order[n++]=1;paused=!pause_error;return pause_error;},
            [&] {assert(paused);order[n++]=2;return read_error;},
            [&] {assert(paused);order[n++]=3;paused=bool(resume_error);return resume_error;});
        assert(result.pause==pause_error);
        if(pause_error) {assert(n==1 && !result.suspended && !paused);}
        else {assert(n==3 && order[0]==1 && order[1]==2 && order[2]==3);assert(result.suspended && result.read==read_error && result.resume==resume_error);assert(paused==bool(resume_error));}
    }
    std::puts("PASS: failed reads still resume; failed pauses never resume; resume errors are reported");
}
