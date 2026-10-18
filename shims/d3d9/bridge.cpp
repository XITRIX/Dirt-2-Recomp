// Direct3D 9 bridge: the game's 32-bit COM calls -> DXVK's native d3d9.
//
// Every host object handed to the game is represented by a small proxy in
// guest memory whose vtable entries are dispatch thunks into this file. A
// method's arguments are converted from guest stack slots by type (see Arg<>):
//   - values and floats pass through;
//   - pointers to pointer-free structs (matrices, viewports, caps, vertex
//     elements, rects...) are passed as host pointers into guest memory, which
//     works because guest memory is host memory and the layouts match;
//   - interfaces map proxy <-> host object; out-interfaces are wrapped;
//   - the few structs holding pointers or HWNDs are copied and converted.
// Lock/Unlock return guest-memory staging copies, since the game can only
// address the 4 GB guest space.
#include <d3d9.h>

#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../platform/platform.h"
#include "../shim.h"

namespace shim {
void* host_window(uint32_t hwnd);
}

using namespace shim;

namespace {

// ---------------------------------------------------------------------------
// Proxies
// ---------------------------------------------------------------------------

struct VTableDesc {
    const char* name;
    const recomp_fn* fns;
    uint32_t count;
};
template <typename I>
const VTableDesc& vtable_desc();

constexpr uint32_t kProxyMagic = 0x42443344;  // "D3DB"

struct Entry {
    IUnknown* host;
    const VTableDesc* desc;
};

std::recursive_mutex g_mu;
std::unordered_map<uint32_t, Entry> g_by_guest;
std::unordered_map<IUnknown*, uint32_t> g_by_host;
std::unordered_map<const VTableDesc*, uint32_t> g_vtables;
std::atomic<uint32_t> g_marked_ps{0};  // debug: pixel shader whose CTAB names DIRT2_WATCH_PS_CONST

// Vertex and index buffers keep a persistent guest shadow (they are locked
// every frame); the shadow mirrors the buffer, so only READONLY locks copy in.
struct BufferShadow {
    uint32_t guest = 0, size = 0;
    struct Lock { uint32_t offset, size; uint8_t* host; bool readonly; };
    std::vector<Lock> locks;
};
std::unordered_map<uint32_t, BufferShadow> g_shadows;

uint32_t guest_vtable(const VTableDesc& d) {
    auto it = g_vtables.find(&d);
    if (it != g_vtables.end()) return it->second;
    const uint32_t vt = rt::heap_alloc(d.count * 4);
    for (uint32_t i = 0; i < d.count; ++i) wr32(vt + 4 * i, rt::alloc_thunk(d.fns[i]));
    g_vtables[&d] = vt;
    return vt;
}

uint32_t wrap_with(IUnknown* host, const VTableDesc& d) {
    if (!host) return 0;
    std::lock_guard lk(g_mu);
    if (auto it = g_by_host.find(host); it != g_by_host.end()) return it->second;
    const uint32_t p = rt::heap_alloc(16, true);
    wr32(p, guest_vtable(d));
    wr32(p + 4, kProxyMagic);
    g_by_guest[p] = Entry{host, &d};
    g_by_host[host] = p;
    return p;
}

IUnknown* to_host(uint32_t guest) {
    if (!guest) return nullptr;
    std::lock_guard lk(g_mu);
    auto it = g_by_guest.find(guest);
    if (it == g_by_guest.end()) {
        rt::log("d3d9: unknown proxy 0x%08X", guest);
        return nullptr;
    }
    return it->second.host;
}

void forget(uint32_t guest) {
    std::lock_guard lk(g_mu);
    auto it = g_by_guest.find(guest);
    if (it == g_by_guest.end()) return;
    g_by_host.erase(it->second.host);
    g_by_guest.erase(it);
    // The proxy memory is left in place: the game may still compare the pointer.
}

// Wraps with the object's most-derived interface when the static type is a base.
template <typename I>
uint32_t wrap(I* p) {
    if (!p) return 0;
    if constexpr (std::is_same_v<I, IDirect3DResource9> || std::is_same_v<I, IDirect3DBaseTexture9>) {
        switch (p->GetType()) {
            case D3DRTYPE_SURFACE: return wrap_with(p, vtable_desc<IDirect3DSurface9>());
            case D3DRTYPE_VOLUME: return wrap_with(p, vtable_desc<IDirect3DVolume9>());
            case D3DRTYPE_TEXTURE: return wrap_with(p, vtable_desc<IDirect3DTexture9>());
            case D3DRTYPE_VOLUMETEXTURE: return wrap_with(p, vtable_desc<IDirect3DVolumeTexture9>());
            case D3DRTYPE_CUBETEXTURE: return wrap_with(p, vtable_desc<IDirect3DCubeTexture9>());
            case D3DRTYPE_VERTEXBUFFER: return wrap_with(p, vtable_desc<IDirect3DVertexBuffer9>());
            case D3DRTYPE_INDEXBUFFER: return wrap_with(p, vtable_desc<IDirect3DIndexBuffer9>());
            default: return wrap_with(p, vtable_desc<I>());
        }
    } else {
        return wrap_with(p, vtable_desc<I>());
    }
}

// ---------------------------------------------------------------------------
// Argument conversion, one 32-bit guest stack slot per argument
// ---------------------------------------------------------------------------

template <typename T>
constexpr bool is_interface_v = std::is_base_of_v<IUnknown, std::remove_cv_t<T>>;

template <typename T, typename = void>
struct Arg;

template <typename T>
struct Arg<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>> {
    uint32_t s;
    T get() const { return T(s); }
    void done() {}
};

template <>
struct Arg<float> {
    uint32_t s;
    float get() const { float f; std::memcpy(&f, &s, 4); return f; }
    void done() {}
};

// Interface in.
template <typename I>
struct Arg<I*, std::enable_if_t<is_interface_v<I>>> {
    uint32_t s;
    I* get() const { return static_cast<I*>(to_host(s)); }
    void done() {}
};

// Interface out: write the (possibly new) proxy back.
template <typename I>
struct Arg<I**, std::enable_if_t<is_interface_v<I>>> {
    uint32_t s;
    I* v = nullptr;
    I** get() { return s ? &v : nullptr; }
    void done() {
        if (s) wr32(s, wrap(v));
    }
};

// HANDLE* pSharedHandle (and other void** outs): must be NULL for D3D9Ex-less use.
template <>
struct Arg<void**> {
    uint32_t s;
    void* v = nullptr;
    void** get() { return s ? &v : nullptr; }
    void done() {
        if (s) wr32(s, 0);
    }
};

// REFIID / REFGUID.
template <>
struct Arg<const GUID&> {
    uint32_t s;
    const GUID& get() const { return *reinterpret_cast<const GUID*>(rt::host_ptr(s)); }
    void done() {}
};

// Pointers to pointer-free data: guest memory used in place.
template <typename T>
struct Arg<T*, std::enable_if_t<!is_interface_v<T> && !std::is_pointer_v<T> && !std::is_same_v<std::remove_cv_t<T>, D3DPRESENT_PARAMETERS> &&
                                !std::is_same_v<std::remove_cv_t<T>, D3DDEVICE_CREATION_PARAMETERS>>> {
    uint32_t s;
    T* get() const { return s ? reinterpret_cast<T*>(rt::host_ptr(s)) : nullptr; }
    void done() {}
};

// D3DPRESENT_PARAMETERS: 14 dwords in the guest, hDeviceWindow is a guest HWND.
void pp_in(uint32_t g, D3DPRESENT_PARAMETERS& p) {
    p.BackBufferWidth = rd32(g + 0);
    p.BackBufferHeight = rd32(g + 4);
    p.BackBufferFormat = D3DFORMAT(rd32(g + 8));
    p.BackBufferCount = rd32(g + 12);
    p.MultiSampleType = D3DMULTISAMPLE_TYPE(rd32(g + 16));
    p.MultiSampleQuality = rd32(g + 20);
    p.SwapEffect = D3DSWAPEFFECT(rd32(g + 24));
    p.hDeviceWindow = HWND(host_window(rd32(g + 28)));
    p.Windowed = BOOL(rd32(g + 32));
    p.EnableAutoDepthStencil = BOOL(rd32(g + 36));
    p.AutoDepthStencilFormat = D3DFORMAT(rd32(g + 40));
    p.Flags = rd32(g + 44);
    p.FullScreen_RefreshRateInHz = rd32(g + 48);
    p.PresentationInterval = rd32(g + 52);
}
void pp_out(uint32_t g, const D3DPRESENT_PARAMETERS& p, uint32_t guest_hwnd) {
    wr32(g + 0, p.BackBufferWidth);
    wr32(g + 4, p.BackBufferHeight);
    wr32(g + 8, uint32_t(p.BackBufferFormat));
    wr32(g + 12, p.BackBufferCount);
    wr32(g + 16, uint32_t(p.MultiSampleType));
    wr32(g + 20, p.MultiSampleQuality);
    wr32(g + 24, uint32_t(p.SwapEffect));
    wr32(g + 28, guest_hwnd);
    wr32(g + 32, uint32_t(p.Windowed));
    wr32(g + 36, uint32_t(p.EnableAutoDepthStencil));
    wr32(g + 40, uint32_t(p.AutoDepthStencilFormat));
    wr32(g + 44, p.Flags);
    wr32(g + 48, p.FullScreen_RefreshRateInHz);
    wr32(g + 52, p.PresentationInterval);
}

// Exclusive fullscreen becomes a windowed swapchain on a borderless desktop-
// fullscreen window; the game still sees the parameters it asked for.
// DIRT2_WINDOWED=1 keeps a normal window sized to the back buffer instead.
void present_window(const D3DPRESENT_PARAMETERS& p, bool fullscreen) {
    if (!p.hDeviceWindow) return;
    static const bool force_windowed = std::getenv("DIRT2_WINDOWED") != nullptr;
    if (fullscreen && !force_windowed) {
        platform::set_fullscreen(p.hDeviceWindow, true);
    } else {
        platform::set_fullscreen(p.hDeviceWindow, false);
        if (p.BackBufferWidth && p.BackBufferHeight)
            platform::set_window_size(p.hDeviceWindow, int(p.BackBufferWidth), int(p.BackBufferHeight));
    }
    platform::show_window(p.hDeviceWindow, true);
}

template <>
struct Arg<D3DPRESENT_PARAMETERS*> {
    uint32_t s;
    D3DPRESENT_PARAMETERS v{};
    uint32_t hwnd = 0;
    BOOL windowed = TRUE;
    UINT refresh = 0;
    D3DPRESENT_PARAMETERS* get() {
        if (!s) return nullptr;
        hwnd = rd32(s + 28);
        pp_in(s, v);
        windowed = v.Windowed;
        refresh = v.FullScreen_RefreshRateInHz;
        if (!windowed) {
            v.Windowed = TRUE;
            v.FullScreen_RefreshRateInHz = 0;
        }
        return &v;
    }
    void apply_window(HWND fallback) {
        if (!s) return;
        if (!v.hDeviceWindow) v.hDeviceWindow = fallback;
        present_window(v, !windowed);
    }
    void done() {
        if (!s) return;
        v.Windowed = windowed;
        v.FullScreen_RefreshRateInHz = refresh;
        pp_out(s, v, hwnd);
    }
};

template <>
struct Arg<D3DDEVICE_CREATION_PARAMETERS*> {
    uint32_t s;
    D3DDEVICE_CREATION_PARAMETERS v{};
    D3DDEVICE_CREATION_PARAMETERS* get() { return s ? &v : nullptr; }
    void done() {
        if (!s) return;
        wr32(s + 0, v.AdapterOrdinal);
        wr32(s + 4, uint32_t(v.DeviceType));
        wr32(s + 8, 0x00010010);  // the game's main window
        wr32(s + 12, v.BehaviorFlags);
    }
};

// ---------------------------------------------------------------------------
// Generic method bridge
// ---------------------------------------------------------------------------

template <typename R>
void set_result(X86* c, R r) {
    if constexpr (std::is_same_v<R, float>) fpush(c, double(r));
    else c->eax = uint32_t(r);
}

template <typename F, F M>
struct BridgeImpl;

template <typename C, typename R, typename... A, R (C::*M)(A...)>
struct BridgeImpl<R (C::*)(A...), M> {
    template <size_t... I>
    static void run(X86* c, std::index_sequence<I...>) {
        C* self = static_cast<C*>(to_host(rd32(c->esp + 4)));
        std::tuple<Arg<A>...> args{Arg<A>{rd32(c->esp + 8 + 4 * uint32_t(I))}...};
        if (!self) {
            c->eax = uint32_t(D3DERR_INVALIDCALL);
        } else if constexpr (std::is_void_v<R>) {
            (self->*M)(std::get<I>(args).get()...);
        } else {
            set_result<R>(c, (self->*M)(std::get<I>(args).get()...));
        }
        (std::get<I>(args).done(), ...);
        c->esp += 4 * (2 + uint32_t(sizeof...(A)));  // return address, this, arguments
    }
    static void call(X86* c) { run(c, std::index_sequence_for<A...>{}); }
};

template <auto M>
struct Select {
    static constexpr recomp_fn fn = &BridgeImpl<decltype(M), M>::call;
};

// Overrides: Select<&I::Method> specialisations pointing at hand-written bridges.
#define OVERRIDE(I, M, F) \
    template <>           \
    struct Select<&I::M> { static constexpr recomp_fn fn = &F; };

#define SLOT(I, M) Select<&I::M>::fn

// ---- IUnknown ----

void flush_shadows(uint32_t proxy);  // SYSTEMMEM shadows (defined with the lock code)
void drop_shadows(uint32_t proxy);

template <typename I>
void QI(X86* c);
template <typename I>
void AddRefB(X86* c) {
    IUnknown* self = to_host(rd32(c->esp + 4));
    c->eax = self ? self->AddRef() : 0;
    c->esp += 8;
}
template <typename I>
void ReleaseB(X86* c) {
    const uint32_t g = rd32(c->esp + 4);
    IUnknown* self = to_host(g);
    const ULONG r = self ? self->Release() : 0;
    if (self && r == 0) {
        forget(g);
        drop_shadows(g);
    }
    c->eax = r;
    c->esp += 8;
}

// Returns the guest proxy wrapping obj according to the requested IID.
uint32_t wrap_by_iid(const GUID& iid, void* obj);

template <typename I>
void QI(X86* c) {
    IUnknown* self = to_host(rd32(c->esp + 4));
    const GUID& iid = *reinterpret_cast<const GUID*>(rt::host_ptr(rd32(c->esp + 8)));
    const uint32_t out = rd32(c->esp + 12);
    void* obj = nullptr;
    const HRESULT hr = self ? self->QueryInterface(iid, &obj) : E_NOINTERFACE;
    if (out) wr32(out, SUCCEEDED(hr) ? wrap_by_iid(iid, obj) : 0);
    c->eax = uint32_t(hr);
    c->esp += 16;
}

// Hand-written bridges (defined below the generated vtables).
void d3d_create_device(X86* c);
void dev_reset(X86* c);
void dev_create_texture(X86* c);
void dev_create_vertex_buffer(X86* c);
void dev_create_index_buffer(X86* c);
void dev_set_vertex_shader(X86* c);
void dev_draw_indexed_primitive(X86* c);
void dev_draw_primitive_up(X86* c);
void dev_draw_primitive(X86* c);
void dev_create_pixel_shader(X86* c);
void dev_create_vertex_shader(X86* c);
void dev_set_pixel_shader(X86* c);
void dev_set_texture(X86* c);
void dev_update_texture(X86* c);
void dev_update_surface(X86* c);
void d3d_get_adapter_monitor(X86* c);
void dev_present(X86* c);
void swap_present(X86* c);
void surf_lock(X86* c);
void surf_unlock(X86* c);
void surf_get_dc(X86* c);
void tex_lock(X86* c);
void tex_unlock(X86* c);
void cube_lock(X86* c);
void cube_unlock(X86* c);
void vol_lock(X86* c);
void vol_unlock(X86* c);
void voltex_lock(X86* c);
void voltex_unlock(X86* c);
void vb_lock(X86* c);
void vb_unlock(X86* c);
void ib_lock(X86* c);
void ib_unlock(X86* c);

OVERRIDE(IDirect3D9, CreateDevice, d3d_create_device)
OVERRIDE(IDirect3D9, GetAdapterMonitor, d3d_get_adapter_monitor)
OVERRIDE(IDirect3DDevice9, Present, dev_present)
OVERRIDE(IDirect3DDevice9, Reset, dev_reset)
OVERRIDE(IDirect3DDevice9, CreateTexture, dev_create_texture)
OVERRIDE(IDirect3DDevice9, CreateVertexBuffer, dev_create_vertex_buffer)
OVERRIDE(IDirect3DDevice9, CreateIndexBuffer, dev_create_index_buffer)
OVERRIDE(IDirect3DDevice9, SetVertexShader, dev_set_vertex_shader)
OVERRIDE(IDirect3DDevice9, DrawIndexedPrimitive, dev_draw_indexed_primitive)
OVERRIDE(IDirect3DDevice9, DrawPrimitiveUP, dev_draw_primitive_up)
OVERRIDE(IDirect3DDevice9, DrawPrimitive, dev_draw_primitive)
OVERRIDE(IDirect3DDevice9, CreatePixelShader, dev_create_pixel_shader)
OVERRIDE(IDirect3DDevice9, CreateVertexShader, dev_create_vertex_shader)
OVERRIDE(IDirect3DDevice9, SetPixelShader, dev_set_pixel_shader)
OVERRIDE(IDirect3DDevice9, SetTexture, dev_set_texture)
OVERRIDE(IDirect3DDevice9, UpdateTexture, dev_update_texture)
OVERRIDE(IDirect3DDevice9, UpdateSurface, dev_update_surface)
OVERRIDE(IDirect3DSwapChain9, Present, swap_present)
OVERRIDE(IDirect3DSurface9, LockRect, surf_lock)
OVERRIDE(IDirect3DSurface9, UnlockRect, surf_unlock)
OVERRIDE(IDirect3DSurface9, GetDC, surf_get_dc)
OVERRIDE(IDirect3DTexture9, LockRect, tex_lock)
OVERRIDE(IDirect3DTexture9, UnlockRect, tex_unlock)
OVERRIDE(IDirect3DCubeTexture9, LockRect, cube_lock)
OVERRIDE(IDirect3DCubeTexture9, UnlockRect, cube_unlock)
OVERRIDE(IDirect3DVolume9, LockBox, vol_lock)
OVERRIDE(IDirect3DVolume9, UnlockBox, vol_unlock)
OVERRIDE(IDirect3DVolumeTexture9, LockBox, voltex_lock)
OVERRIDE(IDirect3DVolumeTexture9, UnlockBox, voltex_unlock)
OVERRIDE(IDirect3DVertexBuffer9, Lock, vb_lock)
OVERRIDE(IDirect3DVertexBuffer9, Unlock, vb_unlock)
OVERRIDE(IDirect3DIndexBuffer9, Lock, ib_lock)
OVERRIDE(IDirect3DIndexBuffer9, Unlock, ib_unlock)


#include "vtables.inc"

// ---------------------------------------------------------------------------
// Hand-written methods
// ---------------------------------------------------------------------------

// IDirect3D9::CreateDevice(adapter, type, hFocusWindow, flags, pp, **device)
void d3d_create_device(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3D9*>(to_host(a[0]));
    Arg<D3DPRESENT_PARAMETERS*> pp{a[5]};
    IDirect3DDevice9* dev = nullptr;
    HWND focus = HWND(host_window(a[3]));
    D3DPRESENT_PARAMETERS* p = pp.get();
    pp.apply_window(focus);
    rt::log("d3d9: CreateDevice %ux%u fmt %u fullscreen %d flags 0x%X backbuffers %u swap %u interval 0x%X ms %u",
            p ? p->BackBufferWidth : 0, p ? p->BackBufferHeight : 0, p ? p->BackBufferFormat : 0, pp.windowed ? 0 : 1,
            a[4], p ? p->BackBufferCount : 0, p ? p->SwapEffect : 0, p ? p->PresentationInterval : 0,
            p ? p->MultiSampleType : 0);
    // The game uses the device from its loader threads as well as the render
    // thread; Windows drivers tolerate that, DXVK only serialises with
    // D3DCREATE_MULTITHREADED. Without it, concurrent calls corrupt DXVK state
    // (device lost on Metal, flicker).
    const DWORD flags = a[4] | D3DCREATE_MULTITHREADED;
    const HRESULT hr = self ? self->CreateDevice(a[1], D3DDEVTYPE(a[2]), focus, flags, p, &dev) : D3DERR_INVALIDCALL;
    pp.done();
    if (a[6]) wr32(a[6], SUCCEEDED(hr) ? wrap(dev) : 0);
    rt::log("d3d9: CreateDevice -> 0x%08X", uint32_t(hr));
    c->eax = uint32_t(hr);
    c->esp += 4 * 8;
}

// IDirect3D9::GetAdapterMonitor returns an HMONITOR.
// IDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS*)
void dev_reset(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    Arg<D3DPRESENT_PARAMETERS*> pp{a[1]};
    D3DPRESENT_PARAMETERS* p = pp.get();
    pp.apply_window(HWND(host_window(0)));
    const HRESULT hr = self ? self->Reset(p) : D3DERR_INVALIDCALL;
    pp.done();
    rt::log("d3d9: Reset %ux%u -> 0x%08X", p ? p->BackBufferWidth : 0, p ? p->BackBufferHeight : 0, uint32_t(hr));
    c->eax = uint32_t(hr);
    c->esp += 4 * 3;
}

// UpdateTexture(src, dst) / UpdateSurface(src, rect, dst, point): the source
// is usually SYSTEMMEM; push its persistent shadows into it first.
void dev_update_texture(X86* c) {
    const Args a{c};
    if (std::getenv("DIRT2_LOG_LOCKS")) rt::log("d3d9: UpdateTexture %08X -> %08X", a[1], a[2]);
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    flush_shadows(a[1]);
    c->eax = self ? uint32_t(self->UpdateTexture(static_cast<IDirect3DBaseTexture9*>(to_host(a[1])),
                                                 static_cast<IDirect3DBaseTexture9*>(to_host(a[2]))))
                  : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 4;
}
void dev_update_surface(X86* c) {
    const Args a{c};
    if (std::getenv("DIRT2_LOG_LOCKS")) rt::log("d3d9: UpdateSurface %08X -> %08X", a[1], a[3]);
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    flush_shadows(a[1]);
    c->eax = self ? uint32_t(self->UpdateSurface(static_cast<IDirect3DSurface9*>(to_host(a[1])), a.ptr<const RECT>(2),
                                                 static_cast<IDirect3DSurface9*>(to_host(a[3])), a.ptr<const POINT>(4)))
                  : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 6;
}

// SetTexture(stage, texture): debug logging of dynamic texture bindings (DIRT2_LOG_LOCKS).
void dev_set_texture(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    auto* tex = static_cast<IDirect3DBaseTexture9*>(to_host(a[2]));
    static const bool log = std::getenv("DIRT2_LOG_LOCKS") != nullptr;
    if (log && tex && tex->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d);
        if (d.Usage & D3DUSAGE_DYNAMIC) rt::log("d3d9: SetTexture(%u, %08X) %ux%u fmt %u", a[1], a[2], d.Width, d.Height, d.Format);
    }
    c->eax = self ? uint32_t(self->SetTexture(a[1], tex)) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 4;
}

