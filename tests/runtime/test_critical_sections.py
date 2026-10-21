"""Exercise the production critical-section registry without a game runtime."""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).read_text()
registry = source.split("// ---- critical sections: host mutex index", 1)[1]
registry = registry[registry.index("struct HostCS"):].split("// ---- TLS slots:", 1)[0]
harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
static uint32_t memory[1024]{};
namespace rt {
void* host_ptr(uint32_t p) { return reinterpret_cast<char*>(memory) + p; }
template<class... T> void log(const char*, T...) {}
template<class... T> [[noreturn]] void fatal(const char*, T...) { std::abort(); }
}
uint32_t rd32(uint32_t p) { return *static_cast<uint32_t*>(rt::host_ptr(p)); }
void wr32(uint32_t p, uint32_t v) { *static_cast<uint32_t*>(rt::host_ptr(p)) = v; }
''' + registry + r'''
int main() {
    constexpr uint32_t a = 64, b = 128;
    cs_init(a);
    const auto slot = rd32(a + 16);
    HostCS& retired = cs_for(a);
    retired.m.lock();
    retired.m.lock();
    // Recreate the captured stale-native-owner condition. No concurrent
    // guest use of the deleted section is required by this regression.
    cs_delete(a);
    cs_init(b);
    assert(rd32(b + 16) == slot); // retain bounded table-index reuse
    HostCS& fresh = cs_for(b);
    assert(&fresh != &retired);
    bool acquired = false;
    std::thread worker([&] {
        acquired = fresh.m.try_lock();
        if (acquired) fresh.m.unlock();
    });
    worker.join();
    assert(acquired); // a fresh lock cannot inherit another thread's owner
    retired.m.unlock();
    retired.m.unlock(); // old references remain valid
    for (int i = 0; i < 1000; ++i) {
        cs_delete(b);
        cs_init(b);
        assert(rd32(b + 16) == slot);
        assert(cs_for(b).m.try_lock());
        cs_for(b).m.unlock();
    }
    HostCS& previous = cs_for(b);
    previous.m.lock();
    cs_init(b); // guest ownership is reset even when the index stays the same
    assert(rd32(b + 16) == slot);
    assert(&cs_for(b) != &previous);
    acquired = false;
    std::thread reinitialised([&] {
        acquired = cs_for(b).m.try_lock();
        if (acquired) cs_for(b).m.unlock();
    });
    reinitialised.join();
    assert(acquired);
    previous.m.unlock();
    assert(g_cs_count.load() == 1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = pathlib.Path(tmp) / "critical_sections.cpp"
    exe = pathlib.Path(tmp) / "critical_sections"
    cpp.write_text(harness)
    sdk = ["-isysroot", sys.argv[3]] if len(sys.argv) > 3 and sys.argv[3] else []
    subprocess.run([sys.argv[2], *sdk, "-std=c++20", "-pthread", "-UNDEBUG", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
print("Critical-section generation regression passed")
