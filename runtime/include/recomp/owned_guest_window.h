#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::owned_window {
// libounce 20261113's mimalloc arena accepts alignments up to 64 KiB.
// A larger alignment bypasses the premanaged heap and requests new backing
// from the small reserve outside that arena. Guest page permissions only
// require page alignment, so preserve arena eligibility for the 4 GiB span.
inline constexpr std::size_t kSwitch2HeapAlignment=0x10000;
enum class Stage { invalid, allocation_failed, protection_failed, ready };
struct Result { void* address=nullptr; std::uint32_t error=0; Stage stage=Stage::invalid; };

// The allocator owns this complete span. A failed initial permission change
// returns the allocation before it can be published as guest memory.
template<class Allocate,class Protect,class Release>
Result acquire(std::size_t bytes,std::size_t alignment,Allocate allocate,Protect protect,Release release) {
    if(!bytes || !alignment || (alignment&(alignment-1)) || bytes%alignment) return {};
    void* address=allocate(alignment,bytes);
    if(!address) return {nullptr,0,Stage::allocation_failed};
    if(reinterpret_cast<std::uintptr_t>(address)%alignment) {
        release(address); return {};
    }
    const std::uint32_t error=protect(address,bytes);
    if(error) { release(address); return {nullptr,error,Stage::protection_failed}; }
    return {address,0,Stage::ready};
}

// Subtraction-based bounds avoid wrapping at the 4 GiB edge. The low guest
// guard is never made accessible, including a commit rounded to a large run.
inline bool can_commit(std::uint64_t offset,std::uint64_t bytes,std::uint64_t total,
                       std::uint64_t guard,std::uint64_t page) {
    return page && !(page&(page-1)) && bytes && offset>=guard && offset%page==0 &&
        bytes%page==0 && offset<total && bytes<=total-offset;
}
}