// Debug: DIRT2_DUMP_SHADERS=<dir> writes every pixel/vertex shader's bytecode
// as <dir>/<ps|vs>_<proxy>.dxso.
uint32_t shader_length(const uint32_t* w) {
    uint32_t n = 1;
    while (w[n] != 0x0000FFFF) {
        const uint32_t tok = w[n];
        if ((tok & 0xFFFF) == 0xFFFE) n += 1 + (tok >> 16);
        else n += 1 + ((tok >> 24) & 0xF);
    }
    return n + 1;
}
template <bool Pixel>
void dev_create_shader(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    const DWORD* code = a.ptr<const DWORD>(1);
    HRESULT hr = D3DERR_INVALIDCALL;
    uint32_t proxy = 0;
    if (self && code) {
        if constexpr (Pixel) {
            IDirect3DPixelShader9* sh = nullptr;
            hr = self->CreatePixelShader(code, &sh);
            proxy = SUCCEEDED(hr) ? wrap(sh) : 0;
        } else {
            IDirect3DVertexShader9* sh = nullptr;
            hr = self->CreateVertexShader(code, &sh);
            proxy = SUCCEEDED(hr) ? wrap(sh) : 0;
        }
        if constexpr (Pixel) {
            static const char* mark = std::getenv("DIRT2_WATCH_PS_CONST");
            if (mark && proxy) {
                const uint32_t len = shader_length(reinterpret_cast<const uint32_t*>(code)) * 4;
                const char* b = reinterpret_cast<const char*>(code);
                if (std::search(b, b + len, mark, mark + std::strlen(mark)) != b + len) {
                    g_marked_ps = proxy;
                    rt::log("d3d9: watching pixel shader %08X (CTAB names %s)", proxy, mark);
                }
            }
        }
        static const char* dir = std::getenv("DIRT2_DUMP_SHADERS");
        if (dir && proxy) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/%s_%08X.dxso", dir, Pixel ? "ps" : "vs", proxy);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(code, 4, shader_length(reinterpret_cast<const uint32_t*>(code)), f);
                std::fclose(f);
            }
        }
    }
    if (a[2]) wr32(a[2], proxy);
    c->eax = uint32_t(hr);
    c->esp += 4 * 4;
}
std::atomic<uint32_t> g_cur_vs{0};
void dev_set_vertex_shader(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    g_cur_vs = a[1];
    c->eax = self ? uint32_t(self->SetVertexShader(static_cast<IDirect3DVertexShader9*>(to_host(a[1])))) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 3;
}
void dev_create_pixel_shader(X86* c) { dev_create_shader<true>(c); }
void dev_create_vertex_shader(X86* c) { dev_create_shader<false>(c); }
std::atomic<uint32_t> g_cur_ps{0};
void dev_set_pixel_shader(X86* c) {
    const Args a{c};
    g_cur_ps = a[1];
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    static const bool log = std::getenv("DIRT2_LOG_LOCKS") != nullptr;
    if (log) rt::log("d3d9: SetPixelShader(%08X)", a[1]);
    c->eax = self ? uint32_t(self->SetPixelShader(static_cast<IDirect3DPixelShader9*>(to_host(a[1])))) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 3;
}

