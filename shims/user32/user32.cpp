// user32 + gdi32, headless. Windows are records with a wndproc; messages go
// through a per-process queue that the platform layer will feed with input.
// Window creation delivers WM_NCCREATE/WM_CREATE synchronously like Windows.
#include <cwctype>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>

#include "../../platform/platform.h"
#include "../guest_printf.h"
#include "../shim.h"

using namespace shim;

namespace {

constexpr uint32_t WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
                   WM_QUIT = 0x12, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_NCCREATE = 0x81;

struct WndClass {
    uint32_t wndproc = 0, style = 0, instance = 0;
};
struct Window {
    uint32_t wndproc = 0, style = 0, exstyle = 0, userdata = 0;
    int32_t x = 0, y = 0, w = 1280, h = 720;
    bool visible = false;
    platform::Window host = nullptr;  // SDL window backing this HWND
};
struct Msg {
    uint32_t hwnd, msg, wparam, lparam;
};

std::mutex g_mu;
std::map<std::string, WndClass> g_classes;  // lower-case class name or "#atom"
std::map<uint32_t, Window> g_windows;
std::deque<Msg> g_queue;
uint32_t g_next_hwnd = 0x00010010, g_next_atom = 0xC000;
uint32_t g_focus = 0;
int g_cursor_count = 0;

void screen_size(int32_t* w, int32_t* h) {
    static int sw = 0, sh = 0;
    if (!sw) platform::display_size(&sw, &sh);
    *w = sw;
    *h = sh;
}

std::string class_key(uint32_t name_or_atom, bool wide) {
    if (name_or_atom < 0x10000) return "#" + std::to_string(name_or_atom);
    std::string s = wide ? from_utf16(name_or_atom) : rt::host_str(name_or_atom);
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

uint32_t register_class(uint32_t wc, bool wide) {
    WndClass k;
    k.style = rd32(wc + 4);
    k.wndproc = rd32(wc + 8);
    k.instance = rd32(wc + 20);
    const std::string name = class_key(rd32(wc + 40), wide);
    std::lock_guard lk(g_mu);
    const uint32_t atom = g_next_atom++;
    g_classes[name] = k;
    g_classes["#" + std::to_string(atom)] = k;
    return atom;
}

uint32_t wndproc_of(uint32_t hwnd) {
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(hwnd);
    return it == g_windows.end() ? 0 : it->second.wndproc;
}

uint32_t send(X86* c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    const uint32_t proc = wndproc_of(hwnd);
    if (!proc) return 0;
    return callback(c, proc, {hwnd, msg, wp, lp});
}

uint32_t create_window(X86* c, const Args& a, bool wide) {
    const std::string key = class_key(a[1], wide);
    WndClass k;
    {
        std::lock_guard lk(g_mu);
        auto it = g_classes.find(key);
        if (it == g_classes.end()) {
            rt::log("CreateWindowEx: unknown class %s", key.c_str());
            return 0;
        }
        k = it->second;
    }
    Window w;
    w.wndproc = k.wndproc;
    w.exstyle = a[0];
    w.style = a[3];
    w.x = int32_t(a[4]) == int32_t(0x80000000) ? 0 : int32_t(a[4]);
    w.y = int32_t(a[5]) == int32_t(0x80000000) ? 0 : int32_t(a[5]);
    if (int32_t(a[6]) > 0 && int32_t(a[6]) != int32_t(0x80000000)) w.w = int32_t(a[6]);
    if (int32_t(a[7]) > 0 && int32_t(a[7]) != int32_t(0x80000000)) w.h = int32_t(a[7]);
    // Top-level windows get a real host window; child windows are records only.
    if (!(w.style & 0x40000000))  // WS_CHILD
        w.host = platform::create_window(w.w, w.h, "DiRT 2");
    uint32_t hwnd;
    {
        std::lock_guard lk(g_mu);
        hwnd = g_next_hwnd;
        g_next_hwnd += 0x10;
        g_windows[hwnd] = w;
    }
    // CREATESTRUCTA on the heap for WM_NCCREATE/WM_CREATE.
    const uint32_t cs = rt::heap_alloc(48, true);
    wr32(cs + 0, a[11]);   // lpCreateParams
    wr32(cs + 4, a[10]);   // hInstance
    wr32(cs + 8, a[9]);    // hMenu
    wr32(cs + 12, a[8]);   // hwndParent
    wr32(cs + 16, uint32_t(w.h));
    wr32(cs + 20, uint32_t(w.w));
    wr32(cs + 24, uint32_t(w.y));
    wr32(cs + 28, uint32_t(w.x));
    wr32(cs + 32, w.style);
    wr32(cs + 36, a[2]);
    wr32(cs + 40, a[1]);
    wr32(cs + 44, w.exstyle);
    send(c, hwnd, WM_NCCREATE, 0, cs);
    send(c, hwnd, WM_CREATE, 0, cs);
    rt::heap_free(cs);
    rt::log("CreateWindowEx(%s) -> 0x%X (%dx%d)", key.c_str(), hwnd, w.w, w.h);
    return hwnd;
}

uint32_t peek(const Args& a) {
    std::lock_guard lk(g_mu);
    if (g_queue.empty()) return 0;
    const Msg m = g_queue.front();
    if (a[4] & 1) g_queue.pop_front();  // PM_REMOVE
    const uint32_t d = a[0];
    wr32(d + 0, m.hwnd);
    wr32(d + 4, m.msg);
    wr32(d + 8, m.wparam);
    wr32(d + 12, m.lparam);
    wr32(d + 16, 0);
    wr32(d + 20, 0);
    wr32(d + 24, 0);
    return 1;
}

uint32_t dispatch(X86* c, uint32_t msg) {
    return send(c, rd32(msg), rd32(msg + 4), rd32(msg + 8), rd32(msg + 12));
}

void write_rect(uint32_t r, int32_t l, int32_t t, int32_t rr, int32_t b) {
    wr32(r + 0, uint32_t(l));
    wr32(r + 4, uint32_t(t));
    wr32(r + 8, uint32_t(rr));
    wr32(r + 12, uint32_t(b));
}

struct Mode {
    uint32_t w, h, hz;
};
const Mode kModes[] = {{800, 600, 60}, {1024, 768, 60}, {1280, 720, 60}, {1280, 800, 60},
                       {1366, 768, 60}, {1600, 900, 60}, {1920, 1080, 60}};

}  // namespace

namespace shim {
// Host window behind an HWND (used by the D3D9 bridge).
void* host_window(uint32_t hwnd) {
    std::lock_guard lk(g_mu);
    if (!hwnd && !g_windows.empty()) return g_windows.begin()->second.host;
    auto it = g_windows.find(hwnd);
    return it == g_windows.end() ? nullptr : it->second.host;
}

// Used by the platform layer to deliver input and lifecycle messages.
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    std::lock_guard lk(g_mu);
    if (!hwnd && !g_windows.empty() && msg != WM_QUIT) hwnd = g_windows.begin()->first;
    g_queue.push_back(Msg{hwnd, msg, wp, lp});
}
}  // namespace shim

