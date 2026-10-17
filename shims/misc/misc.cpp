// advapi32, shell32, shlwapi, version, ole32, oleaut32, imagehlp, crypt32,
// wintrust, winmm: the small DLLs.
#include <sys/stat.h>

#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include "../shim.h"
#include "../vfs.h"

using namespace shim;

namespace {

// ---- registry: in-memory, per run (the game only probes optional keys) ----
struct Key {
    std::string path;
    std::map<std::string, std::vector<uint8_t>> values;
    std::map<std::string, uint32_t> types;
};
std::mutex g_reg_mu;
std::map<std::string, Key> g_keys;          // lower-case full path -> key
std::map<uint32_t, std::string> g_open;     // HKEY -> path
uint32_t g_next_key = 0x1100;

std::string root_name(uint32_t hkey) {
    switch (hkey) {
        case 0x80000000: return "hkcr";
        case 0x80000001: return "hkcu";
        case 0x80000002: return "hklm";
        case 0x80000003: return "hku";
        default: {
            auto it = g_open.find(hkey);
            return it == g_open.end() ? "" : it->second;
        }
    }
}

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

uint32_t open_key(uint32_t parent, const char* sub, uint32_t out, bool create) {
    std::lock_guard lk(g_reg_mu);
    const std::string base = root_name(parent);
    if (base.empty()) return ERROR_INVALID_HANDLE;
    const std::string path = lower(sub && *sub ? base + "\\" + sub : base);
    if (!g_keys.count(path)) {
        if (!create) return ERROR_FILE_NOT_FOUND;
        g_keys[path].path = path;
    }
    const uint32_t h = g_next_key += 4;
    g_open[h] = path;
    if (out) wr32(out, h);
    return ERROR_SUCCESS;
}

constexpr uint32_t CSIDL_COMMON_DOCUMENTS = 0x2E, CSIDL_PERSONAL = 0x05, CSIDL_APPDATA = 0x1A, CSIDL_LOCAL_APPDATA = 0x1C,
                   CSIDL_COMMON_APPDATA = 0x23, CSIDL_PROFILE = 0x28;

std::string folder_path(uint32_t csidl) {
    switch (csidl & 0xFF) {
        case CSIDL_PERSONAL: return "C:\\Users\\Player\\Documents";
        case CSIDL_APPDATA: return "C:\\Users\\Player\\AppData\\Roaming";
        case CSIDL_LOCAL_APPDATA: return "C:\\Users\\Player\\AppData\\Local";
        case CSIDL_COMMON_APPDATA: return "C:\\ProgramData";
        case CSIDL_PROFILE: return "C:\\Users\\Player";
        case CSIDL_COMMON_DOCUMENTS: return "C:\\Users\\Public\\Documents";
        default: return "";
    }
}

void mkdirs(const std::string& host) {
    std::string cur;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '/') {
            if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
        }
        if (i < host.size()) cur += host[i];
    }
}

}  // namespace

// ---- advapi32 ----
STDCALL(advapi32, RegOpenKeyExA, 5) { return open_key(a[0], a.str(1), a[4], false); }
STDCALL(advapi32, RegCreateKeyExA, 9) {
    const uint32_t r = open_key(a[0], a.str(1), a[7], true);
    if (r == ERROR_SUCCESS && a[8]) wr32(a[8], 1);  // REG_CREATED_NEW_KEY
    return r;
}
STDCALL(advapi32, RegCloseKey, 1) {
    std::lock_guard lk(g_reg_mu);
    g_open.erase(a[0]);
    return ERROR_SUCCESS;
}
STDCALL(advapi32, RegQueryValueExA, 6) {
    std::lock_guard lk(g_reg_mu);
    auto it = g_keys.find(root_name(a[0]));
    if (it == g_keys.end()) return ERROR_FILE_NOT_FOUND;
    const std::string name = lower(a.str(1) ? a.str(1) : "");
    auto v = it->second.values.find(name);
    if (v == it->second.values.end()) return ERROR_FILE_NOT_FOUND;
    if (a[3]) wr32(a[3], it->second.types[name]);
    const uint32_t size = uint32_t(v->second.size());
    if (!a[5]) return ERROR_SUCCESS;
    const uint32_t cap = rd32(a[5]);
    wr32(a[5], size);
    if (!a[4]) return ERROR_SUCCESS;
    if (cap < size) return ERROR_MORE_DATA;
    std::memcpy(rt::host_ptr(a[4]), v->second.data(), size);
    return ERROR_SUCCESS;
}
STDCALL(advapi32, RegSetValueExA, 6) {
    std::lock_guard lk(g_reg_mu);
    auto it = g_keys.find(root_name(a[0]));
    if (it == g_keys.end()) return ERROR_INVALID_HANDLE;
    const std::string name = lower(a.str(1) ? a.str(1) : "");
    const uint8_t* p = rt::host_ptr(a[4]);
    it->second.values[name].assign(p, p + a[5]);
    it->second.types[name] = a[3];
    return ERROR_SUCCESS;
}
STDCALL(advapi32, RegEnumKeyA, 4) { return ERROR_NO_MORE_ITEMS; }
STDCALL(advapi32, GetUserNameA, 2) {
    const uint32_t n = copy_out(a[0], rd32(a[1]), "Player");
    wr32(a[1], n + 1);
    return 1;
}