// Debug: DIRT2_WATCH_PS=<proxy hex> logs draws made with that pixel shader,
// with the stream-0 vertex data (first 3 vertices as floats).
uint32_t watch_ps() { return g_marked_ps; }
void log_vertices(const char* what, const uint8_t* data, uint32_t stride) {
    std::string line;
    char buf[64];
    for (uint32_t v = 0; v < 3; ++v) {
        line += " |";
        for (uint32_t k = 0; k < stride / 4 && k < 10; ++k) {
            float f;
            std::memcpy(&f, data + v * stride + 4 * k, 4);
            std::snprintf(buf, sizeof buf, " %g", f);
            line += buf;
        }
    }
    rt::log("d3d9: %s stride %u:%s", what, stride, line.c_str());
}
void dev_draw_primitive_up(X86* c) {  // (type, count, data, stride)
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    if (watch_ps() && g_cur_ps == watch_ps()) log_vertices("DrawPrimitiveUP", rt::host_ptr(a[3]), a[4]);
    c->eax = self ? uint32_t(self->DrawPrimitiveUP(D3DPRIMITIVETYPE(a[1]), a[2], rt::host_ptr(a[3]), a[4]))
                  : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 6;
}
void dev_draw_primitive(X86* c) {  // (type, start vertex, count)
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    if (self && watch_ps() && g_cur_ps == watch_ps()) {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        self->GetStreamSource(0, &vb, &off, &stride);
        uint32_t proxy = 0;
        {
            std::lock_guard lk(g_mu);
            if (auto it = g_by_host.find(vb); it != g_by_host.end()) proxy = it->second;
        }
        auto sh = g_shadows.find(proxy);
        rt::log("d3d9: DrawPrimitive type %u start %u count %u vb %08X off %u stride %u vs %08X", a[1], a[2], a[3], proxy, off,
                stride, uint32_t(g_cur_vs));
        float vc[4 * 16];
        self->GetVertexShaderConstantF(0, vc, 16);
        std::string cs;
        char cb[64];
        for (int r = 0; r < 16; ++r) {
            std::snprintf(cb, sizeof cb, " c%d(%g %g %g %g)", r, vc[4 * r], vc[4 * r + 1], vc[4 * r + 2], vc[4 * r + 3]);
            cs += cb;
        }
        rt::log("  vs consts:%s", cs.c_str());
        float pc[8];
        self->GetPixelShaderConstantF(0, pc, 2);
        rt::log("  ps consts: c0(%g %g %g %g) c1(%g %g %g %g)", pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7]);
        {
            std::string rs;
            char b2[48];
            for (DWORD k = 7; k <= 209; ++k) {
                DWORD v = 0;
                if (SUCCEEDED(self->GetRenderState(D3DRENDERSTATETYPE(k), &v))) {
                    std::snprintf(b2, sizeof b2, " %lu=%lX", (unsigned long)k, (unsigned long)v);
                    rs += b2;
                }
            }
            rt::log("  render states:%s", rs.c_str());
            for (DWORD st = 0; st < 3; ++st) {
                std::string ss;
                for (DWORD k = 1; k <= 13; ++k) {
                    DWORD v = 0;
                    self->GetSamplerState(st, D3DSAMPLERSTATETYPE(k), &v);
                    std::snprintf(b2, sizeof b2, " %lu=%lX", (unsigned long)k, (unsigned long)v);
                    ss += b2;
                }
                rt::log("  sampler %lu states:%s", (unsigned long)st, ss.c_str());
            }
            IDirect3DSurface9* rt0 = nullptr;
            if (SUCCEEDED(self->GetRenderTarget(0, &rt0)) && rt0) {
                D3DSURFACE_DESC d{};
                rt0->GetDesc(&d);
                rt::log("  rt0 %ux%u fmt %u ms %u", d.Width, d.Height, d.Format, d.MultiSampleType);
                rt0->Release();
            }
        }
        for (DWORD st = 0; st < 3; ++st) {
            IDirect3DBaseTexture9* bt = nullptr;
            self->GetTexture(st, &bt);
            uint32_t tp = 0;
            {
                std::lock_guard lk(g_mu);
                if (auto it = g_by_host.find(bt); it != g_by_host.end()) tp = it->second;
            }
            DWORD au = 0, av = 0, mn = 0, mg = 0, mp = 0, srgb = 0;
            self->GetSamplerState(st, D3DSAMP_ADDRESSU, &au);
            self->GetSamplerState(st, D3DSAMP_ADDRESSV, &av);
            self->GetSamplerState(st, D3DSAMP_MINFILTER, &mn);
            self->GetSamplerState(st, D3DSAMP_MAGFILTER, &mg);
            self->GetSamplerState(st, D3DSAMP_MIPFILTER, &mp);
            self->GetSamplerState(st, D3DSAMP_SRGBTEXTURE, &srgb);
            uint64_t sum = 0;
            if (bt && bt->GetType() == D3DRTYPE_TEXTURE) {
                auto* t2 = static_cast<IDirect3DTexture9*>(bt);
                D3DSURFACE_DESC d{};
                t2->GetLevelDesc(0, &d);
                D3DLOCKED_RECT lr{};
                if (SUCCEEDED(t2->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
                    const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + (d.Height / 4) * lr.Pitch;
                    for (UINT x = 0; x < d.Width / 2; ++x) sum += row[x];
                    t2->UnlockRect(0);
                }
            }
            rt::log("  stage %u: tex %08X sum %llu addr %u/%u filt %u/%u/%u srgb %u", st, tp, (unsigned long long)sum, au, av, mn,
                    mg, mp, srgb);
            if (bt && bt->GetType() == D3DRTYPE_TEXTURE && st == 0) {
                // GPU-side content: StretchRect the level into a render target and read that back.
                auto* t2 = static_cast<IDirect3DTexture9*>(bt);
                D3DSURFACE_DESC d{};
                t2->GetLevelDesc(0, &d);
                IDirect3DSurface9 *src = nullptr, *rtgt = nullptr, *sys = nullptr;
                t2->GetSurfaceLevel(0, &src);
                HRESULT h1 = self->CreateRenderTarget(256, 256, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rtgt, nullptr);
                RECT sr{0, 0, LONG(d.Width / 2), LONG(d.Height / 2)};
                HRESULT h2 = src && rtgt ? self->StretchRect(src, &sr, rtgt, nullptr, D3DTEXF_POINT) : E_FAIL;
                HRESULT h3 = self->CreateOffscreenPlainSurface(256, 256, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
                HRESULT h4 = rtgt && sys ? self->GetRenderTargetData(rtgt, sys) : E_FAIL;
                uint64_t gsum = 0;
                D3DLOCKED_RECT lr{};
                if (SUCCEEDED(h4) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                    for (int x = 0; x < 256; ++x) gsum += static_cast<const uint8_t*>(lr.pBits)[128 * lr.Pitch + x * 4 + 2];
                    sys->UnlockRect();
                }
                rt::log("  stage 0 GPU copy: hr %08X %08X %08X %08X red-row sum %llu", uint32_t(h1), uint32_t(h2), uint32_t(h3),
                        uint32_t(h4), (unsigned long long)gsum);
                // Control: a fresh texture created and filled right here, probed the same way.
                IDirect3DTexture9* fresh = nullptr;
                if (SUCCEEDED(self->CreateTexture(2048, 1024, 1, D3DUSAGE_DYNAMIC, D3DFMT_L8, D3DPOOL_DEFAULT, &fresh, nullptr))) {
                    D3DLOCKED_RECT fl{};
                    fresh->LockRect(0, &fl, nullptr, D3DLOCK_DISCARD);
                    for (int y = 0; y < 1024; ++y) std::memset(static_cast<uint8_t*>(fl.pBits) + y * fl.Pitch, 200, 2048);
                    fresh->UnlockRect(0);
                    IDirect3DSurface9 *fs = nullptr, *frt = nullptr, *fsys = nullptr;
                    fresh->GetSurfaceLevel(0, &fs);
                    self->CreateRenderTarget(256, 256, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &frt, nullptr);
                    self->StretchRect(fs, &sr, frt, nullptr, D3DTEXF_POINT);
                    self->CreateOffscreenPlainSurface(256, 256, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &fsys, nullptr);
                    self->GetRenderTargetData(frt, fsys);
                    D3DLOCKED_RECT fr{};
                    uint32_t px = 0;
                    if (SUCCEEDED(fsys->LockRect(&fr, nullptr, D3DLOCK_READONLY))) {
                        px = static_cast<const uint32_t*>(fr.pBits)[128 * fr.Pitch / 4 + 128];
                        fsys->UnlockRect();
                    }
                    rt::log("  control texture GPU copy pixel %08X (expect FFC8C8C8)", px);
                    fs->Release();
                    frt->Release();
                    fsys->Release();
                    fresh->Release();
                }
                if (src) src->Release();
                if (rtgt) rtgt->Release();
                if (sys) sys->Release();
            }
            if (bt) bt->Release();
        }
        IDirect3DVertexDeclaration9* decl = nullptr;
        if (SUCCEEDED(self->GetVertexDeclaration(&decl)) && decl) {
            D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
            UINT n = MAXD3DDECLLENGTH + 1;
            decl->GetDeclaration(el, &n);
            for (UINT i = 0; i + 1 < n; ++i) {
                rt::log("  decl stream %u off %u type %u usage %u/%u", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage,
                        el[i].UsageIndex);
                IDirect3DVertexBuffer9* sb = nullptr;
                UINT so = 0, ss = 0;
                self->GetStreamSource(el[i].Stream, &sb, &so, &ss);
                uint32_t sp = 0;
                {
                    std::lock_guard lk(g_mu);
                    if (auto it = g_by_host.find(sb); it != g_by_host.end()) sp = it->second;
                }
                if (auto sh2 = g_shadows.find(sp); sh2 != g_shadows.end())
                    log_vertices("    stream data", rt::host_ptr(sh2->second.guest + so + el[i].Offset), ss);
                else
                    rt::log("    stream %u: vb %08X off %u stride %u (no shadow)", el[i].Stream, sp, so, ss);
                if (sb) sb->Release();
            }
            decl->Release();
        }
        if (sh != g_shadows.end()) log_vertices("  vb shadow", rt::host_ptr(sh->second.guest + off + a[2] * stride), stride);
        if (vb) vb->Release();
    }
    c->eax = self ? uint32_t(self->DrawPrimitive(D3DPRIMITIVETYPE(a[1]), a[2], a[3])) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 5;
}

void dev_draw_indexed_primitive(X86* c) {  // (type, base vertex, min index, num vertices, start index, prim count)
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    if (self && watch_ps() && g_cur_ps == watch_ps()) {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        self->GetStreamSource(0, &vb, &off, &stride);
        uint32_t proxy = 0;
        {
            std::lock_guard lk(g_mu);
            if (auto it = g_by_host.find(vb); it != g_by_host.end()) proxy = it->second;
        }
        rt::log("d3d9: DrawIndexedPrimitive type %u base %d min %u n %u start %u count %u vb %08X off %u stride %u", a[1],
                int32_t(a[2]), a[3], a[4], a[5], a[6], proxy, off, stride);
        auto sh = g_shadows.find(proxy);
        if (sh != g_shadows.end())
            log_vertices("  vb shadow", rt::host_ptr(sh->second.guest + off + (int32_t(a[2]) + a[3]) * stride), stride);
        if (vb) vb->Release();
    }
    c->eax = self ? uint32_t(self->DrawIndexedPrimitive(D3DPRIMITIVETYPE(a[1]), INT(a[2]), a[3], a[4], a[5], a[6]))
                  : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 8;
}

// CreateTexture(w, h, levels, usage, format, pool, **tex, *shared): logged for dynamic textures when DIRT2_LOG_LOCKS.
void dev_create_texture(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    IDirect3DTexture9* t = nullptr;
    const HRESULT hr = self ? self->CreateTexture(a[1], a[2], a[3], a[4], D3DFORMAT(a[5]), D3DPOOL(a[6]), &t, nullptr)
                            : D3DERR_INVALIDCALL;
    const uint32_t proxy = SUCCEEDED(hr) ? wrap(t) : 0;
    if (a[7]) wr32(a[7], proxy);
    if (a[8]) wr32(a[8], 0);
    static const bool log = std::getenv("DIRT2_LOG_LOCKS") != nullptr;
    if (log && (a[4] & D3DUSAGE_DYNAMIC))
        rt::log("d3d9: CreateTexture %ux%u levels %u usage %X fmt %u pool %u shared %08X -> %08X hr %08X (thread %u)", a[1], a[2],
                a[3], a[4], a[5], a[6], a[8], proxy, uint32_t(hr), rt::current_thread_id());
    c->eax = uint32_t(hr);
    c->esp += 4 * 10;
}

// CreateVertexBuffer(length, usage, fvf, pool, **vb, shared) and
// CreateIndexBuffer(length, usage, format, pool, **ib, shared). The game does
// not check these, so a failure becomes a call through a null vtable: log it.
void dev_create_vertex_buffer(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    IDirect3DVertexBuffer9* vb = nullptr;
    const HRESULT hr = self ? self->CreateVertexBuffer(a[1], a[2], a[3], D3DPOOL(a[4]), &vb, nullptr) : D3DERR_INVALIDCALL;
    if (a[5]) wr32(a[5], SUCCEEDED(hr) ? wrap(vb) : 0);
    if (a[6]) wr32(a[6], 0);
    if (FAILED(hr))
        rt::log("d3d9: CreateVertexBuffer(%u bytes, usage %X, fvf %X, pool %u) failed: %08X (caller %08X)", a[1], a[2],
                a[3], a[4], uint32_t(hr), rd32(c->esp));
    c->eax = uint32_t(hr);
    c->esp += 4 * 8;
}

void dev_create_index_buffer(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    IDirect3DIndexBuffer9* ib = nullptr;
    const HRESULT hr = self ? self->CreateIndexBuffer(a[1], a[2], D3DFORMAT(a[3]), D3DPOOL(a[4]), &ib, nullptr)
                            : D3DERR_INVALIDCALL;
    if (a[5]) wr32(a[5], SUCCEEDED(hr) ? wrap(ib) : 0);
    if (a[6]) wr32(a[6], 0);
    if (FAILED(hr))
        rt::log("d3d9: CreateIndexBuffer(%u bytes, usage %X, fmt %u, pool %u) failed: %08X (caller %08X)", a[1], a[2],
                a[3], a[4], uint32_t(hr), rd32(c->esp));
    c->eax = uint32_t(hr);
    c->esp += 4 * 8;
}

void d3d_get_adapter_monitor(X86* c) {
    c->eax = 0x00010001;
    c->esp += 4 * 3;
}

// Debug: DIRT2_SCREENSHOT_DIR=<dir> [DIRT2_SCREENSHOT_EVERY=<n>] saves the back
// buffer as BMP every n presents; frame rate is logged every 300 presents.
void capture_back_buffer(IDirect3DDevice9* dev, uint32_t frame) {
    static const char* dir = std::getenv("DIRT2_SCREENSHOT_DIR");
    static const uint32_t every = std::getenv("DIRT2_SCREENSHOT_EVERY") ? std::atoi(std::getenv("DIRT2_SCREENSHOT_EVERY")) : 120;
    static const uint32_t from = std::getenv("DIRT2_SCREENSHOT_FROM") ? std::atoi(std::getenv("DIRT2_SCREENSHOT_FROM")) : 0;
    static const uint32_t count = std::getenv("DIRT2_SCREENSHOT_COUNT") ? std::atoi(std::getenv("DIRT2_SCREENSHOT_COUNT")) : 0;
    if (!dir || !every || frame < from || frame % every) return;
    if (count && frame >= from + count * every) return;
    static const bool made = [] {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return true;
    }();
    (void)made;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/frame_%06u.bmp", dir, frame);
            if (FILE* f = std::fopen(path, "wb")) {
                const uint32_t row = d.Width * 4, size = row * d.Height;
                uint8_t hdr[54] = {'B', 'M'};
                auto put32 = [&](int off, uint32_t v) { std::memcpy(hdr + off, &v, 4); };
                put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, d.Width);
                put32(22, uint32_t(-int32_t(d.Height)));  // top-down
                hdr[26] = 1; hdr[28] = 32; put32(34, size);
                std::fwrite(hdr, 1, 54, f);
                for (uint32_t y = 0; y < d.Height; ++y) {
                    const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
                    std::vector<uint8_t> line(src, src + row);
                    for (uint32_t x = 0; x < d.Width; ++x) line[4 * x + 3] = 0xFF;
                    std::fwrite(line.data(), 1, row, f);
                }
                std::fclose(f);
                rt::log("d3d9: saved %s (%ux%u fmt %u)", path, d.Width, d.Height, d.Format);
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
}

// Present(src, dst, hDestWindowOverride, dirty): the override window is ignored.
std::string str_rect(uint32_t r) {
    char b[64];
    std::snprintf(b, sizeof b, "(%d,%d)-(%d,%d)", int32_t(rd32(r)), int32_t(rd32(r + 4)), int32_t(rd32(r + 8)),
                  int32_t(rd32(r + 12)));
    return b;
}

void dev_present(X86* c) {
    platform::gpu_checkpoint();  // no GPU work while the app is in the background
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    static std::atomic<uint32_t> frames{0};
    static auto t0 = std::chrono::steady_clock::now();
    const uint32_t frame = ++frames;
    if (self) capture_back_buffer(self, frame);
    if (frame == 1) rt::log("d3d9: first Present");
    static const bool log_present = std::getenv("DIRT2_LOG_PRESENT") != nullptr;
    if (log_present) {
        static std::mutex mu;
        static std::map<uint32_t, uint32_t> per_thread;
        std::lock_guard lk(mu);
        ++per_thread[rt::current_thread_id()];
        if (frame <= 40) {
            auto rect = [&](uint32_t r) {
                return r ? str_rect(r) : std::string("-");
            };
            rt::log("d3d9: Present #%u t%u caller %08X src %s dst %s hwnd 0x%X dirty %08X", frame,
                    rt::current_thread_id(), rd32(c->esp), rect(a[1]).c_str(), rect(a[2]).c_str(), a[3], a[4]);
        }
        if (frame % 300 == 0) {
            std::string line;
            for (auto& [t, n] : per_thread) line += " t" + std::to_string(t) + "=" + std::to_string(n);
            rt::log("d3d9: Present by thread:%s (caller %08X)", line.c_str(), rd32(c->esp));
            per_thread.clear();
        }
    }
    if (frame % 300 == 0) {
        const auto now = std::chrono::steady_clock::now();
        rt::log("d3d9: %u presents, %.1f fps", frame, 300.0 / std::chrono::duration<double>(now - t0).count());
        t0 = now;
    }
    const HRESULT hr = self ? self->Present(a.ptr<const RECT>(1), a.ptr<const RECT>(2), nullptr,
                                            a.ptr<const RGNDATA>(4))
                            : D3DERR_INVALIDCALL;
    c->eax = uint32_t(hr);
    c->esp += 4 * 6;
}
void swap_present(X86* c) {
    platform::gpu_checkpoint();
    const Args a{c};
    if (std::getenv("DIRT2_LOG_PRESENT")) rt::log("d3d9: SwapChain::Present by t%u (caller %08X)", rt::current_thread_id(), rd32(c->esp));
    auto* self = static_cast<IDirect3DSwapChain9*>(to_host(a[0]));
    const HRESULT hr = self ? self->Present(a.ptr<const RECT>(1), a.ptr<const RECT>(2), nullptr,
                                            a.ptr<const RGNDATA>(4), a[5])
                            : D3DERR_INVALIDCALL;
    c->eax = uint32_t(hr);
    c->esp += 4 * 7;
}

// ---- locking ----

struct FormatInfo {
    uint32_t bw, bh, bytes;  // block size and bytes per block
};

FormatInfo format_info(D3DFORMAT f) {
    switch (uint32_t(f)) {
        case D3DFMT_DXT1: case MAKEFOURCC('A', 'T', 'I', '1'): return {4, 4, 8};
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        case MAKEFOURCC('A', 'T', 'I', '2'): return {4, 4, 16};
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: return {1, 1, 1};
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_V8U8: case D3DFMT_R16F:
        case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_A8R3G3B2: case D3DFMT_L6V5U5:
            return {1, 1, 2};
        case D3DFMT_R8G8B8: return {1, 1, 3};
        case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
            return {1, 1, 8};
        case D3DFMT_A32B32G32R32F: return {1, 1, 16};
        default: return {1, 1, 4};
    }
}

struct Staging {
    uint32_t guest = 0;
    uint8_t* host = nullptr;
    uint32_t row_pitch = 0, slice_pitch = 0;
    uint32_t rows = 0, slices = 1, row_bytes = 0;
    bool readonly = false;
    bool persistent = false;  // guest buffer outlives the lock (SYSTEMMEM)
    // DISCARD locks: the host resource is not kept locked while the game
    // writes; commit() locks it (DISCARD), copies the staging in and unlocks.
    std::function<HRESULT(const Staging&)> commit;
};

std::map<std::pair<uint32_t, uint32_t>, Staging> g_locks;  // (proxy, subresource) -> staging

// SYSTEMMEM resources: Windows keeps their memory mapped, and games rely on
// the locked pointer staying valid after UnlockRect (DiRT 2 hands it to Bink
// as a video plane and writes into it every frame). Each subresource gets a
// persistent guest shadow returned by every lock; the shadow is copied into
// the host resource at unlock and before UpdateTexture/UpdateSurface reads it.
struct Shadow {
    uint32_t guest = 0, size = 0;
    uint32_t row_pitch = 0, rows = 0, row_bytes = 0;
    std::function<void(const Shadow&)> flush;  // locks the host subresource and copies the shadow in
};
std::map<std::pair<uint32_t, uint32_t>, Shadow> g_shadows_sys;

void flush_shadows(uint32_t proxy) {
    std::vector<Shadow> todo;
    {
        std::lock_guard lk(g_mu);
        for (auto it = g_shadows_sys.lower_bound({proxy, 0}); it != g_shadows_sys.end() && it->first.first == proxy; ++it)
            if (!g_locks.count(it->first)) todo.push_back(it->second);  // a locked one is flushed at its unlock
    }
    for (const Shadow& sh : todo) sh.flush(sh);
}

void drop_shadows(uint32_t proxy) {
    std::lock_guard lk(g_mu);
    for (auto it = g_shadows_sys.lower_bound({proxy, 0}); it != g_shadows_sys.end() && it->first.first == proxy;) {
        rt::heap_free(it->second.guest);
        it = g_shadows_sys.erase(it);
    }
}

void copy_shadow_in(const Shadow& sh, void* host_bits, uint32_t host_pitch) {
    for (uint32_t y = 0; y < sh.rows; ++y)
        std::memcpy(static_cast<uint8_t*>(host_bits) + size_t(y) * host_pitch, rt::host_ptr(sh.guest + y * sh.row_pitch),
                    std::min(sh.row_bytes, host_pitch));
}

void copy_rows(uint8_t* dst, const uint8_t* src, const Staging& s) {
    for (uint32_t z = 0; z < s.slices; ++z)
        for (uint32_t y = 0; y < s.rows; ++y) {
            const size_t off = size_t(z) * s.slice_pitch + size_t(y) * s.row_pitch;
            std::memcpy(dst + off, src + off, s.row_bytes);
        }
}

uint32_t begin_staging(Staging& s, void* host_bits, DWORD flags) {
    s.host = static_cast<uint8_t*>(host_bits);
    s.readonly = (flags & D3DLOCK_READONLY) != 0;
    const uint32_t size = (s.slices - 1) * s.slice_pitch + (s.rows - 1) * s.row_pitch + s.row_bytes;
    if (!s.persistent) s.guest = rt::heap_alloc(size);
    if (!(flags & D3DLOCK_DISCARD)) copy_rows(rt::host_ptr(s.guest), s.host, s);
    return s.guest;
}

void end_staging(const Staging& s) {
    if (!s.readonly) copy_rows(s.host, rt::host_ptr(s.guest), s);
    if (!s.persistent) rt::heap_free(s.guest);
}

// Shape of a locked rectangle on a level with the given description.
void shape_rect(Staging& s, const D3DSURFACE_DESC& d, const RECT* r, uint32_t pitch) {
    const FormatInfo fi = format_info(d.Format);
    const uint32_t w = r ? uint32_t(r->right - r->left) : d.Width;
    const uint32_t h = r ? uint32_t(r->bottom - r->top) : d.Height;
    s.row_pitch = pitch;
    s.rows = (h + fi.bh - 1) / fi.bh;
    s.row_bytes = std::min(pitch, (w + fi.bw - 1) / fi.bw * fi.bytes);
    s.slices = 1;
    s.slice_pitch = 0;
    if (!s.rows) s.rows = 1;
}

template <typename LockFn, typename UnlockFn>
HRESULT lock_rect(uint32_t proxy, uint32_t sub, uint32_t guest_lr, const D3DSURFACE_DESC& desc, const RECT* rect,
                  DWORD flags, LockFn&& lock, UnlockFn&& unlock) {
    D3DLOCKED_RECT lr{};
    const HRESULT hr = lock(&lr, flags);
    if (FAILED(hr)) return hr;
    Staging s;
    shape_rect(s, desc, rect, uint32_t(lr.Pitch));
    if ((flags & D3DLOCK_DISCARD) && desc.Pool != D3DPOOL_SYSTEMMEM) {
        // Release the host lock straight away and upload in one step at unlock:
        // DXVK renames the mapping on every DISCARD lock, so a pointer held across
        // the game's lock span (while other threads use the device) can go stale.
        unlock();
        s.guest = rt::heap_alloc((s.slices - 1) * s.slice_pitch + (s.rows - 1) * s.row_pitch + s.row_bytes);
        s.commit = [lock, unlock](const Staging& st) mutable {
            D3DLOCKED_RECT l{};
            HRESULT h = lock(&l, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK);
            if (FAILED(h)) return h;
            for (uint32_t y = 0; y < st.rows; ++y)
                std::memcpy(static_cast<uint8_t*>(l.pBits) + size_t(y) * l.Pitch, rt::host_ptr(st.guest + y * st.row_pitch),
                            std::min<uint32_t>(st.row_bytes, uint32_t(l.Pitch)));
            return unlock();
        };
        wr32(guest_lr, uint32_t(lr.Pitch));
        wr32(guest_lr + 4, s.guest);
        std::lock_guard lk(g_mu);
        g_locks[{proxy, sub}] = std::move(s);
        return hr;
    }
    if (desc.Pool == D3DPOOL_SYSTEMMEM && !rect) {
        // Whole-level lock of a SYSTEMMEM resource: persistent shadow.
        std::lock_guard lk(g_mu);
        Shadow& sh = g_shadows_sys[{proxy, sub}];
        const uint32_t size = (s.rows - 1) * s.row_pitch + s.row_bytes;
        if (!sh.guest || sh.size < size) {
            if (sh.guest) rt::heap_free(sh.guest);
            sh.guest = rt::heap_alloc(size, true);
            sh.size = size;
        }
        sh.row_pitch = s.row_pitch;
        sh.rows = s.rows;
        sh.row_bytes = s.row_bytes;
        sh.flush = [lock, unlock](const Shadow& me) mutable {
            D3DLOCKED_RECT l{};
            if (SUCCEEDED(lock(&l, 0))) {
                copy_shadow_in(me, l.pBits, uint32_t(l.Pitch));
                unlock();
            }
        };
        s.guest = sh.guest;
        s.persistent = true;
    }
    const uint32_t g = begin_staging(s, lr.pBits, flags);
    if (std::getenv("DIRT2_LOG_LUM") && desc.Format == D3DFMT_A16B16G16R16F && desc.Width == 1 && desc.Height == 1) {
        uint16_t h[4];
        std::memcpy(h, rt::host_ptr(g), 8);
        auto f16 = [](uint16_t v) {
            const uint32_t sgn = v >> 15, e = (v >> 10) & 31, m = v & 1023;
            float r = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 1024), int(e) - 25);
            return sgn ? -r : r;
        };
        rt::log("d3d9: luminance readback %08X: %g %g %g %g", proxy, f16(h[0]), f16(h[1]), f16(h[2]), f16(h[3]));
    }
    static const bool log_locks = std::getenv("DIRT2_LOG_LOCKS") != nullptr;
    if (log_locks)
        rt::log("d3d9: lock %08X/%X %ux%u fmt %u pool %u usage %X flags %X -> %08X pitch %u%s", proxy, sub, desc.Width,
                desc.Height, desc.Format, desc.Pool, desc.Usage, flags, g, s.row_pitch, s.persistent ? " (persistent)" : "");
    wr32(guest_lr, uint32_t(lr.Pitch));
    wr32(guest_lr + 4, g);
    std::lock_guard lk(g_mu);
    g_locks[{proxy, sub}] = s;
    return hr;
}