STDCALL(user32, RegisterClassExA, 1) { return register_class(a[0], false); }
STDCALL(user32, RegisterClassExW, 1) { return register_class(a[0], true); }
STDCALL(user32, UnregisterClassA, 2) { return 1; }
STDCALL(user32, UnregisterClassW, 2) { return 1; }
STDCALL(user32, CreateWindowExA, 12) { return create_window(c, a, false); }
STDCALL(user32, CreateWindowExW, 12) { return create_window(c, a, true); }
STDCALL(user32, DestroyWindow, 1) {
    send(c, a[0], WM_DESTROY, 0, 0);
    platform::Window host = nullptr;
    {
        std::lock_guard lk(g_mu);
        auto it = g_windows.find(a[0]);
        if (it != g_windows.end()) host = it->second.host;
        g_windows.erase(a[0]);
    }
    if (host) platform::destroy_window(host);
    return 1;
}
STDCALL(user32, DefWindowProcA, 4) { return a[1] == WM_NCCREATE ? 1 : 0; }
STDCALL(user32, DefWindowProcW, 4) { return a[1] == WM_NCCREATE ? 1 : 0; }
STDCALL(user32, PeekMessageA, 5) { return peek(a); }
STDCALL(user32, PeekMessageW, 5) { return peek(a); }
STDCALL(user32, TranslateMessage, 1) { return 0; }
STDCALL(user32, DispatchMessageA, 1) { return dispatch(c, a[0]); }
STDCALL(user32, DispatchMessageW, 1) { return dispatch(c, a[0]); }
STDCALL(user32, SendMessageA, 4) { return send(c, a[0], a[1], a[2], a[3]); }
STDCALL(user32, PostQuitMessage, 1) {
    post_message(0, WM_QUIT, a[0], 0);
    return 0;
}
STDCALL(user32, ShowWindow, 2) {
    platform::Window host = nullptr;
    bool was_visible;
    {
        std::lock_guard lk(g_mu);
        auto it = g_windows.find(a[0]);
        if (it == g_windows.end()) return 0;
        was_visible = it->second.visible;
        it->second.visible = a[1] != 0;
        host = it->second.host;
    }
    if (host) platform::show_window(host, a[1] != 0);
    send(c, a[0], WM_SHOWWINDOW, a[1] != 0, 0);
    if (a[1] && !was_visible) {
        // Windows activates a newly shown top-level window.
        send(c, a[0], WM_ACTIVATEAPP, 1, 0);
        send(c, a[0], WM_ACTIVATE, 1, 0);
        send(c, a[0], WM_SETFOCUS, 0, 0);
    }
    return was_visible;
}
STDCALL(user32, UpdateWindow, 1) { return 1; }
STDCALL(user32, IsWindowVisible, 1) {
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(a[0]);
    return it != g_windows.end() && it->second.visible;
}
STDCALL(user32, SetWindowPos, 7) {
    platform::Window host = nullptr;
    int32_t w = 0, h = 0;
    const uint32_t flags = a[6];
    {
        std::lock_guard lk(g_mu);
        auto it = g_windows.find(a[0]);
        if (it == g_windows.end()) return 0;
        if (!(flags & 0x2)) { it->second.x = int32_t(a[2]); it->second.y = int32_t(a[3]); }  // SWP_NOMOVE
        if (!(flags & 0x1)) { it->second.w = int32_t(a[4]); it->second.h = int32_t(a[5]); }  // SWP_NOSIZE
        host = it->second.host;
        w = it->second.w;
        h = it->second.h;
    }
    if (host && !(flags & 0x1) && w > 0 && h > 0) platform::set_window_size(host, w, h);
    return 1;
}
STDCALL(user32, AdjustWindowRect, 3) { return 1; }  // borderless: client == window
STDCALL(user32, GetClientRect, 2) {
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    write_rect(a[1], 0, 0, it->second.w, it->second.h);
    return 1;
}
STDCALL(user32, GetWindowRect, 2) {
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    const Window& w = it->second;
    write_rect(a[1], w.x, w.y, w.x + w.w, w.y + w.h);
    return 1;
}
STDCALL(user32, GetWindowLongA, 2) {
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    switch (int32_t(a[1])) {
        case -4: return it->second.wndproc;
        case -16: return it->second.style;
        case -20: return it->second.exstyle;
        case -21: return it->second.userdata;
        default: return 0;
    }
}
STDCALL(user32, SetWindowLongA, 3) {
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(a[0]);
    if (it == g_windows.end()) return 0;
    uint32_t* field = nullptr;
    switch (int32_t(a[1])) {
        case -4: field = &it->second.wndproc; break;
        case -16: field = &it->second.style; break;
        case -20: field = &it->second.exstyle; break;
        case -21: field = &it->second.userdata; break;
        default: return 0;
    }
    const uint32_t prev = *field;
    *field = a[2];
    return prev;
}
STDCALL(user32, GetWindowTextA, 3) { return copy_out(a[1], a[2], "DiRT 2"); }
STDCALL(user32, SetFocus, 1) {
    const uint32_t prev = g_focus;
    g_focus = a[0];
    return prev;
}
STDCALL(user32, ShowCursor, 1) { return uint32_t(a[0] ? ++g_cursor_count : --g_cursor_count); }
STDCALL(user32, ClipCursor, 1) { return 1; }
STDCALL(user32, ReleaseCapture, 0) { return 1; }
STDCALL(user32, LoadCursorA, 2) { return 0x00010003; }
STDCALL(user32, LoadIconA, 2) { return 0x00010005; }
STDCALL(user32, LoadBitmapA, 2) { return 0x00010007; }
STDCALL(user32, GetDC, 1) { return 0x00010101; }
STDCALL(user32, ReleaseDC, 2) { return 1; }
STDCALL(user32, BeginPaint, 2) {
    std::memset(rt::host_ptr(a[1]), 0, 64);
    wr32(a[1], 0x00010101);
    return 0x00010101;
}
STDCALL(user32, EndPaint, 2) { return 1; }
STDCALL(user32, GetSystemMetrics, 1) {
    int32_t sw, sh;
    screen_size(&sw, &sh);
    switch (a[0]) {
        case 0: return uint32_t(sw);   // SM_CXSCREEN
        case 1: return uint32_t(sh);   // SM_CYSCREEN
        case 78: return uint32_t(sw);  // SM_CXVIRTUALSCREEN
        case 79: return uint32_t(sh);
        case 80: return 1;                     // SM_CMONITORS
        default: return 0;
    }
}
STDCALL(user32, EnumDisplaySettingsA, 3) {
    const uint32_t mode = a[1], d = a[2];
    const Mode* m;
    if (mode == 0xFFFFFFFF || mode == 0xFFFFFFFE) m = &kModes[std::size(kModes) - 1];  // current / registry
    else if (mode < std::size(kModes)) m = &kModes[mode];
    else return 0;
    std::memset(rt::host_ptr(d + 32), 0, 124);
    copy_out(d, 32, "\\\\.\\DISPLAY1");
    wr16(d + 36, 156);
    wr32(d + 40, 0x5C0000);  // DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
    wr32(d + 104, 32);
    wr32(d + 108, m->w);
    wr32(d + 112, m->h);
    wr32(d + 120, m->hz);
    return 1;
}
STDCALL(user32, SystemParametersInfoA, 4) { return 1; }
STDCALL(user32, SetRect, 5) {
    write_rect(a[0], int32_t(a[1]), int32_t(a[2]), int32_t(a[3]), int32_t(a[4]));
    return 1;
}
STDCALL(user32, MessageBoxA, 4) {
    rt::log("MessageBoxA: [%s] %s", a.str(2) ? a.str(2) : "", a.str(1) ? a.str(1) : "");
    return 1;  // IDOK
}
STDCALL(user32, MessageBoxW, 4) {
    rt::log("MessageBoxW: [%s] %s", from_utf16(a[2]).c_str(), from_utf16(a[1]).c_str());
    return 1;
}
STDCALL(user32, SetWindowsHookExA, 4) { return 0x00020001; }
STDCALL(user32, UnhookWindowsHookEx, 1) { return 1; }
STDCALL(user32, CallNextHookEx, 4) { return 0; }
STDCALL(user32, SendInput, 3) { return a[0]; }
STDCALL(user32, MapVirtualKeyA, 2) { return 0; }
STDCALL(user32, ToUnicode, 6) { return 0; }
STDCALL(user32, CharLowerA, 1) {
    if (a[0] < 0x10000) return uint32_t(std::tolower(int(a[0])));
    for (uint32_t p = a[0]; rd8(p); ++p) wr8(p, uint8_t(std::tolower(rd8(p))));
    return a[0];
}
STDCALL(user32, CharUpperA, 1) {
    if (a[0] < 0x10000) return uint32_t(std::toupper(int(a[0])));
    for (uint32_t p = a[0]; rd8(p); ++p) wr8(p, uint8_t(std::toupper(rd8(p))));
    return a[0];
}
STDCALL(user32, CharLowerW, 1) {
    if (a[0] < 0x10000) return uint32_t(std::towlower(wint_t(a[0])));
    for (uint32_t p = a[0]; rd16(p); p += 2) wr16(p, uint16_t(std::towlower(rd16(p))));
    return a[0];
}
STDCALL(user32, CharUpperW, 1) {
    if (a[0] < 0x10000) return uint32_t(std::towupper(wint_t(a[0])));
    for (uint32_t p = a[0]; rd16(p); p += 2) wr16(p, uint16_t(std::towupper(rd16(p))));
    return a[0];
}
CDECL(user32, wsprintfA) {
    const std::string s = guest_format(a.str(1), c->esp + 12);
    return copy_out(a[0], 1025, s);
}

// ---- gdi32: only used for the splash bitmap and capability queries ----
STDCALL(gdi32, CreateDCA, 4) { return 0x00010201; }
STDCALL(gdi32, CreateCompatibleDC, 1) { return 0x00010202; }
STDCALL(gdi32, DeleteDC, 1) { return 1; }
STDCALL(gdi32, DeleteObject, 1) { return 1; }
STDCALL(gdi32, SelectObject, 2) { return 0x00010301; }
STDCALL(gdi32, GetStockObject, 1) { return 0x00010400 + a[0]; }
STDCALL(gdi32, CreateBrushIndirect, 1) { return 0x00010501; }
STDCALL(gdi32, BitBlt, 9) { return 1; }
STDCALL(gdi32, GetObjectA, 3) {
    if (a[2] && a[1] >= 24) std::memset(rt::host_ptr(a[2]), 0, 24);
    return a[1] >= 24 ? 24 : 0;
}
STDCALL(gdi32, GetDIBits, 7) { return 0; }
STDCALL(gdi32, ExtEscape, 6) { return 0; }
