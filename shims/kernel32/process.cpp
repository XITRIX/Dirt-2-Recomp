// kernel32: process, modules, environment, system information, time.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

#include "../shim.h"
#include "../vfs.h"

using namespace shim;

namespace {

const std::string kExePath = std::string(vfs::kInstallDir) + "\\dirt2_game.exe";
const std::string kCmdLine = "\"" + kExePath + "\"";

// Module handles for DLLs we provide: base addresses that never hold memory.
struct Module {
    const char* name;
    uint32_t handle;
};
const Module kModules[] = {
    {"kernel32.dll", 0x7C800000}, {"user32.dll", 0x7E410000}, {"gdi32.dll", 0x77F10000},
    {"advapi32.dll", 0x77DD0000}, {"ntdll.dll", 0x7C900000}, {"ole32.dll", 0x774E0000},
    {"oleaut32.dll", 0x77120000}, {"shell32.dll", 0x7C9C0000}, {"shlwapi.dll", 0x77F60000},
    {"version.dll", 0x77C00000}, {"winmm.dll", 0x76B40000}, {"ws2_32.dll", 0x71AB0000},
    {"d3d9.dll", 0x4FDD0000}, {"d3dx9_42.dll", 0x3FE00000}, {"d3dcompiler_42.dll", 0x3F700000},
    {"xinput1_3.dll", 0x6B000000}, {"dinput8.dll", 0x6F000000}, {"dsound.dll", 0x73F10000},
    {"openal32.dll", 0x6E000000}, {"binkw32.dll", 0x6D000000}, {"xlive.dll", 0x6C000000},
    {"ddraw.dll", 0x73760000}, {"imagehlp.dll", 0x76C90000}, {"crypt32.dll", 0x77AA0000},
    {"wintrust.dll", 0x76C30000}, {"dbghelp.dll", 0x59A60000}, {"psapi.dll", 0x76BF0000},
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
}

std::string module_key(const char* name) {
    std::string n = lower(name ? name : "");
    if (const size_t s = n.find_last_of("\\/"); s != std::string::npos) n = n.substr(s + 1);
    if (n.find('.') == std::string::npos) n += ".dll";
    return n;
}

const Module* find_module(const std::string& key) {
    for (const Module& m : kModules)
        if (key == m.name) return &m;
    return nullptr;
}

const Module* module_by_handle(uint32_t h) {
    for (const Module& m : kModules)
        if (m.handle == h) return &m;
    return nullptr;
}

uint64_t now_filetime() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return uint64_t(ns / 100) + 116444736000000000ull;
}

void write_systemtime(uint32_t d, const std::tm& t, int ms) {
    wr16(d + 0, uint16_t(t.tm_year + 1900));
    wr16(d + 2, uint16_t(t.tm_mon + 1));
    wr16(d + 4, uint16_t(t.tm_wday));
    wr16(d + 6, uint16_t(t.tm_mday));
    wr16(d + 8, uint16_t(t.tm_hour));
    wr16(d + 10, uint16_t(t.tm_min));
    wr16(d + 12, uint16_t(t.tm_sec));
    wr16(d + 14, uint16_t(ms));
}

void systemtime_now(uint32_t d, bool local) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm t{};
    if (local) localtime_r(&tt, &t);
    else gmtime_r(&tt, &t);
    write_systemtime(d, t, ms);
}

uint32_t g_env_a = 0, g_env_w = 0;
uint32_t g_cmd_a = 0, g_cmd_w = 0;
uint32_t g_exception_filter = 0;

uint32_t processor_count() {
    // The game picks workerMap{2,4,8}Core.xml from this; cap at 8.
    if (const char* e = std::getenv("DIRT2_CPUS")) return std::clamp<uint32_t>(uint32_t(std::atoi(e)), 1, 8);
    const unsigned n = std::thread::hardware_concurrency();
    return std::clamp<uint32_t>(n ? n : 4, 1, 8);
}

}  // namespace