template <typename UnlockFn>
HRESULT unlock_sub(uint32_t proxy, uint32_t sub, UnlockFn&& unlock) {
    {
        std::unique_lock lk(g_mu);
        auto it = g_locks.find({proxy, sub});
        if (it != g_locks.end() && it->second.commit) {
            Staging st = std::move(it->second);
            g_locks.erase(it);
            lk.unlock();
            const HRESULT h = st.commit(st);
            rt::heap_free(st.guest);
            return h;
        }
        if (it != g_locks.end()) {
            if (std::getenv("DIRT2_LOG_LOCKS") && it->second.row_pitch >= 1024) {
                const Staging& st = it->second;
                uint64_t sum = 0;
                for (uint32_t x = 0; x < st.row_bytes; ++x) sum += rd8(st.guest + (st.rows / 4) * st.row_pitch + x);
                rt::log("d3d9: unlock %08X/%X staging row sum %llu", proxy, sub, (unsigned long long)sum);
            }
            end_staging(it->second);
            g_locks.erase(it);
        }
    }
    return unlock();
}

// IDirect3DSurface9::LockRect(D3DLOCKED_RECT*, const RECT*, DWORD) / UnlockRect()
void surf_lock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DSurface9*>(to_host(a[0]));
    HRESULT hr = D3DERR_INVALIDCALL;
    if (self) {
        D3DSURFACE_DESC d{};
        self->GetDesc(&d);
        const RECT* r = a.ptr<const RECT>(2);
        hr = lock_rect(a[0], 0, a[1], d, r, a[3],
                       [self, r](D3DLOCKED_RECT* lr, DWORD fl) { return self->LockRect(lr, r, fl); },
                       [self] { return self->UnlockRect(); });
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 5;
}
void surf_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DSurface9*>(to_host(a[0]));
    c->eax = self ? uint32_t(unlock_sub(a[0], 0, [&] { return self->UnlockRect(); })) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 2;
}

