#include "shims/com/proxy_lookup_cache.h"
#include <cassert>
#include <map>
#include <random>
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
}
