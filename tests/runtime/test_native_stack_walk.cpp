#include "recomp/native_stack_walk.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
    std::array<uint64_t,4> links{};
    unsigned reads = 0;
    auto read = [&](uint64_t at,uint64_t& prev,uint64_t& lr) {
        assert(at >= 0x1000 && at <= 0x1ff0 && !(at & 15));
        ++reads; prev = at + 0x20; lr = 0x8000 + (at - 0x1000); return true;
    };
    assert(rt::sampling::walk_native_frames(0x1000,0x1000,0x1000,0x2000,links,read) == 4);
    assert(reads == 4 && links[3] == 0x8060);
    for (uint64_t fp : std::array<uint64_t,6>{0,0xff0,0x1001,0x1ff8,0x2000,UINT64_MAX}) {
        reads = 0;
        assert(rt::sampling::walk_native_frames(fp,0x1000,0x1000,0x2000,links,read) == 0);
        assert(reads == 0);
    }
    reads = 0;
    assert(rt::sampling::walk_native_frames(0x1000,0x1010,0x1000,0x2000,links,read) == 0 && reads == 0);
    assert(rt::sampling::walk_native_frames(0x1000,0x1000,0x2000,0x1000,links,read) == 0);
    assert(rt::sampling::walk_native_frames(0x1000,0x1000,0x1000,0x1008,links,read) == 0);
    assert(rt::sampling::walk_native_frames(0x1000,0x2000,0x1000,0x2000,links,read) == 0);
    for (uint64_t previous : {0ull,0x1000ull,0xff0ull,0x2000ull}) {
        reads = 0;
        auto bad_next = [&](uint64_t at,uint64_t& prev,uint64_t& lr) { ++reads; prev=previous;lr=0x8000;return true; };
        assert(rt::sampling::walk_native_frames(0x1000,0x1000,0x1000,0x2000,links,bad_next) == 1 && reads == 1);
    }
    auto failure = [](uint64_t,uint64_t&,uint64_t&) { return false; };
    assert(rt::sampling::walk_native_frames(0x1000,0x1000,0x1000,0x2000,links,failure) == 0);
    for (uint64_t link : {0ull,0x8001ull}) {
        auto bad_link = [&](uint64_t,uint64_t& p,uint64_t& lr) {p=0x1020;lr=link;return true;};
        assert(rt::sampling::walk_native_frames(0x1000,0x1000,0x1000,0x2000,links,bad_link) == 0);
    }
    std::puts("PASS: bounded native frame walks, truncation, invalid/cyclic pointers and failed reads");
}