// IDirect3DTexture9::LockRect(level, D3DLOCKED_RECT*, const RECT*, DWORD) / UnlockRect(level)
void tex_lock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DTexture9*>(to_host(a[0]));
    HRESULT hr = D3DERR_INVALIDCALL;
    if (self) {
        D3DSURFACE_DESC d{};
        self->GetLevelDesc(a[1], &d);
        const RECT* r = a.ptr<const RECT>(3);
        const UINT level = a[1];
        hr = lock_rect(a[0], level, a[2], d, r, a[4],
                       [self, level, r](D3DLOCKED_RECT* lr, DWORD fl) { return self->LockRect(level, lr, r, fl); },
                       [self, level] { return self->UnlockRect(level); });
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 6;
}
void tex_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DTexture9*>(to_host(a[0]));
    c->eax = self ? uint32_t(unlock_sub(a[0], a[1], [&] { return self->UnlockRect(a[1]); })) : uint32_t(D3DERR_INVALIDCALL);
    static int dbg = 0;
    if (self && std::getenv("DIRT2_LOG_LOCKS") && dbg < 12) {
        D3DSURFACE_DESC d{};
        self->GetLevelDesc(a[1], &d);
        if (d.Format == D3DFMT_L8 && d.Width >= 1024) {
            ++dbg;
            D3DLOCKED_RECT lr{};
            const HRESULT hr = self->LockRect(a[1], &lr, nullptr, D3DLOCK_READONLY);
            uint64_t sum = 0;
            if (SUCCEEDED(hr)) {
                const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + (d.Height / 4) * lr.Pitch;
                for (uint32_t x = 0; x < std::min<uint32_t>(lr.Pitch, d.Width); ++x) sum += row[x];
                self->UnlockRect(a[1]);
            }
            rt::log("d3d9: readback %08X hr %08X row sum %llu levels %u usage %X pool %u", a[0], uint32_t(hr), (unsigned long long)sum, self->GetLevelCount(), d.Usage, d.Pool);
        }
    }
    c->esp += 4 * 3;
}

