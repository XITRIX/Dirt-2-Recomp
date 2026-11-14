#include "shims/com/proxy_lookup_cache.h"
#include <cassert>
#include <map>
#include <random>
#include <cstdio>
int main() {
    ProxyLookupCache<void*, 8> cache;
    int a = 1, b = 2;
    assert(!cache.find(0, 1));
    cache.put(0x1000, 1, &a);
    assert(cache.find(0x1000, 1) == &a);
    // Retirement invalidates every thread's previous generation. Reusing the
    // native address for a different proxy must not resurrect the old proxy.
    assert(!cache.find(0x1000, 2));
    cache.put(0x1010, 2, &a);
    assert(!cache.find(0x1000, 2));
    cache.put(0x1000, 2, &b);
    assert(cache.find(0x1000, 2) == &b);
    // Force collisions, removals and re-creations against an authoritative
    // registry, including stable references across unrelated insertions.
    std::map<uint32_t, void*> registry;
    uint64_t generation = 3;
    std::mt19937 random(7);
    for (unsigned i = 0; i < 100000; ++i) {
        const uint32_t key = 0x1000 + 16 * (random() % 32);
        switch (random() % 4) {
        case 0: registry.erase(key); ++generation; break;
        case 1: registry[key] = random() % 2 ? &a : &b; ++generation; break;
        default: {
            const auto it = registry.find(key);
            void* expected = it == registry.end() ? nullptr : it->second;
            void* found = cache.find(key, generation);
            if (found) assert(found == expected);
            else if (expected) cache.put(key, generation, expected);
            break;
        }
        }
    }
    // Allocator-aligned live proxies can share all six low index bits. A
    // cyclic working set must stay hot rather than repeatedly lock/hash the
    // authoritative registry. Compare with the actual former indexing rule.
    for (uint32_t stride : {32u, 64u, 128u, 1024u, 4096u}) {
        ProxyLookupCache<void*> aligned;
        struct OldEntry { uint32_t guest = 0; void* host = nullptr; };
        OldEntry old[64]{};
        uint64_t old_hits = 0;
        for (unsigned i = 0; i < 10000; ++i) {
            const uint32_t key = 0x20000010u + (i % 16) * stride;
            if (auto* found = aligned.find(key, 9)) assert(found == &a);
            else aligned.put(key, 9, &a);
            auto& e = old[((key >> 4) * 0x9E3779B1u) & 63];
            if (e.guest == key && e.host) ++old_hits;
            else e = {key, &a};
        }
        const auto stats = aligned.take_statistics();
        assert(stats[0] + stats[1] == 10000);
        assert(stats[0] >= 9900);
        assert(aligned.take_statistics()[0] == 0);
        std::printf("stride%u: old hits%llu, new hits%llu/10000\n", stride,
                    (unsigned long long)old_hits, (unsigned long long)stats[0]);
    }

}