// ---- process and modules ----
STDCALL(kernel32, GetCommandLineA, 0) {
    if (!g_cmd_a) g_cmd_a = rt::guest_strdup(kCmdLine);
    return g_cmd_a;
}
STDCALL(kernel32, GetCommandLineW, 0) {
    if (!g_cmd_w) {
        g_cmd_w = rt::heap_alloc(uint32_t(kCmdLine.size() * 2 + 2));
        copy_out_w(g_cmd_w, uint32_t(kCmdLine.size() + 1), kCmdLine);
    }
    return g_cmd_w;
}
STDCALL(kernel32, GetModuleHandleA, 1) {
    if (!a[0]) return recomp_image_info.image_base;
    const std::string key = module_key(a.str(0));
    if (key == "dirt2_game.exe") return recomp_image_info.image_base;
    if (const Module* m = find_module(key)) return m->handle;
    set_last_error(ERROR_MOD_NOT_FOUND);
    return 0;
}
STDCALL(kernel32, GetModuleFileNameA, 3) {
    if (a[0] && a[0] != recomp_image_info.image_base) {
        if (const Module* m = module_by_handle(a[0])) return copy_out(a[1], a[2], std::string("C:\\Windows\\system32\\") + m->name);
        return 0;
    }
    return copy_out(a[1], a[2], kExePath);
}
STDCALL(kernel32, GetModuleFileNameW, 3) { return copy_out_w(a[1], a[2], kExePath); }
STDCALL(kernel32, LoadLibraryA, 1) {
    const std::string key = module_key(a.str(0));
    // No D3D10/11 path: refusing these makes the game select its D3D9 renderer.
    if (key == "d3d11.dll" || key == "dxgi.dll" || key == "d3d10.dll" || key == "d3d10_1.dll") {
        rt::log("LoadLibraryA(%s): refused (D3D9 only)", a.str(0));
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (const Module* m = find_module(key)) return m->handle;
    rt::log("LoadLibraryA(%s): not available", a.str(0));
    set_last_error(ERROR_MOD_NOT_FOUND);
    return 0;
}
STDCALL(kernel32, FreeLibrary, 1) { return 1; }
STDCALL(kernel32, GetProcAddress, 2) {
    const Module* m = module_by_handle(a[0]);
    const bool by_ordinal = a[1] < 0x10000;
    const char* name = by_ordinal ? nullptr : a.str(1);
    uint32_t r = 0;
    if (m) r = resolve_export(m->name, name, uint16_t(a[1]));
    if (!r) {
        rt::log("GetProcAddress(%s, %s): not found", m ? m->name : "?", name ? name : "#ordinal");
        set_last_error(ERROR_PROC_NOT_FOUND);
    }
    return r;
}
STDCALL(kernel32, GetCurrentProcess, 0) { return kCurrentProcess; }
STDCALL(kernel32, GetCurrentProcessId, 0) { return 0x10; }
STDCALL(kernel32, OpenProcess, 3) { return a[2] == 0x10 ? kCurrentProcess : 0; }  // only our own process
STDCALL(kernel32, GetExitCodeProcess, 2) { return 0; }
STDCALL(kernel32, CreateProcessA, 10) {
    rt::log("CreateProcessA(%s, %s) not supported", a.str(0) ? a.str(0) : "", a.str(1) ? a.str(1) : "");
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
STDCALL(kernel32, CreateProcessW, 10) {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
STDCALL(kernel32, ExitProcess, 1) {
    rt::log("ExitProcess(%u)", a[0]);
    std::fflush(nullptr);
    std::_Exit(int(a[0]));
}
STDCALL(kernel32, TerminateProcess, 2) {
    rt::log("TerminateProcess(%u)", a[1]);
    std::fflush(nullptr);
    std::_Exit(int(a[1]));
}
STDCALL(kernel32, FatalAppExitA, 2) { rt::fatal("FatalAppExitA: %s", a.str(1)); }
STDCALL(kernel32, GetStartupInfoA, 1) {
    std::memset(rt::host_ptr(a[0]), 0, 68);
    wr32(a[0], 68);
    return 0;
}
STDCALL(kernel32, GetProcessAffinityMask, 3) {
    const uint32_t mask = (1u << processor_count()) - 1;
    wr32(a[1], mask);
    wr32(a[2], mask);
    return 1;
}
STDCALL(kernel32, SetProcessAffinityMask, 2) { return 1; }
STDCALL(kernel32, SetThreadAffinityMask, 2) { return 1; }
STDCALL(kernel32, SetThreadIdealProcessor, 2) { return 0; }
STDCALL(kernel32, SetThreadPriority, 2) { return 1; }
STDCALL(kernel32, GetThreadPriority, 1) { return 0; }
STDCALL(kernel32, SetThreadExecutionState, 1) { return 0x80000000; }

// ---- environment ----
STDCALL(kernel32, GetEnvironmentStrings, 0) {
    if (!g_env_a) g_env_a = rt::heap_alloc(16, true);
    return g_env_a;
}
STDCALL(kernel32, GetEnvironmentStringsW, 0) {
    if (!g_env_w) g_env_w = rt::heap_alloc(16, true);
    return g_env_w;
}
STDCALL(kernel32, FreeEnvironmentStringsA, 1) { return 1; }
STDCALL(kernel32, FreeEnvironmentStringsW, 1) { return 1; }
STDCALL(kernel32, GetEnvironmentVariableA, 3) {
    set_last_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
STDCALL(kernel32, GetEnvironmentVariableW, 3) {
    set_last_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
STDCALL(kernel32, SetEnvironmentVariableA, 2) { return 1; }
STDCALL(kernel32, ExpandEnvironmentStringsA, 3) {
    const std::string s = a.str(0);
    if (a[2] > s.size()) copy_out(a[1], a[2], s);
    return uint32_t(s.size() + 1);
}
STDCALL(kernel32, GetComputerNameA, 2) {
    const uint32_t n = copy_out(a[0], rd32(a[1]), "DIRT2PC");
    wr32(a[1], n);
    return 1;
}
STDCALL(kernel32, GetComputerNameExA, 3) {
    const uint32_t n = copy_out(a[1], rd32(a[2]), "DIRT2PC");
    wr32(a[2], n);
    return 1;
}

// ---- system information ----
STDCALL(kernel32, GetVersion, 0) { return (7601u << 16) | (1u << 8) | 6u; }  // Windows 7 SP1
STDCALL(kernel32, GetVersionExA, 1) {
    const uint32_t v = a[0];
    const uint32_t size = rd32(v);
    std::memset(rt::host_ptr(v + 4), 0, size > 4 ? size - 4 : 0);
    wr32(v + 4, 6);
    wr32(v + 8, 1);
    wr32(v + 12, 7601);
    wr32(v + 16, 2);  // VER_PLATFORM_WIN32_NT
    copy_out(v + 20, 128, "Service Pack 1");
    if (size >= 156) {
        wr16(v + 148, 1);  // wServicePackMajor
        wr8(v + 154, 1);   // wProductType = VER_NT_WORKSTATION
    }
    return 1;
}
static void fill_system_info(uint32_t d) {
    std::memset(rt::host_ptr(d), 0, 36);
    wr32(d + 4, rt::kPage);
    wr32(d + 8, 0x10000);
    wr32(d + 12, 0x7FFEFFFF);
    wr32(d + 16, (1u << processor_count()) - 1);
    wr32(d + 20, processor_count());
    wr32(d + 24, 586);
    wr32(d + 28, rt::kGranularity);
    wr16(d + 32, 6);
}
STDCALL(kernel32, GetSystemInfo, 1) {
    fill_system_info(a[0]);
    return 0;
}
STDCALL(kernel32, GetNativeSystemInfo, 1) {
    fill_system_info(a[0]);
    return 0;
}
STDCALL(kernel32, IsProcessorFeaturePresent, 1) {
    switch (a[0]) {
        case 0:   // PF_FLOATING_POINT_PRECISION_ERRATA
        case 1:   // PF_FLOATING_POINT_EMULATED
            return 0;
        case 3:   // PF_MMX_INSTRUCTIONS_AVAILABLE
        case 6:   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
        case 8:   // PF_RDTSC_INSTRUCTION_AVAILABLE
        case 10:  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
            return 1;
        default: return 0;
    }
}
STDCALL(kernel32, IsDebuggerPresent, 0) { return 0; }
STDCALL(kernel32, DebugBreak, 0) {
    rt::log("DebugBreak ignored (caller 0x%08X)", rd32(c->esp));
    return 0;
}
STDCALL(kernel32, OutputDebugStringA, 1) {
    std::string s = a.str(0) ? a.str(0) : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    rt::log("ODS: %s", s.c_str());
    return 0;
}
STDCALL(kernel32, FormatMessageA, 7) {
    const std::string s = "Error " + std::to_string(a[2]);
    if (a[0] & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
        wr32(a[4], rt::guest_strdup(s));
        return uint32_t(s.size());
    }
    return copy_out(a[4], a[5], s);
}

// ---- errors and exceptions ----
STDCALL(kernel32, GetLastError, 0) { return last_error(); }
STDCALL(kernel32, SetLastError, 1) {
    set_last_error(a[0]);
    return 0;
}
STDCALL(kernel32, SetUnhandledExceptionFilter, 1) {
    const uint32_t prev = g_exception_filter;
    g_exception_filter = a[0];
    return prev;
}
STDCALL(kernel32, UnhandledExceptionFilter, 1) { return 1; }  // EXCEPTION_EXECUTE_HANDLER

// ---- time ----
STDCALL(kernel32, GetTickCount, 0) {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
STDCALL(kernel32, QueryPerformanceFrequency, 1) {
    wr64(a[0], 10000000);
    return 1;
}
STDCALL(kernel32, QueryPerformanceCounter, 1) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    wr64(a[0], uint64_t(ns / 100));
    return 1;
}
STDCALL(kernel32, GetSystemTimeAsFileTime, 1) {
    wr64(a[0], now_filetime());
    return 0;
}
STDCALL(kernel32, GetSystemTime, 1) {
    systemtime_now(a[0], false);
    return 0;
}
STDCALL(kernel32, GetLocalTime, 1) {
    systemtime_now(a[0], true);
    return 0;
}
STDCALL(kernel32, FileTimeToLocalFileTime, 2) {
    wr64(a[1], rd64(a[0]));
    return 1;
}
STDCALL(kernel32, FileTimeToSystemTime, 2) {
    const uint64_t ft = rd64(a[0]);
    const std::time_t tt = std::time_t(ft / 10000000ull - 11644473600ull);
    std::tm t{};
    gmtime_r(&tt, &t);
    write_systemtime(a[1], t, int((ft / 10000) % 1000));
    return 1;
}
STDCALL(kernel32, GetTimeZoneInformation, 1) {
    std::memset(rt::host_ptr(a[0]), 0, 172);
    return 0;  // TIME_ZONE_ID_UNKNOWN
}

// ---- structured exceptions ----
STDCALL(kernel32, RaiseException, 4) {
    if (a[0] == 0x406D1388) return 0;  // MSVC SetThreadName: only meaningful to debuggers
    rt::raise_exception(c, a[0], a[1], a[2], a[3]);
    return c->eax;
}
STDCALL(kernel32, RtlUnwind, 4) {
    rt::rtl_unwind(c, a[0], a[2], a[3]);
    return a[3];
}
