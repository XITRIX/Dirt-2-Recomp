#include "recomp/owned_guest_window.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    using namespace rt::owned_window;
    constexpr std::uint64_t total=1ull<<32, guard=0x10000, page=0x1000;
    assert(can_commit(guard,page,total,guard,page));
    assert(can_commit(total-page,page,total,guard,page));
    assert(!can_commit(0,page,total,guard,page));
    assert(!can_commit(guard-1,page,total,guard,page));
    assert(!can_commit(total,page,total,guard,page));
    assert(!can_commit(total-page,2*page,total,guard,page));
    assert(!can_commit(guard,UINT64_MAX,total,guard,page));
    assert(!can_commit(guard,0,total,guard,page));
    assert(!can_commit(guard,page,total,guard,3));
    assert(!can_commit(guard+1,page,total,guard,page));
    static_assert(kSwitch2HeapAlignment==0x10000);
    // Arena-style allocator: reject excessive alignment despite available size.
    // The successful pointer deliberately lacks the old 2 MiB alignment.
    auto* pointer=reinterpret_cast<void*>(0x400010000ull);
    unsigned allocations=0,protections=0,releases=0;
    auto allocate=[&](std::size_t align,std::size_t bytes) { ++allocations; assert(bytes==total); return align<=0x10000 ? pointer : nullptr; };
    auto protect=[&](void* p,std::size_t bytes)->std::uint32_t { ++protections; assert(p==pointer && bytes==total); return 0; };
    auto release=[&](void* p) { ++releases; assert(p==pointer); };
    auto result=acquire(total,kSwitch2HeapAlignment,allocate,protect,release);
    assert(result.stage==Stage::ready && result.address==pointer && !result.error);
    assert(allocations==1 && protections==1 && releases==0);
    result=acquire(total,kSwitch2HeapAlignment,allocate,[](void*,std::size_t)->std::uint32_t {return 0x1234;},release);
    assert(result.stage==Stage::protection_failed && !result.address && result.error==0x1234 && releases==1);
    result=acquire(total,kSwitch2HeapAlignment,[](auto,auto)->void* {return nullptr;},protect,release);
    assert(result.stage==Stage::allocation_failed && !result.address && protections==1 && releases==1);
    result=acquire(total,3,allocate,protect,release);
    assert(result.stage==Stage::invalid && allocations==2);
    result=acquire(total,0x200000,allocate,protect,release);
    assert(result.stage==Stage::allocation_failed && !result.address && releases==1);
    std::puts("owned guest window: guard/bounds/failure ownership checks passed");
}
