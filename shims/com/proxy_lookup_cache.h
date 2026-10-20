#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

// Per-thread cache only. The owner publishes a new generation when a proxy is
// retired, and fills a miss while holding the same mutex as its object registry.
// This cache neither owns objects nor changes COM reference-counting rules.
template <typename Pointer, std::size_t Slots = 64>
class ProxyLookupCache {
    static_assert(Slots && !(Slots & (Slots - 1)));
    struct Entry { uint32_t guest = 0; uint64_t generation = 0; Pointer host = nullptr; };
    std::array<Entry, Slots> entries{};
    static std::size_t index(uint32_t guest) { return ((guest >> 4) * 0x9E3779B1u) & (Slots - 1); }
public:
    Pointer find(uint32_t guest, uint64_t generation) const {
        const auto& e = entries[index(guest)];
        return guest && e.guest == guest && e.generation == generation ? e.host : nullptr;
    }
    void put(uint32_t guest, uint64_t generation, Pointer host) {
        entries[index(guest)] = {guest, generation, host};
    }
};
