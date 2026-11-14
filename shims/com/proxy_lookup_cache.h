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
    static constexpr std::size_t Ways = Slots < 4 ? Slots : 4;
    static constexpr std::size_t Sets = Slots / Ways;
    struct Entry { uint32_t guest = 0; uint64_t generation = 0; Pointer host = nullptr; };
    std::array<Entry, Slots> entries{};
    std::array<uint8_t, Sets> next{};
    mutable uint64_t hits = 0, misses = 0;
    static std::size_t index(uint32_t guest) {
        // Low-bit masking preserved allocator strides: proxies separated by
        // 1024 bytes all evicted one another in the former64-slot direct map.
        // Use the high product bits and four ways without enlarging the table.
        const uint32_t hash = (guest >> 4) * 0x9E3779B1u;
        return std::size_t((uint64_t(hash) * Sets) >> 32);
    }
public:
    Pointer find(uint32_t guest, uint64_t generation) const {
        const auto first = index(guest) * Ways;
        for (std::size_t i = 0; i < Ways; ++i) {
            const auto& e = entries[first + i];
            if (guest && e.guest == guest && e.generation == generation && e.host) {
                ++hits;
                return e.host;
            }
        }
        ++misses;
        return nullptr;
    }
    void put(uint32_t guest, uint64_t generation, Pointer host) {
        const auto set = index(guest), first = set * Ways;
        for (std::size_t i = 0; i < Ways; ++i)
            if (entries[first + i].guest == guest) {
                entries[first + i] = {guest, generation, host};
                return;
            }
        for (std::size_t i = 0; i < Ways; ++i)
            if (!entries[first + i].host || entries[first + i].generation != generation) {
                entries[first + i] = {guest, generation, host};
                return;
            }
        entries[first + next[set]] = {guest, generation, host};
        next[set] = uint8_t((next[set] + 1) % Ways);
    }
    // Called on the owning thread only; no shared per-call counter contention.
    std::array<uint64_t, 2> take_statistics() const {
        const std::array<uint64_t, 2> result{hits, misses};
        hits = misses = 0;
        return result;
    }
};