// ---- shell32 ----
STDCALL(shell32, SHGetFolderPathA, 5) {
    const std::string p = folder_path(a[1]);
    if (p.empty()) {
        rt::log("SHGetFolderPathA: unsupported CSIDL 0x%X", a[1]);
        return 0x80004005;  // E_FAIL
    }
    mkdirs(vfs::host_path(p));
    copy_out(a[4], 260, p);
    return 0;
}
STDCALL(shell32, ShellExecuteA, 6) {
    rt::log("ShellExecuteA(%s, %s) ignored", a.str(1) ? a.str(1) : "", a.str(2) ? a.str(2) : "");
    return 2;  // SE_ERR_FNF
}
STDCALL(shell32, CommandLineToArgvW, 2) {
    // One argument: the executable path.
    const std::string exe = std::string(vfs::kInstallDir) + "\\dirt2_game.exe";
    const uint32_t block = rt::heap_alloc(uint32_t(8 + (exe.size() + 1) * 2), true);
    wr32(block, block + 8);
    wr32(block + 4, 0);
    copy_out_w(block + 8, uint32_t(exe.size() + 1), exe);
    if (a[1]) wr32(a[1], 1);
    return block;
}

// ---- shlwapi ----
STDCALL(shlwapi, PathRemoveFileSpecA, 1) {
    std::string s = a.str(0);
    const size_t p = s.find_last_of("\\/");
    if (p == std::string::npos) return 0;
    wr8(a[0] + uint32_t(p == 2 && s[1] == ':' ? 3 : p), 0);
    return 1;
}
STDCALL(shlwapi, PathStripPathW, 1) {
    const std::string s = from_utf16(a[0]);
    const size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) copy_out_w(a[0], uint32_t(s.size() + 1), s.substr(p + 1));
    return 0;
}
STDCALL(shlwapi, StrStrIA, 2) {
    const char* hay = a.str(0);
    const char* needle = a.str(1);
    const size_t n = std::strlen(needle);
    for (const char* p = hay; *p; ++p)
        if (strncasecmp(p, needle, n) == 0) return a[0] + uint32_t(p - hay);
    return 0;
}

// ---- version ----
STDCALL(version, GetFileVersionInfoSizeA, 2) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return 0;
}
STDCALL(version, GetFileVersionInfoA, 4) { return 0; }
STDCALL(version, VerQueryValueA, 4) { return 0; }

// ---- ole32 / oleaut32 ----
STDCALL(ole32, CoInitialize, 1) { return 0; }
STDCALL(ole32, CoUninitialize, 0) { return 0; }
STDCALL(ole32, CoCreateInstance, 5) {
    if (a[4]) wr32(a[4], 0);
    return 0x80040154;  // REGDB_E_CLASSNOTREG (WMI and DirectShow are not available)
}
STDCALL(ole32, CoSetProxyBlanket, 8) { return 0; }
STDCALL_ORD(oleaut32, 2, 1) {  // SysAllocString
    if (!a[0]) return 0;
    const uint32_t n = wlen(a[0]);
    const uint32_t b = rt::heap_alloc(4 + 2 * n + 2);
    wr32(b, 2 * n);
    std::memcpy(rt::host_ptr(b + 4), rt::host_ptr(a[0]), 2 * n + 2);
    return b + 4;
}
STDCALL_ORD(oleaut32, 6, 1) {  // SysFreeString
    if (a[0]) rt::heap_free(a[0] - 4);
    return 0;
}
STDCALL_ORD(oleaut32, 8, 1) {  // VariantInit
    std::memset(rt::host_ptr(a[0]), 0, 16);
    return 0;
}
STDCALL_ORD(oleaut32, 9, 1) {  // VariantClear
    std::memset(rt::host_ptr(a[0]), 0, 16);
    return 0;
}

// ---- imagehlp / crypt32 / wintrust: the game checks its own signature ----
STDCALL(imagehlp, SymInitialize, 3) { return 1; }
STDCALL(imagehlp, SymSetOptions, 1) { return a[0]; }
STDCALL(imagehlp, SymFromAddr, 4) { return 0; }
STDCALL(imagehlp, ImageEnumerateCertificates, 5) {
    if (a[2]) wr32(a[2], 0);
    return 1;
}
STDCALL(imagehlp, ImageGetCertificateHeader, 3) { return 0; }
STDCALL(imagehlp, ImageGetCertificateData, 4) { return 0; }
STDCALL(wintrust, WinVerifyTrust, 3) { return 0; }  // trusted
STDCALL(crypt32, CryptVerifyMessageSignature, 7) { return 0; }
STDCALL(crypt32, CertGetNameStringA, 6) { return copy_out(a[4], a[5], "Codemasters Software Company Limited") + 1; }
STDCALL(crypt32, CertFreeCertificateContext, 1) { return 1; }

// ---- winmm ----
STDCALL(winmm, timeGetTime, 0) {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- psapi: the process list contains only the game itself ----
STDCALL(psapi, EnumProcesses, 3) {  // (pids, cb, *needed)
    if (a[1] >= 4 && a[0]) wr32(a[0], 0x10);
    if (a[2]) wr32(a[2], 4);
    return 1;
}
STDCALL(psapi, EnumProcessModules, 4) {  // (process, modules, cb, *needed)
    if (a[2] >= 4 && a[1]) wr32(a[1], recomp_image_info.image_base);
    if (a[3]) wr32(a[3], 4);
    return 1;
}
STDCALL(psapi, GetModuleBaseNameA, 4) { return copy_out(a[2], a[3], "dirt2_game.exe"); }  // (process, module, name, size)
STDCALL(psapi, GetProcessMemoryInfo, 3) {  // (process, PROCESS_MEMORY_COUNTERS*, cb)
    if (a[1] && a[2] >= 40) {
        std::memset(rt::host_ptr(a[1]), 0, a[2]);
        wr32(a[1], a[2]);
        wr32(a[1] + 12, 512u << 20);  // WorkingSetSize
    }
    return 1;
}