// IDirect3DCubeTexture9::LockRect(face, level, D3DLOCKED_RECT*, const RECT*, DWORD) / UnlockRect(face, level)
void cube_lock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DCubeTexture9*>(to_host(a[0]));
    HRESULT hr = D3DERR_INVALIDCALL;
    if (self) {
        D3DSURFACE_DESC d{};
        self->GetLevelDesc(a[2], &d);
        const RECT* r = a.ptr<const RECT>(4);
        const D3DCUBEMAP_FACES face = D3DCUBEMAP_FACES(a[1]);
        const UINT level = a[2];
        hr = lock_rect(a[0], (a[1] << 16) | a[2], a[3], d, r, a[5],
                       [self, face, level, r](D3DLOCKED_RECT* lr, DWORD fl) { return self->LockRect(face, level, lr, r, fl); },
                       [self, face, level] { return self->UnlockRect(face, level); });
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 7;
}
void cube_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DCubeTexture9*>(to_host(a[0]));
    c->eax = self ? uint32_t(unlock_sub(a[0], (a[1] << 16) | a[2],
                                        [&] { return self->UnlockRect(D3DCUBEMAP_FACES(a[1]), a[2]); }))
                  : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 4;
}

// Volumes: D3DLOCKED_BOX { RowPitch, SlicePitch, pBits }.
template <typename LockFn>
HRESULT lock_box(uint32_t proxy, uint32_t sub, uint32_t guest_lb, const D3DVOLUME_DESC& d, const D3DBOX* box, DWORD flags,
                 LockFn&& lock) {
    D3DLOCKED_BOX lb{};
    const HRESULT hr = lock(&lb);
    if (FAILED(hr)) return hr;
    const FormatInfo fi = format_info(d.Format);
    Staging s;
    const uint32_t w = box ? box->Right - box->Left : d.Width;
    const uint32_t h = box ? box->Bottom - box->Top : d.Height;
    const uint32_t depth = box ? box->Back - box->Front : d.Depth;
    s.row_pitch = uint32_t(lb.RowPitch);
    s.slice_pitch = uint32_t(lb.SlicePitch);
    s.rows = std::max(1u, (h + fi.bh - 1) / fi.bh);
    s.row_bytes = std::min(s.row_pitch, (w + fi.bw - 1) / fi.bw * fi.bytes);
    s.slices = std::max(1u, depth);
    const uint32_t g = begin_staging(s, lb.pBits, flags);
    wr32(guest_lb, s.row_pitch);
    wr32(guest_lb + 4, s.slice_pitch);
    wr32(guest_lb + 8, g);
    std::lock_guard lk(g_mu);
    g_locks[{proxy, sub}] = s;
    return hr;
}
void vol_lock(X86* c) {  // IDirect3DVolume9::LockBox(D3DLOCKED_BOX*, const D3DBOX*, DWORD)
    const Args a{c};
    auto* self = static_cast<IDirect3DVolume9*>(to_host(a[0]));
    HRESULT hr = D3DERR_INVALIDCALL;
    if (self) {
        D3DVOLUME_DESC d{};
        self->GetDesc(&d);
        const D3DBOX* b = a.ptr<const D3DBOX>(2);
        hr = lock_box(a[0], 0, a[1], d, b, a[3], [&](D3DLOCKED_BOX* lb) { return self->LockBox(lb, b, a[3]); });
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 5;
}
void vol_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DVolume9*>(to_host(a[0]));
    c->eax = self ? uint32_t(unlock_sub(a[0], 0, [&] { return self->UnlockBox(); })) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 2;
}
void voltex_lock(X86* c) {  // IDirect3DVolumeTexture9::LockBox(level, D3DLOCKED_BOX*, const D3DBOX*, DWORD)
    const Args a{c};
    auto* self = static_cast<IDirect3DVolumeTexture9*>(to_host(a[0]));
    HRESULT hr = D3DERR_INVALIDCALL;
    if (self) {
        D3DVOLUME_DESC d{};
        self->GetLevelDesc(a[1], &d);
        const D3DBOX* b = a.ptr<const D3DBOX>(3);
        hr = lock_box(a[0], a[1], a[2], d, b, a[4], [&](D3DLOCKED_BOX* lb) { return self->LockBox(a[1], lb, b, a[4]); });
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 6;
}
void voltex_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DVolumeTexture9*>(to_host(a[0]));
    c->eax = self ? uint32_t(unlock_sub(a[0], a[1], [&] { return self->UnlockBox(a[1]); })) : uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 3;
}


