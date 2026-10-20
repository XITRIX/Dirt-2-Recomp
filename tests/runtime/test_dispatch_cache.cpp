// Links the real dispatcher; no substitute cache implementation.
#include "recomp/host.h"
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
extern "C" {
uint8_t* recomp_mem = nullptr;
const RecompFnEntry recomp_fn_table[] = {{0, nullptr}};
const uint32_t recomp_fn_count = 0;
void recomp_resume_at(uint32_t) {}
}
namespace rt {
void log(const char*, ...) {}
[[noreturn]] void fatal(const char*, ...) { std::abort(); }
}
void first(X86*) {}
void second(X86*) {}
int main() {
    // Collide in both the former shared65536-slot and new4096-slot hashes.
    const uint32_t a = 0x1000;
    const auto bucket = [](uint32_t k) { return (k * 0x9E3779B1u) >> 16; };
    uint32_t b = a + 1;
    while (bucket(b) != bucket(a)) ++b;
    rt::register_thunk(a, first); rt::register_thunk(b, second);
    std::atomic<unsigned> wrong{0};
    std::barrier ready(8);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) threads.emplace_back([&, t] {
        ready.arrive_and_wait();
        for (unsigned i = 0; i < 500000; ++i) {
            const bool which = (i + t) & 1;
            if (rt::lookup(which ? a : b) != (which ? first : second)) ++wrong;
        }
    });
    for (auto& t : threads) t.join();
    if (wrong) std::fprintf(stderr,"Wrong function lookups: %u\n",wrong.load());
    assert(wrong == 0);
    // A different thread must retire an already hot mapping after registration.
    std::barrier sync(2);
    std::thread reader([&] {
        assert(rt::lookup(a) == first);
        sync.arrive_and_wait(); sync.arrive_and_wait();
        assert(rt::lookup(a) == second);
        assert(rt::lookup(b) == second);
        assert(rt::lookup(0x12345678) == nullptr);
    });
    sync.arrive_and_wait(); rt::register_thunk(a, second); sync.arrive_and_wait();
    reader.join();
    std::puts("PASS: 4 million colliding lookups and cross-thread invalidation");
}
