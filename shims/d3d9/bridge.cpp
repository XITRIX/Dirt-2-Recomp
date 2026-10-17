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

#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

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

template <>
struct Arg<D3DPRESENT_PARAMETERS*> {
    uint32_t s;
    D3DPRESENT_PARAMETERS v{};
    uint32_t hwnd = 0;
    D3DPRESENT_PARAMETERS* get() {
        if (!s) return nullptr;
        hwnd = rd32(s + 28);
        pp_in(s, v);
        return &v;
    }
    void done() {
        if (s) pp_out(s, v, hwnd);
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
    if (self && r == 0) forget(g);
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
    if (p && !p->hDeviceWindow) p->hDeviceWindow = focus;
    rt::log("d3d9: CreateDevice %ux%u fmt %u windowed %d flags 0x%X", p ? p->BackBufferWidth : 0,
            p ? p->BackBufferHeight : 0, p ? p->BackBufferFormat : 0, p ? p->Windowed : 0, a[4]);
    const HRESULT hr = self ? self->CreateDevice(a[1], D3DDEVTYPE(a[2]), focus, a[4], p, &dev) : D3DERR_INVALIDCALL;
    pp.done();
    if (a[6]) wr32(a[6], SUCCEEDED(hr) ? wrap(dev) : 0);
    rt::log("d3d9: CreateDevice -> 0x%08X", uint32_t(hr));
    c->eax = uint32_t(hr);
    c->esp += 4 * 8;
}

// IDirect3D9::GetAdapterMonitor returns an HMONITOR.
void d3d_get_adapter_monitor(X86* c) {
    c->eax = 0x00010001;
    c->esp += 4 * 3;
}

// Present(src, dst, hDestWindowOverride, dirty): the override window is ignored.
void dev_present(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DDevice9*>(to_host(a[0]));
    const HRESULT hr = self ? self->Present(a.ptr<const RECT>(1), a.ptr<const RECT>(2), nullptr,
                                            a.ptr<const RGNDATA>(4))
                            : D3DERR_INVALIDCALL;
    c->eax = uint32_t(hr);
    c->esp += 4 * 6;
}
void swap_present(X86* c) {
    const Args a{c};
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
};

std::map<std::pair<uint32_t, uint32_t>, Staging> g_locks;  // (proxy, subresource) -> staging

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
    s.guest = rt::heap_alloc(size);
    if (!(flags & D3DLOCK_DISCARD)) copy_rows(rt::host_ptr(s.guest), s.host, s);
    return s.guest;
}

void end_staging(const Staging& s) {
    if (!s.readonly) copy_rows(s.host, rt::host_ptr(s.guest), s);
    rt::heap_free(s.guest);
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

template <typename LockFn>
HRESULT lock_rect(uint32_t proxy, uint32_t sub, uint32_t guest_lr, const D3DSURFACE_DESC& desc, const RECT* rect,
                  DWORD flags, LockFn&& lock) {
    D3DLOCKED_RECT lr{};
    const HRESULT hr = lock(&lr);
    if (FAILED(hr)) return hr;
    Staging s;
    shape_rect(s, desc, rect, uint32_t(lr.Pitch));
    const uint32_t g = begin_staging(s, lr.pBits, flags);
    wr32(guest_lr, uint32_t(lr.Pitch));
    wr32(guest_lr + 4, g);
    std::lock_guard lk(g_mu);
    g_locks[{proxy, sub}] = s;
    return hr;
}

template <typename UnlockFn>
HRESULT unlock_sub(uint32_t proxy, uint32_t sub, UnlockFn&& unlock) {
    {
        std::lock_guard lk(g_mu);
        auto it = g_locks.find({proxy, sub});
        if (it != g_locks.end()) {
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
        hr = lock_rect(a[0], 0, a[1], d, r, a[3], [&](D3DLOCKED_RECT* lr) { return self->LockRect(lr, r, a[3]); });
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
        hr = lock_rect(a[0], a[1], a[2], d, r, a[4], [&](D3DLOCKED_RECT* lr) { return self->LockRect(a[1], lr, r, a[4]); });
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 6;
}
void tex_unlock(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDirect3DTexture9*>(to_host(a[0]));
    c->eax = self ? uint32_t(unlock_sub(a[0], a[1], [&] { return self->UnlockRect(a[1]); })) : uint32_t(D3DERR_INVALIDCALL);
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
        hr = lock_rect(a[0], (a[1] << 16) | a[2], a[3], d, r, a[5],
                       [&](D3DLOCKED_RECT* lr) { return self->LockRect(D3DCUBEMAP_FACES(a[1]), a[2], lr, r, a[5]); });
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

// Vertex and index buffers keep a persistent guest shadow (they are locked
// every frame); the shadow mirrors the buffer, so only READONLY locks copy in.
struct BufferShadow {
    uint32_t guest = 0, size = 0;
    struct Lock { uint32_t offset, size; uint8_t* host; bool readonly; };
    std::vector<Lock> locks;
};
std::unordered_map<uint32_t, BufferShadow> g_shadows;

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