template <typename B, typename Desc>
void buf_lock(X86* c) {  // Lock(offset, size, void** ppbData, flags)
    const Args a{c};
    auto* self = static_cast<B*>(to_host(a[0]));
    HRESULT hr = D3DERR_INVALIDCALL;
    if (self) {
        Desc d{};
        self->GetDesc(&d);
        void* host = nullptr;
        hr = self->Lock(a[1], a[2], &host, a[4]);
        if (SUCCEEDED(hr)) {
            std::lock_guard lk(g_mu);
            BufferShadow& sh = g_shadows[a[0]];
            if (!sh.guest) {
                sh.size = d.Size;
                sh.guest = rt::heap_alloc(d.Size ? d.Size : 16, true);
            }
            const uint32_t off = std::min(a[1], sh.size);
            const uint32_t size = a[2] ? std::min(a[2], sh.size - off) : sh.size - off;
            const bool ro = (a[4] & D3DLOCK_READONLY) != 0;
            if (ro) std::memcpy(rt::host_ptr(sh.guest + off), host, size);
            sh.locks.push_back({off, size, static_cast<uint8_t*>(host), ro});
            wr32(a[3], sh.guest + off);
        }
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 6;
}
template <typename B>
void buf_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<B*>(to_host(a[0]));
    if (!self) {
        c->eax = uint32_t(D3DERR_INVALIDCALL);
        c->esp += 8;
        return;
    }
    {
        std::lock_guard lk(g_mu);
        auto it = g_shadows.find(a[0]);
        if (it != g_shadows.end() && !it->second.locks.empty()) {
            const auto l = it->second.locks.back();
            it->second.locks.pop_back();
            if (!l.readonly) std::memcpy(l.host, rt::host_ptr(it->second.guest + l.offset), l.size);
        }
    }
    c->eax = uint32_t(self->Unlock());
    c->esp += 8;
}

