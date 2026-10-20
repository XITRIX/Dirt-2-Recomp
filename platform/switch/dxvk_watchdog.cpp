// Startup-only diagnostic. Keep its thread/stack alive until process exit:
// this Horizon newlib does not implement pthread_detach.
#include <switch.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include "recomp/host.h"
namespace {
std::atomic<bool> first_present{false};
std::atomic<uint64_t> last_progress_ns{0};
uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
Thread watchdog;
alignas(0x1000) unsigned char watchdog_stack[0x10000];
void watch_startup(void*) {
    while (!first_present.load()) {
        svcSleepThread(100000000);
        const uint64_t last = last_progress_ns.load();
        const uint64_t now = now_ns();
        if (now > last && now - last > 120000000000ull) {
            // Never take logging/filesystem locks potentially held by a stuck thread.
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_Timeout));
        }
    }
}
}
extern "C" void dirt2_dx11_present_succeeded() { first_present = true; }
extern "C" void dirt2_dx11_startup_progress() { last_progress_ns = now_ns(); }
namespace switch_platform {
void start_dx11_startup_watchdog() {
    const char* setting = std::getenv("DIRT2_DX11_STARTUP_WATCHDOG");
    if (!setting || setting[0] != '1') return;
    dirt2_dx11_startup_progress();
    Result rc = threadCreate(&watchdog, watch_startup, nullptr, watchdog_stack,
                             sizeof(watchdog_stack), 0x2c, -2);
    if (R_FAILED(rc)) {
        rt::log("DX11 startup watchdog: threadCreate failed %08X; disabled", unsigned(rc));
        return;
    }
    rc = threadStart(&watchdog);
    if (R_FAILED(rc)) {
        threadClose(&watchdog);
        rt::log("DX11 startup watchdog: threadStart failed %08X; disabled", unsigned(rc));
        return;
    }
    rt::log("DX11 startup watchdog: 120s without progress before first game Present; timeout requests a crash report");
}
}
