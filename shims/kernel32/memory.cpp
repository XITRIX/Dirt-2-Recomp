// kernel32: heaps and virtual memory.
#include "../shim.h"

using namespace shim;

namespace {
constexpr uint32_t kProcessHeap = 0x00150000;  // any non-zero value; all heaps share the runtime heap
constexpr uint32_t HEAP_ZERO_MEMORY = 0x8;
constexpr uint32_t MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000;
constexpr uint32_t MEM_FREE = 0x10000, MEM_PRIVATE = 0x20000, MEM_IMAGE = 0x1000000;
constexpr uint32_t PAGE_READWRITE = 0x04, PAGE_EXECUTE_READWRITE = 0x40, PAGE_NOACCESS = 0x01;
}  // namespace

STDCALL(kernel32, GetProcessHeap, 0) { return kProcessHeap; }
STDCALL(kernel32, HeapCreate, 3) { return kProcessHeap + 0x10000; }
STDCALL(kernel32, HeapDestroy, 1) { return 1; }
STDCALL(kernel32, HeapAlloc, 3) {
    const uint32_t p = rt::heap_alloc(a[2], (a[1] & HEAP_ZERO_MEMORY) != 0);
    if (!p) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
STDCALL(kernel32, HeapFree, 3) {
    rt::heap_free(a[2]);
    return 1;
}
STDCALL(kernel32, HeapReAlloc, 4) {
    const uint32_t old = rt::heap_size(a[2]);
    const uint32_t p = rt::heap_realloc(a[2], a[3]);
    if (p && (a[1] & HEAP_ZERO_MEMORY) && a[3] > old) std::memset(rt::host_ptr(p + old), 0, a[3] - old);
    return p;
}
STDCALL(kernel32, HeapSize, 3) { return a[2] ? rt::heap_size(a[2]) : 0xFFFFFFFF; }
STDCALL(kernel32, LocalFree, 1) {
    rt::heap_free(a[0]);
    return 0;
}

STDCALL(kernel32, VirtualAlloc, 4) {
    const uint32_t addr = a[0], size = a[1], type = a[2];
    const uint32_t r = rt::vm_alloc(addr, size, (type & MEM_COMMIT) != 0);
    if (!r) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        rt::log("VirtualAlloc(0x%08X, 0x%X, 0x%X) failed", addr, size, type);
    }
    return r;
}
STDCALL(kernel32, VirtualFree, 3) {
    const bool release = (a[2] & MEM_RELEASE) != 0;
    return rt::vm_free(a[0], release ? 0 : a[1], release) ? 1 : 0;
}
STDCALL(kernel32, VirtualProtect, 4) {
    if (a[3]) wr32(a[3], PAGE_EXECUTE_READWRITE);
    return 1;
}
STDCALL(kernel32, VirtualQuery, 3) {
    const rt::VmInfo i = rt::vm_query(a[0]);
    const uint32_t m = a[1];
    if (!m || a[2] < 28) return 0;
    const bool image = i.alloc_base == recomp_image_info.image_base;
    wr32(m + 0, i.base);
    wr32(m + 4, i.free ? 0 : i.alloc_base);
    wr32(m + 8, i.free ? 0 : (image ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE));
    wr32(m + 12, i.size);
    wr32(m + 16, i.free ? MEM_FREE : (i.committed ? MEM_COMMIT : MEM_RESERVE));
    wr32(m + 20, i.free ? PAGE_NOACCESS : (image ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE));
    wr32(m + 24, i.free ? 0 : (image ? MEM_IMAGE : MEM_PRIVATE));
    return 28;
}
STDCALL(kernel32, GlobalMemoryStatus, 1) {
    // Report a 2 GB machine with most memory free: what the game expects of a 32-bit process.
    const uint32_t m = a[0];
    wr32(m + 0, 32);
    wr32(m + 4, 25);             // dwMemoryLoad %
    wr32(m + 8, 0x7FFF0000);     // dwTotalPhys
    wr32(m + 12, 0x60000000);    // dwAvailPhys
    wr32(m + 16, 0xFFFF0000);    // dwTotalPageFile
    wr32(m + 20, 0xC0000000);    // dwAvailPageFile
    wr32(m + 24, 0x7FFE0000);    // dwTotalVirtual
    wr32(m + 28, 0x70000000);    // dwAvailVirtual
    return 0;
}

// File mappings. The only one the game opens is the SecuROM launcher block
// "-=[SMS_dirt2_game.exe_SMS]=-" (100 bytes: launcher window handle and a
// message id). Providing it zero-filled stands in for running DiRT2.exe.
namespace {
struct Mapping : KObject {
    Mapping() : KObject(Kind::Other) {}
    uint32_t base = 0, size = 0;
};
}  // namespace

STDCALL(kernel32, OpenFileMappingW, 3) {
    const std::string name = from_utf16(a[2]);
    if (name.rfind("-=[SMS_", 0) == 0) {
        auto m = std::make_shared<Mapping>();
        m->size = 0x1000;
        m->base = rt::vm_alloc(0, m->size, true);
        rt::log("OpenFileMappingW(%s): emulating launcher handshake", name.c_str());
        return handle_new(m);
    }
    set_last_error(ERROR_FILE_NOT_FOUND);
    return 0;
}
STDCALL(kernel32, MapViewOfFile, 5) {
    auto m = handle_as<Mapping>(a[0]);
    return m ? m->base : 0;
}
STDCALL(kernel32, UnmapViewOfFile, 1) { return 1; }