void vb_lock(X86* c) { buf_lock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
void ib_lock(X86* c) { buf_lock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
void vb_unlock(X86* c) { buf_unlock<IDirect3DVertexBuffer9>(c); }
void ib_unlock(X86* c) { buf_unlock<IDirect3DIndexBuffer9>(c); }

// IDirect3DSurface9::GetDC/ReleaseDC are GDI interop; not supported.
void surf_get_dc(X86* c) {
    c->eax = uint32_t(D3DERR_INVALIDCALL);
    c->esp += 4 * 3;
}


uint32_t wrap_by_iid(const GUID& iid, void* obj) {
    auto* u = static_cast<IUnknown*>(obj);
#define TRY(I) \
    if (iid == __uuidof(I)) return wrap_with(u, vtable_desc<I>());
    TRY(IDirect3DDevice9)
    TRY(IDirect3D9)
    TRY(IDirect3DTexture9)
    TRY(IDirect3DCubeTexture9)
    TRY(IDirect3DVolumeTexture9)
    TRY(IDirect3DSurface9)
    TRY(IDirect3DVolume9)
    TRY(IDirect3DVertexBuffer9)
    TRY(IDirect3DIndexBuffer9)
    TRY(IDirect3DSwapChain9)
    TRY(IDirect3DQuery9)
    TRY(IDirect3DStateBlock9)
    TRY(IDirect3DVertexDeclaration9)
    TRY(IDirect3DVertexShader9)
    TRY(IDirect3DPixelShader9)
#undef TRY
    // IUnknown / base interfaces: keep the existing proxy if there is one.
    {
        std::lock_guard lk(g_mu);
        if (auto it = g_by_host.find(u); it != g_by_host.end()) return it->second;
    }
    rt::log("d3d9: QueryInterface for an unbridged interface");
    u->Release();
    return 0;
}
}  // namespace

// ---- DLL exports ----

STDCALL(d3d9, Direct3DCreate9, 1) {
    IDirect3D9* d3d = Direct3DCreate9(a[0]);
    rt::log("d3d9: Direct3DCreate9(%u) -> %p", a[0], static_cast<void*>(d3d));
    return wrap(d3d);
}
STDCALL(d3d9, D3DPERF_SetOptions, 1) { return 0; }

namespace shim {
// Host object behind a guest D3D9 proxy (used by the D3DX shims).
void* d3d9_host(uint32_t proxy) { return to_host(proxy); }
}  // namespace shim
