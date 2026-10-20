// Standalone graphics probe: deliberately no guest runtime, bridge or FFmpeg.
#include <switch.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "recomp/host.h"

namespace rt {
void log(const char* format, ...) {
    static const auto start = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    char message[2048];
    va_list args; va_start(args, format); std::vsnprintf(message, sizeof message, format, args); va_end(args);
    std::fprintf(stderr, "[probe %9.3f] %s\n", elapsed, message);
}
}
namespace switch_platform { bool check_c11_abis(); int run_dx11_preflight(); }
namespace {
std::atomic<bool> allocation_logs_ready{false};
std::atomic<unsigned> nv_references{0};
std::atomic<uintptr_t> gpu_transfer_base{0};
std::atomic<size_t> gpu_transfer_size{0};
}
extern "C" Result __real_nvInitialize();
extern "C" void __real_nvExit();
extern "C" Result __wrap_nvInitialize() {
    const Result rc = __real_nvInitialize();
    if (R_SUCCEEDED(rc)) ++nv_references;
    return rc;
}
extern "C" void __wrap_nvExit() {
    __real_nvExit();
    if (nv_references.load()) --nv_references;
}
extern "C" void nouveau_horizon_runtime_shutdown(void (*step)(const char*));
extern "C" void dirt2_probe_final_cleanup() {
    rt::log("NVK shutdown: %u outstanding nvInitialize references", nv_references.load());
    // All test COM objects have been released and their workers joined. Close
    // any process-lifetime backend cache before the homebrew loader reuses RAM.
    nouveau_horizon_runtime_shutdown([](const char* step) { rt::log("NVK shutdown: %s", step); });
    // The backend shutdown intentionally leaves the base session for process
    // death. NRO return is a chainload in the SAME process, so close ours here.
    while (nv_references.load()) __wrap_nvExit();
    const uintptr_t base = gpu_transfer_base.load();
    const size_t length = gpu_transfer_size.load();
    const auto wait_start = std::chrono::steady_clock::now();
    bool logged_wait = false;
    for (;;) {
        bool recovered = true;
        for (uintptr_t at = base; base && at < base + length;) {
            MemoryInfo memory{}; u32 page = 0;
            const Result rc = svcQueryMemory(&memory, &page, at);
            if (R_FAILED(rc) || memory.perm != Perm_Rw || memory.attr != 0 || memory.addr + memory.size <= at) {
                recovered = false;
                if (!logged_wait) {
                    rt::log("NVK shutdown: waiting for transfer region %llX+%llX perm %u attr %u, query %08X",
                        (unsigned long long)memory.addr, (unsigned long long)memory.size,
                        unsigned(memory.perm), unsigned(memory.attr), unsigned(rc));
                    logged_wait = true;
                }
                break;
            }
            at = memory.addr + memory.size;
        }
        if (recovered) break;
        // Closing a service session can return before its server releases the
        // last transfer-memory reference. Do not return to the loader yet.
        if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(10)) {
            rt::log("NVK shutdown: refusing unsafe launcher return; transfer pages still protected after 10s");
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxEventWait));
        }
        svcSleepThread(1000000);
    }
    rt::log("NVK shutdown: transfer-memory recovery PASS");
}
extern "C" void* __real___libnx_aligned_alloc(size_t, size_t);
extern "C" void* __wrap___libnx_aligned_alloc(size_t alignment, size_t size) {
    void* pointer = __real___libnx_aligned_alloc(alignment, size);
    if (allocation_logs_ready && size >= 0x1000) {
        MemoryInfo memory{}; u32 page = 0;
        const Result rc = pointer ? svcQueryMemory(&memory, &page, reinterpret_cast<u64>(pointer)) : 0;
        rt::log("Aligned allocation: %zu bytes align %zu -> %p, query %08X, region %llX+%llX perm %u type %u attr %u",
            size, alignment, pointer, unsigned(rc), (unsigned long long)memory.addr,
            (unsigned long long)memory.size, unsigned(memory.perm), unsigned(memory.type), unsigned(memory.attr));
    }
    return pointer;
}
int main(int, char**);
extern "C" Result __real_tmemCreate(TransferMemory*, size_t, Permission);
extern "C" Result __wrap_tmemCreate(TransferMemory* t, size_t size, Permission perm) {
    const auto caller = reinterpret_cast<intptr_t>(__builtin_return_address(0));
    const auto entry = reinterpret_cast<intptr_t>(&main);
    rt::log("Transfer memory: creating %zu bytes, permission %u, caller main%+lld", size, unsigned(perm), (long long)(caller - entry));
    const Result rc = __real_tmemCreate(t, size, perm);
    rt::log("Transfer memory: result %08X, address %p, size %zu", unsigned(rc), t->src_addr, t->size);
    if (R_SUCCEEDED(rc) && size == 8u*1024u*1024u) {
        gpu_transfer_base = reinterpret_cast<uintptr_t>(t->src_addr);
        gpu_transfer_size = size;
    }
    return rc;
}
int main(int, char**) {
    constexpr const char* directory = "sdmc:/switch/dirt2-dx11";
    std::filesystem::create_directories(directory);
    std::rename("sdmc:/switch/dirt2-dx11/dirt2.log", "sdmc:/switch/dirt2-dx11/dirt2.prev.log");
    std::freopen("sdmc:/switch/dirt2-dx11/dirt2.log", "w", stderr);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    allocation_logs_ready = true;
    rt::log("Standalone DX11 probe: no guest runtime/FFmpeg; normal libnx exception handling");
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);
    setenv("DXVK_LOG_LEVEL", "info", 1);
    setenv("DXVK_LOG_PATH", directory, 1);
    setenv("DXVK_SHADER_CACHE_PATH", directory, 1);
    setenv("DXVK_SHADER_CACHE_NAME", "preflight", 1);
    setenv("DXVK_PIPELINE_WARMUP", "", 1);
    setenv("DXVK_PIPELINE_RECORD", "", 1);
    setenv("DXVK_HUD", "", 1);
    setenv("MESA_SHADER_CACHE_DIR", "sdmc:/switch/dirt2-dx11/mesa-cache", 1);
    if (!switch_platform::check_c11_abis()) return 1;
    SDL_SetHint("SDL_SWITCH_NATIVE_WINDOW", "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        rt::log("SDL init failed: %s", SDL_GetError()); return 1;
    }
    return switch_platform::run_dx11_preflight();
}
