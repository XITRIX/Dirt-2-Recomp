// Direct3D 11 / DXGI bridge: the game's 32-bit COM calls -> DXMT (native,
// libdxmt-native) on Metal. The DX11 renderer runs with DIRT2_RENDERER=dx11.
//
// The generic machinery (proxies, Arg<>, interface arrays, IID outs) is
// shims/com/bridge_core.h; vtables.inc is generated from DXMT's own headers
// (tools/scripts/gen_d3d11_vtables.py). Hand-written here are the methods
// taking structs laid out differently in the 32-bit guest (pointers, SIZE_T,
// HWND) and Map/Unmap, which hand the game guest-memory shadows.
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <map>
#include <mutex>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "../../platform/platform.h"
#include "../shim.h"
#include "guest_string.h"
#include "render_size.h"
#include "shadow_write.h"
#include "perf_stats.h"
#include "../input/touch_d3d11.h"

namespace shim {
void* host_window(uint32_t hwnd);
}
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
extern "C" void dirt2_dx11_present_succeeded();
extern "C" uint32_t dirt2D3D11GetPerfStats(ID3D11Device*, uint64_t*, uint32_t);
namespace shim::d3d11 { void preload_switch_shaders(ID3D11Device*, IDXGISwapChain*); }
#endif

// dxmt/0005-metalfx-native-target.patch: MetalFX upscales the back buffer to
// this size (the window's drawable) while enabled and larger.
#ifndef DIRT2_D3D11_DXVK
extern "C" void dxmt_set_spatial_upscale(int enabled, uint32_t width, uint32_t height);
#endif

using namespace shim;

namespace {

constexpr const char* kBridge = "d3d11";
constexpr uint32_t kProxyMagic = 0x42313144;  // "D11B"
constexpr HRESULT kInvalidCall = E_INVALIDARG;

// Distinct objects: one host pointer is never two of these at once. DXGI
// interfaces are views of D3D11 objects (a texture is also an IDXGISurface),
// so they are not leaves.
bool is_leaf_name(const char* name) {
    static const char* const leaves[] = {
        "ID3D11Buffer",          "ID3D11Texture1D",       "ID3D11Texture2D",         "ID3D11Texture3D",
        "ID3D11ShaderResourceView", "ID3D11RenderTargetView", "ID3D11DepthStencilView", "ID3D11UnorderedAccessView",
        "ID3D11VertexShader",    "ID3D11PixelShader",     "ID3D11HullShader",        "ID3D11DomainShader",
        "ID3D11GeometryShader",  "ID3D11ComputeShader",   "ID3D11InputLayout",       "ID3D11BlendState",
        "ID3D11DepthStencilState", "ID3D11RasterizerState", "ID3D11SamplerState",    "ID3D11Query",
        "ID3D11Predicate",       "ID3D11Counter"};
    for (const char* n : leaves)
        if (std::strcmp(name, n) == 0) return true;
    return false;
}

template <typename I>
uint32_t wrap(I* p);
uint32_t wrap_by_iid(const GUID& iid, void* obj);
void on_release(uint32_t proxy);

#include "../com/bridge_core.h"

// ---------------------------------------------------------------------------
// Guest layouts of the structs that differ (32-bit pointers, SIZE_T, HWND)
// ---------------------------------------------------------------------------

// DXGI_SWAP_CHAIN_DESC: 15 dwords; OutputWindow (a guest HWND) at +44.
void scd_in(uint32_t g, DXGI_SWAP_CHAIN_DESC& d) {
    d.BufferDesc.Width = rd32(g + 0);
    d.BufferDesc.Height = rd32(g + 4);
    d.BufferDesc.RefreshRate.Numerator = rd32(g + 8);
    d.BufferDesc.RefreshRate.Denominator = rd32(g + 12);
    d.BufferDesc.Format = DXGI_FORMAT(rd32(g + 16));
    d.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER(rd32(g + 20));
    d.BufferDesc.Scaling = DXGI_MODE_SCALING(rd32(g + 24));
    d.SampleDesc.Count = rd32(g + 28);
    d.SampleDesc.Quality = rd32(g + 32);
    d.BufferUsage = rd32(g + 36);
    d.BufferCount = rd32(g + 40);
    d.OutputWindow = nullptr;
    d.Windowed = WINBOOL(rd32(g + 48));
    d.SwapEffect = DXGI_SWAP_EFFECT(rd32(g + 52));
    d.Flags = rd32(g + 56);
}
void scd_out(uint32_t g, const DXGI_SWAP_CHAIN_DESC& d, uint32_t guest_hwnd) {
    const uint32_t v[15] = {d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.RefreshRate.Numerator,
                            d.BufferDesc.RefreshRate.Denominator, uint32_t(d.BufferDesc.Format),
                            uint32_t(d.BufferDesc.ScanlineOrdering), uint32_t(d.BufferDesc.Scaling), d.SampleDesc.Count,
                            d.SampleDesc.Quality, d.BufferUsage, d.BufferCount, guest_hwnd, uint32_t(d.Windowed),
                            uint32_t(d.SwapEffect), d.Flags};
    for (int i = 0; i < 15; ++i) wr32(g + 4 * i, v[i]);
}

// Per swap chain: the game's HWND, its host window, and the fullscreen state the
// game asked for (presentation is always a window; fullscreen is borderless).
struct SwapInfo {
    uint32_t guest_hwnd = 0;
    platform::Window window = nullptr;
    bool fullscreen = false;
    uint32_t target_width = 0, target_height = 0;
};
std::map<uint32_t, SwapInfo> g_swaps;  // swap chain proxy -> info

// Converts a guest swap-chain description into the host one: OutputWindow is
// the window's CAMetalLayer (dxmt/0001-native-hwnd-is-metal-layer.patch).
// Exclusive fullscreen becomes borderless desktop fullscreen; DIRT2_WINDOWED=1
// keeps a window sized to the buffers.
bool prepare_swap_chain(uint32_t gdesc, DXGI_SWAP_CHAIN_DESC& d, SwapInfo& info) {
    scd_in(gdesc, d);
    info.guest_hwnd = rd32(gdesc + 44);
    info.window = host_window(info.guest_hwnd);
    if (!info.window) {
        rt::log("d3d11: swap chain for unknown HWND %08X", info.guest_hwnd);
        return false;
    }
    info.fullscreen = !d.Windowed;
    d.Windowed = TRUE;
    static const bool force_windowed = std::getenv("DIRT2_WINDOWED") != nullptr;
    if (info.fullscreen && !force_windowed) {
        platform::set_fullscreen(info.window, true);
    } else {
        platform::set_fullscreen(info.window, false);
        if (d.BufferDesc.Width && d.BufferDesc.Height)
            platform::set_window_size(info.window, int(d.BufferDesc.Width), int(d.BufferDesc.Height));
    }
    platform::show_window(info.window, true);
#ifdef DIRT2_D3D11_DXVK
    d.OutputWindow = reinterpret_cast<HWND>(info.window);
#else
    d.OutputWindow = HWND(platform::metal_layer(info.window));
#endif
    if (!d.OutputWindow) return false;
    // 0 means the window's size; DXMT's native (headless) build would use 1024x1024.
    if (!d.BufferDesc.Width || !d.BufferDesc.Height) {
        int w = 0, h = 0;
        platform::drawable_size(info.window, &w, &h);
        if (!d.BufferDesc.Width) d.BufferDesc.Width = uint32_t(w);
        if (!d.BufferDesc.Height) d.BufferDesc.Height = uint32_t(h);
    }
    rt::log("d3d11: swap chain %ux%u format %u, %u buffers, %s", d.BufferDesc.Width, d.BufferDesc.Height,
            uint32_t(d.BufferDesc.Format), d.BufferCount, info.fullscreen ? "fullscreen" : "windowed");
    return true;
}

// D3D11_SUBRESOURCE_DATA[n]: {pSysMem, SysMemPitch, SysMemSlicePitch}, 12 bytes each.
std::vector<D3D11_SUBRESOURCE_DATA> init_data(uint32_t g, uint32_t n) {
    std::vector<D3D11_SUBRESOURCE_DATA> v;
    if (!g) return v;
    v.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t p = rd32(g + 12 * i);
        v[i].pSysMem = p ? rt::host_ptr(p) : nullptr;
        v[i].SysMemPitch = rd32(g + 12 * i + 4);
        v[i].SysMemSlicePitch = rd32(g + 12 * i + 8);
    }
    return v;
}

uint32_t mip_count(uint32_t mips, uint32_t w, uint32_t h = 1, uint32_t d = 1) {
    if (mips) return mips;
    uint32_t n = 1;
    for (uint32_t m = std::max({w, h, d}); m > 1; m >>= 1) ++n;
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Overrides
// ---------------------------------------------------------------------------
namespace {

#if defined(__ANDROID__)
#include "frame_capture.inc"
#endif

void dev_create_buffer(X86* c);
void dev_create_texture1d(X86* c);
void dev_create_texture2d(X86* c);
void dev_create_texture3d(X86* c);
void dev_create_input_layout(X86* c);
void ctx_map(X86* c);
void ctx_unmap(X86* c);
void ctx_copy_resource(X86* c);
void ctx_copy_region(X86* c);
void factory_create_swap_chain(X86* c);
void swap_get_desc(X86* c);
void swap_present(X86* c);
void swap_set_fullscreen_state(X86* c);
void swap_get_fullscreen_state(X86* c);
void swap_resize_buffers(X86* c);
void swap_resize_target(X86* c);
void output_get_modes(X86* c);
void output_find_mode(X86* c);
void adapter_get_desc(X86* c);
void adapter1_get_desc1(X86* c);
void output_get_desc(X86* c);

OVERRIDE(ID3D11Device, CreateBuffer, dev_create_buffer)
OVERRIDE(ID3D11Device, CreateTexture1D, dev_create_texture1d)
OVERRIDE(ID3D11Device, CreateTexture2D, dev_create_texture2d)
OVERRIDE(ID3D11Device, CreateTexture3D, dev_create_texture3d)
OVERRIDE(ID3D11Device, CreateInputLayout, dev_create_input_layout)
OVERRIDE(ID3D11DeviceContext, Map, ctx_map)
OVERRIDE(ID3D11DeviceContext, Unmap, ctx_unmap)
#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
OVERRIDE(ID3D11DeviceContext, CopyResource, ctx_copy_resource)
OVERRIDE(ID3D11DeviceContext, CopySubresourceRegion, ctx_copy_region)
#endif
OVERRIDE(IDXGIFactory, CreateSwapChain, factory_create_swap_chain)
OVERRIDE(IDXGISwapChain, GetDesc, swap_get_desc)
OVERRIDE(IDXGISwapChain, Present, swap_present)
OVERRIDE(IDXGISwapChain, SetFullscreenState, swap_set_fullscreen_state)
OVERRIDE(IDXGISwapChain, GetFullscreenState, swap_get_fullscreen_state)
OVERRIDE(IDXGISwapChain, ResizeBuffers, swap_resize_buffers)
OVERRIDE(IDXGIAdapter, GetDesc, adapter_get_desc)
OVERRIDE(IDXGIAdapter1, GetDesc1, adapter1_get_desc1)
OVERRIDE(IDXGIOutput, GetDesc, output_get_desc)
#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
OVERRIDE(IDXGISwapChain, ResizeTarget, swap_resize_target)
OVERRIDE(IDXGIOutput, GetDisplayModeList, output_get_modes)
OVERRIDE(IDXGIOutput, FindClosestMatchingMode, output_find_mode)
#endif

// IUnknown itself (QueryInterface for IID_IUnknown).
const recomp_fn vt_IUnknown[] = {&QI<IUnknown>, &AddRefB<IUnknown>, &ReleaseB<IUnknown>};
template <>
const VTableDesc& vtable_desc<IUnknown>() {
    static const VTableDesc d{"IUnknown", vt_IUnknown, 3};
    return d;
}

#include "vtables.inc"

// Most-derived wrapping for the resource base interface.
template <typename I>
uint32_t wrap(I* p) {
    if (!p) return 0;
    if constexpr (std::is_same_v<I, ID3D11Resource>) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        p->GetType(&dim);
        switch (dim) {
            case D3D11_RESOURCE_DIMENSION_BUFFER: return wrap_with(p, vtable_desc<ID3D11Buffer>());
            case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return wrap_with(p, vtable_desc<ID3D11Texture1D>());
            case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return wrap_with(p, vtable_desc<ID3D11Texture2D>());
            case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return wrap_with(p, vtable_desc<ID3D11Texture3D>());
            default: break;
        }
    }
    return wrap_with(p, vtable_desc<I>());
}

uint32_t wrap_by_iid(const GUID& iid, void* obj) {
#define TRY_IID(I) \
    if (iid == __uuidof(I)) return wrap(static_cast<I*>(obj));
    BRIDGE_INTERFACES(TRY_IID)
    TRY_IID(IUnknown)
#undef TRY_IID
    rt::log("d3d11: QueryInterface for an interface without a bridge vtable");
    return wrap(static_cast<IUnknown*>(obj));
}

// ---- Map/Unmap: guest shadows ----
//
// The game writes through the pointer Map returns, so it must be guest memory.
// Each (resource, subresource) keeps a persistent guest shadow: WRITE_DISCARD
// and NO_OVERWRITE maps write into it and Unmap copies it to the host mapping
// (the shadow keeps earlier contents, as NO_OVERWRITE expects); READ maps copy
// the host data in first.
struct MapShadow {
    uint32_t guest = 0, size = 0;
    void* host = nullptr;  // host mapping while mapped
    bool write = false, no_overwrite = false;
    uint32_t mapped_bytes = 0;
    std::vector<uint8_t> baseline;
    ID3D11Resource* mapped_resource = nullptr;
};
std::map<std::pair<uint32_t, uint32_t>, MapShadow> g_maps;
std::atomic<uint32_t> g_present_frames{0};
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
bool performance_trace() {
    static const bool on = [] { const char* e = std::getenv("DIRT2_DX11_PERF"); return e && e[0] == '1'; }();
    return on;
}
std::atomic<uint64_t> g_map_calls{0}, g_map_ns{0}, g_map_read_bytes{0}, g_map_write_bytes{0};
#endif
struct ExposureCopy { uint32_t proxy = 0, frame = 0; uint64_t ns = 0; uint32_t context = 0; };
std::map<uint32_t, ExposureCopy> g_exposure_copies;
std::map<uint32_t, ExposureCopy> g_exposure_latest; // context proxy -> most recently queued readback

// Diagnose the exposure ring by texture metadata, not driver-specific row padding.
bool small_float_readback(ID3D11Resource* resource, UINT sub, D3D11_TEXTURE2D_DESC& desc) {
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    if (sub || desc.Width != 1 || desc.Height != 1 || desc.Usage != D3D11_USAGE_STAGING ||
        !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) return false;
    return desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || desc.Format == DXGI_FORMAT_R32G32_FLOAT ||
           desc.Format == DXGI_FORMAT_R32_FLOAT || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           desc.Format == DXGI_FORMAT_R16G16_FLOAT || desc.Format == DXGI_FORMAT_R16_FLOAT;
}

void trace_exposure(uint32_t resource, uint32_t flags, HRESULT hr, const D3D11_TEXTURE2D_DESC& desc,
                    const D3D11_MAPPED_SUBRESOURCE& map, uint32_t size, uint32_t actual_resource = 0, double map_ms = 0) {
    static const bool enabled = std::getenv("DIRT2_LOG_EXPOSURE") != nullptr;
    if (!enabled) return;
    struct Sample { double ms; uint32_t frame, resource, flags, hr, format, pitch, bits[4];
                    uint32_t actual_resource, copy_frame, actual_copy_frame; double age_ms, actual_age_ms, map_ms; };
    static std::array<Sample, 128> samples{};
    static size_t count = 0;
    static const auto start = std::chrono::steady_clock::now();
    static auto last_flush = start;
    const auto now = std::chrono::steady_clock::now();
    static const unsigned trace_seconds = [] {
        const char* value = std::getenv("DIRT2_EXPOSURE_TRACE_SECONDS");
        return value ? unsigned(std::min(3600ul, std::strtoul(value, nullptr, 10))) : 120u;
    }();
    if (now - start > std::chrono::seconds(trace_seconds)) return;
    const uint32_t frame = g_present_frames.load();
    static std::map<uint32_t, uint32_t> last_failed_frame;
    if (FAILED(hr)) {
        auto it = last_failed_frame.find(resource);
        if (it != last_failed_frame.end() && it->second == frame) return;
        last_failed_frame[resource] = frame;
    }
    auto& sample = samples[count++];
    sample = {std::chrono::duration<double, std::milli>(now - start).count(), frame,
              resource, flags, uint32_t(hr), uint32_t(desc.Format), map.RowPitch, {0,0,0,0}};
    sample.map_ms = map_ms;
    sample.actual_resource = actual_resource ? actual_resource : resource;
    sample.copy_frame = sample.actual_copy_frame = UINT32_MAX;
    sample.age_ms = sample.actual_age_ms = -1;
    const uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    auto stamp = [&](uint32_t proxy, uint32_t& copied_frame, double& age) {
        if (auto it = g_exposure_copies.find(proxy); it != g_exposure_copies.end()) {
            copied_frame = it->second.frame;
            if (now_ns >= it->second.ns) age = double(now_ns - it->second.ns) / 1e6;
        }
    };
    stamp(resource, sample.copy_frame, sample.age_ms);
    stamp(sample.actual_resource, sample.actual_copy_frame, sample.actual_age_ms);
    // Raw bits also preserve half floats and NaN payloads, without misdecoding them.
    const uint32_t pixel_size = desc.Format == DXGI_FORMAT_R16_FLOAT ? 2 :
        desc.Format == DXGI_FORMAT_R16G16_FLOAT || desc.Format == DXGI_FORMAT_R32_FLOAT ? 4 :
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format == DXGI_FORMAT_R32G32_FLOAT ? 8 : 16;
    if (SUCCEEDED(hr) && map.pData) std::memcpy(sample.bits, map.pData, std::min(size, pixel_size));
    if (count < samples.size() && now - last_flush < std::chrono::seconds(5)) return;
    static std::FILE* file = [] {
        const char* data = std::getenv("DIRT2_DATA_DIR");
        const std::string path = std::string(data ? data : ".") + "/exposure.csv";
        std::rename(path.c_str(), (path + ".prev").c_str());
        auto* f = std::fopen(path.c_str(), "w");
        if (f) { std::setvbuf(f, nullptr, _IOFBF, 16384); std::fputs("ms,frame,resource,flags,hr,format,row_pitch,bits0,bits1,bits2,bits3,actual_resource,copy_frame,actual_copy_frame,age_ms,actual_age_ms,map_ms\n", f); }
        return f;
    }();
    if (file) {
        for (size_t i = 0; i < count; ++i) {
            const auto& s = samples[i];
            std::fprintf(file, "%.3f,%u,%08X,%X,%08X,%u,%u,%08X,%08X,%08X,%08X,%08X,%u,%u,%.3f,%.3f,%.3f\n", s.ms, s.frame,
                         s.resource, s.flags, s.hr, s.format, s.pitch, s.bits[0], s.bits[1], s.bits[2], s.bits[3],
                         s.actual_resource, s.copy_frame, s.actual_copy_frame, s.age_ms, s.actual_age_ms, s.map_ms);
        }
        std::fflush(file);
    }
    count = 0; last_flush = now;
}

void on_release(uint32_t proxy) {
    std::lock_guard lk(g_mu);
    for (auto it = g_maps.lower_bound({proxy, 0}); it != g_maps.end() && it->first.first == proxy;) {
        rt::heap_free(it->second.guest);
        it = g_maps.erase(it);
    }
    g_swaps.erase(proxy);
    g_exposure_copies.erase(proxy);
    g_exposure_latest.erase(proxy);
    for (auto it = g_exposure_latest.begin(); it != g_exposure_latest.end();) {
        if (it->second.proxy == proxy) it = g_exposure_latest.erase(it);
        else ++it;
    }
}

#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
void record_exposure_copy(uint32_t context, uint32_t proxy, ID3D11Resource* dst, ID3D11Resource* src) {
    D3D11_TEXTURE2D_DESC desc{};
    if (!dst || !small_float_readback(dst, 0, desc) || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    D3D11_RESOURCE_DIMENSION dim{}; src->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC source{}; static_cast<ID3D11Texture2D*>(src)->GetDesc(&source);
    if (source.Width != 1 || source.Height != 1 || source.Format != desc.Format ||
        !(source.BindFlags & D3D11_BIND_RENDER_TARGET)) return;
    const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard lk(g_mu);
    const ExposureCopy stamp{proxy, g_present_frames.load(), ns, context};
    g_exposure_copies[proxy] = stamp;
    g_exposure_latest[context] = stamp;
}
void ctx_copy_resource(X86* c) {
    const Args a{c};
    auto* self = static_cast<ID3D11DeviceContext*>(to_host(a[0]));
    auto* dst = static_cast<ID3D11Resource*>(to_host(a[1]));
    auto* src = static_cast<ID3D11Resource*>(to_host(a[2]));
    if (self && dst && src) { self->CopyResource(dst, src); record_exposure_copy(a[0], a[1], dst, src); }
    c->esp += 4 * 4;
}
void ctx_copy_region(X86* c) {
    const Args a{c};
    auto* self = static_cast<ID3D11DeviceContext*>(to_host(a[0]));
    auto* dst = static_cast<ID3D11Resource*>(to_host(a[1]));
    auto* src = static_cast<ID3D11Resource*>(to_host(a[6]));
    const auto* box = a.ptr<D3D11_BOX>(8);
    if (self && dst && src) {
        self->CopySubresourceRegion(dst, a[2], a[3], a[4], a[5], src, a[7], box);
        if (!a[2] && !a[3] && !a[4] && !a[5] && !a[7] &&
            (!box || (!box->left && !box->top && !box->front && box->right == 1 && box->bottom == 1 && box->back == 1)))
            record_exposure_copy(a[0], a[1], dst, src);
    }
    c->esp += 4 * 10;
}
#endif

// Bytes the game may touch in a mapped subresource.
uint32_t mapped_size(ID3D11Resource* r, UINT sub, const D3D11_MAPPED_SUBRESOURCE& m) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    switch (dim) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
            D3D11_BUFFER_DESC d;
            static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
            return d.ByteWidth;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
            D3D11_TEXTURE3D_DESC d;
            static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
            const uint32_t mip = sub % mip_count(d.MipLevels, d.Width, d.Height, d.Depth);
            return m.DepthPitch * std::max(1u, d.Depth >> mip);
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
            D3D11_TEXTURE2D_DESC d;
            static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            const uint32_t mip = sub % mip_count(d.MipLevels, d.Width, d.Height);
            uint32_t rows = std::max(1u, d.Height >> mip);
            const bool block = (d.Format >= DXGI_FORMAT_BC1_TYPELESS && d.Format <= DXGI_FORMAT_BC5_SNORM) ||
                               (d.Format >= DXGI_FORMAT_BC6H_TYPELESS && d.Format <= DXGI_FORMAT_BC7_UNORM_SRGB);
            if (block) rows = (rows + 3) / 4;
            return std::max(m.DepthPitch, m.RowPitch * rows);
        }
        default:
            return std::max(m.DepthPitch, m.RowPitch);
    }
}

// Map(resource, subresource, type, flags, D3D11_MAPPED_SUBRESOURCE*)
void ctx_map(X86* c) {
    const Args a{c};
    auto* self = static_cast<ID3D11DeviceContext*>(to_host(a[0]));
    auto* res = static_cast<ID3D11Resource*>(to_host(a[1]));
    const uint32_t sub = a[2], type = a[3], out = a[5];
    HRESULT hr = E_INVALIDARG;
    if (self && res) {
        uint32_t actual_proxy = a[1];
        auto* actual = res;
        D3D11_MAPPED_SUBRESOURCE m{};
        bool already_mapped = false;
        D3D11_TEXTURE2D_DESC readback_desc{};
        const bool exposure_read = type == D3D11_MAP_READ && small_float_readback(res, sub, readback_desc);
        bool measure_map = exposure_read;
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
        measure_map = measure_map || performance_trace();
#endif
        const auto map_start = measure_map ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
        static const bool fresh_exposure = [] { const char* e = std::getenv("DIRT2_DX11_FRESH_EXPOSURE"); return e && e[0] == '1'; }();
        // Opt-in DiRT2 compatibility experiment, not a change to general D3D11
        // mapping semantics: consume the latest queued exposure pixel, not an
        // older ring entry. Preserve normal behavior for all other resources.
        if (fresh_exposure && exposure_read && readback_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            !a[4] && rd32(c->esp) == 0x00FAF55C && self->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            uint32_t candidate = 0;
            { std::lock_guard lk(g_mu); if (auto it = g_exposure_latest.find(a[0]); it != g_exposure_latest.end()) candidate = it->second.proxy; }
            if (auto* resource = static_cast<ID3D11Resource*>(to_host(candidate))) {
                D3D11_TEXTURE2D_DESC desc{};
                if (small_float_readback(resource, 0, desc) && desc.Format == readback_desc.Format) {
                    actual = resource; actual_proxy = candidate;
                    static const bool asynchronous = [] {
                        const char* e = std::getenv("DIRT2_DX11_ASYNC_EXPOSURE"); return e && e[0] == '1';
                    }();
                    if (asynchronous) {
                        // Poll newest first, but never consume a pixel more than
                        // two presents old. If the GPU is further behind, retain
                        // the fresh blocking path rather than restoring the long
                        // exposure feedback delay that caused visible pumping.
                        std::vector<ExposureCopy> candidates;
                        const uint32_t frame = g_present_frames.load();
                        {
                            std::lock_guard lk(g_mu);
                            for (const auto& [proxy, copy] : g_exposure_copies)
                                if (copy.context == a[0] && uint32_t(frame - copy.frame) <= 2)
                                    candidates.push_back(copy);
                        }
                        std::sort(candidates.begin(), candidates.end(), [](const auto& x, const auto& y) { return x.ns > y.ns; });
                        for (const auto& copy : candidates) {
                            auto* ready = static_cast<ID3D11Resource*>(to_host(copy.proxy));
                            if (!ready) continue;
                            m = {};
                            hr = self->Map(ready, sub, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
                            if (SUCCEEDED(hr)) {
                                actual = ready; actual_proxy = copy.proxy; already_mapped = true; break;
                            }
                            // Only "still drawing" permits trying another pixel.
                            if (hr != DXGI_ERROR_WAS_STILL_DRAWING) { already_mapped = true; break; }
                        }
                    }
                }
            }
        }
#endif
        if (!already_mapped) {
            m = {};
            hr = self->Map(actual, sub, D3D11_MAP(type), a[4], &m);
        }
        const double map_ms = measure_map ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - map_start).count() : 0;
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
        if (performance_trace()) {
            g_map_calls.fetch_add(1, std::memory_order_relaxed);
            g_map_ns.fetch_add(uint64_t(map_ms * 1e6), std::memory_order_relaxed);
        }
#endif
        if (SUCCEEDED(hr)) {
            std::lock_guard lk(g_mu);
            MapShadow& sh = g_maps[{a[1], sub}];
            const uint32_t size = mapped_size(res, sub, m);
            if (sh.guest && sh.size < size) {
                rt::heap_free(sh.guest);
                sh = MapShadow{};
            }
            if (!sh.guest) {
                sh.guest = rt::heap_alloc(std::max(size, 16u), true);
                sh.size = size;
            }
            sh.host = m.pData;
            sh.mapped_resource = actual;
            sh.write = type != D3D11_MAP_READ;
            sh.no_overwrite = type == D3D11_MAP_WRITE_NO_OVERWRITE;
            sh.mapped_bytes = size;
            if (sh.no_overwrite) {
                sh.baseline.resize(size);
                std::memcpy(sh.baseline.data(), rt::host_ptr(sh.guest), size);
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                    rt::log("d3d11: NO_OVERWRITE map %08X (%u bytes) from %08X", a[1], size, rd32(c->esp));
            }
            if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) {
                std::memcpy(rt::host_ptr(sh.guest), m.pData, size);
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
                if (performance_trace()) g_map_read_bytes.fetch_add(size, std::memory_order_relaxed);
#endif
            }
            // The game reads its scene luminance back through a ring of 1x1 RGBA32F
            // staging textures (16 bytes, from 00FAF55C). A non-finite or negative
            // value there (an HDR edge case on some track spots) sent the auto
            // exposure swinging for seconds: the game gets the last good value
            // instead, and anomalies are logged.
            if (exposure_read) trace_exposure(a[1], a[4], hr, readback_desc, m, size, actual_proxy, map_ms);
            if (exposure_read && readback_desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT && size >= 16) {
                static float last[4] = {0, 0, 0, 0};
                static bool have = false;
                static std::atomic<int> logged{0};
                float* g = reinterpret_cast<float*>(rt::host_ptr(sh.guest));
                bool bad = false, jump = false;
                for (int k = 0; k < 4; ++k) {
                    if (!std::isfinite(g[k]) || g[k] < 0) bad = true;
                    else if (have && last[k] > 1e-6f && (g[k] > last[k] * 8 || g[k] < last[k] / 8)) jump = true;
                }
                if ((bad || jump) && logged++ < 40)
                    rt::log("d3d11: luminance readback %s: %g %g %g %g (last %g %g %g %g)", bad ? "invalid, replaced" : "jump",
                            g[0], g[1], g[2], g[3], last[0], last[1], last[2], last[3]);
                if (bad && have) std::memcpy(g, last, 16);
                else if (!bad) std::memcpy(last, g, 16), have = true;
            }
            // DIRT2_LOG_MAP: reads of small resources (luminance readback) with their values.
            static const bool log_map = std::getenv("DIRT2_LOG_MAP") != nullptr;
            if (log_map && type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE && size <= 4096) {
                static std::atomic<uint32_t> n{0};
                if (n++ % 30 == 0) {
                    float f[4]{};
                    std::memcpy(f, m.pData, std::min(size, uint32_t(sizeof(f))));
                    rt::log("d3d11: Map(%08X sub %u type %u flags %X) %u bytes from %08X: %g %g %g %g", a[1], sub, type,
                            a[4], size, rd32(c->esp), f[0], f[1], f[2], f[3]);
                }
            }
            if (out) {
                wr32(out, sh.guest);
                wr32(out + 4, m.RowPitch);
                wr32(out + 8, m.DepthPitch);
            }
        } else {
            if (exposure_read) {
                std::lock_guard lk(g_mu);
                trace_exposure(a[1], a[4], hr, readback_desc, m, 0, actual_proxy, map_ms);
            }
            static std::atomic<uint32_t> still{0};
            static std::atomic<int> logged{0};
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                if (std::getenv("DIRT2_LOG_MAP") && still++ % 30 == 0)
                    rt::log("d3d11: Map(%08X) still drawing (%u times)", a[1], still.load());
            } else if (logged++ < 20) {
                rt::log("d3d11: Map(sub %u, type %u) failed: %08X", sub, type, uint32_t(hr));
            }
        }
    }
    if (FAILED(hr) && out) wr32(out, 0);
    c->eax = uint32_t(hr);
    c->esp += 4 * 7;
}

// Unmap(resource, subresource)
void ctx_unmap(X86* c) {
    const Args a{c};
    auto* self = static_cast<ID3D11DeviceContext*>(to_host(a[0]));
    auto* res = static_cast<ID3D11Resource*>(to_host(a[1]));
    if (self && res) {
        ID3D11Resource* actual = res;
        {
            std::lock_guard lk(g_mu);
            auto it = g_maps.find({a[1], a[2]});
            if (it != g_maps.end() && it->second.host) {
                if (it->second.write) {
                    if (it->second.no_overwrite)
                        shim::d3d11::write_shadow_changes(it->second.host, rt::host_ptr(it->second.guest),
                                                         it->second.baseline.data(), it->second.mapped_bytes);
                    else
                        std::memcpy(it->second.host, rt::host_ptr(it->second.guest), it->second.mapped_bytes);
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
                    if (performance_trace()) g_map_write_bytes.fetch_add(it->second.size, std::memory_order_relaxed);
#endif
                }
                it->second.host = nullptr;
                if (it->second.mapped_resource) actual = it->second.mapped_resource;
                it->second.mapped_resource = nullptr;
            }
        }
        self->Unmap(actual, a[2]);
    }
    c->esp += 4 * 4;
}

// ---- creation with initial data ----

// CreateBuffer(desc, init, **buffer)
void dev_create_buffer(X86* c) {
    const Args a{c};
    auto* self = static_cast<ID3D11Device*>(to_host(a[0]));
    const auto* desc = a.ptr<D3D11_BUFFER_DESC>(1);
    auto init = init_data(a[2], 1);
    ID3D11Buffer* b = nullptr;
    const HRESULT hr = self && desc ? self->CreateBuffer(desc, init.empty() ? nullptr : init.data(), a[3] ? &b : nullptr)
                                    : E_INVALIDARG;
    if (FAILED(hr)) rt::log("d3d11: CreateBuffer(%u bytes, bind %X) failed: %08X", desc ? desc->ByteWidth : 0,
                            desc ? desc->BindFlags : 0, uint32_t(hr));
    if (a[3]) wr32(a[3], wrap(b));
    c->eax = uint32_t(hr);
    c->esp += 4 * 5;
}

template <typename Desc, typename Tex, HRESULT (STDMETHODCALLTYPE ID3D11Device::*Create)(const Desc*, const D3D11_SUBRESOURCE_DATA*, Tex**)>
void create_texture(X86* c, uint32_t subresources) {
    const Args a{c};
    auto* self = static_cast<ID3D11Device*>(to_host(a[0]));
    const auto* desc = a.ptr<Desc>(1);
    auto init = init_data(a[2], subresources);
    Tex* t = nullptr;
    const HRESULT hr =
        self && desc ? (self->*Create)(desc, init.empty() ? nullptr : init.data(), a[3] ? &t : nullptr) : E_INVALIDARG;
    // Small float render targets (the luminance chain), logged once per size and format.
    if constexpr (requires { desc->Height; })
        if (desc && (desc->BindFlags & D3D11_BIND_RENDER_TARGET) && desc->Width <= 512 && desc->Height <= 512) {
            const uint32_t f = uint32_t(desc->Format);
            const bool fp = f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT ||
                            f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT ||
                            f == DXGI_FORMAT_R11G11B10_FLOAT;
            static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
            std::lock_guard lk(g_mu);
            if (fp && seen.insert({desc->Width, desc->Height, f}).second)
                rt::log("d3d11: float render target %ux%u format %u", desc->Width, desc->Height, f);
        }
    if (FAILED(hr) && desc) {
        uint32_t h = 0;
        if constexpr (requires { desc->Height; }) h = desc->Height;
        rt::log("d3d11: CreateTexture(%ux%u, %u mips, format %u, usage %u, bind %X, cpu %X, misc %X, %zu init) failed: %08X",
                desc->Width, h, desc->MipLevels, uint32_t(desc->Format), uint32_t(desc->Usage), desc->BindFlags,
                desc->CPUAccessFlags, desc->MiscFlags, init.size(), uint32_t(hr));
    }
    if (a[3]) wr32(a[3], wrap(t));
    c->eax = uint32_t(hr);
    c->esp += 4 * 5;
}

void dev_create_texture1d(X86* c) {
    const auto* d = Args{c}.ptr<D3D11_TEXTURE1D_DESC>(1);
    create_texture<D3D11_TEXTURE1D_DESC, ID3D11Texture1D, &ID3D11Device::CreateTexture1D>(
        c, d ? mip_count(d->MipLevels, d->Width) * d->ArraySize : 0);
}
void dev_create_texture2d(X86* c) {
    const auto* d = Args{c}.ptr<D3D11_TEXTURE2D_DESC>(1);
    create_texture<D3D11_TEXTURE2D_DESC, ID3D11Texture2D, &ID3D11Device::CreateTexture2D>(
        c, d ? mip_count(d->MipLevels, d->Width, d->Height) * d->ArraySize : 0);
}
void dev_create_texture3d(X86* c) {
    const auto* d = Args{c}.ptr<D3D11_TEXTURE3D_DESC>(1);
    create_texture<D3D11_TEXTURE3D_DESC, ID3D11Texture3D, &ID3D11Device::CreateTexture3D>(
        c, d ? mip_count(d->MipLevels, d->Width, d->Height, d->Depth) : 0);
}

// CreateInputLayout(descs, n, bytecode, length, **layout): 28-byte guest elements.
void dev_create_input_layout(X86* c) {
    const Args a{c};
    auto* self = static_cast<ID3D11Device*>(to_host(a[0]));
    const uint32_t g = a[1], n = a[2];
    std::vector<D3D11_INPUT_ELEMENT_DESC> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = g + 28 * i;
        v[i].SemanticName = rt::host_str(rd32(e));
        v[i].SemanticIndex = rd32(e + 4);
        v[i].Format = DXGI_FORMAT(rd32(e + 8));
        v[i].InputSlot = rd32(e + 12);
        v[i].AlignedByteOffset = rd32(e + 16);
        v[i].InputSlotClass = D3D11_INPUT_CLASSIFICATION(rd32(e + 20));
        v[i].InstanceDataStepRate = rd32(e + 24);
    }
    ID3D11InputLayout* l = nullptr;
    const HRESULT hr = self ? self->CreateInputLayout(v.data(), n, a[3] ? rt::host_ptr(a[3]) : nullptr, a[4],
                                                      a[5] ? &l : nullptr)
                            : E_INVALIDARG;
    if (FAILED(hr)) rt::log("d3d11: CreateInputLayout(%u elements) failed: %08X", n, uint32_t(hr));
    if (a[5]) wr32(a[5], wrap(l));
    c->eax = uint32_t(hr);
    c->esp += 4 * 7;
}

// ---- DXGI ----

// IDXGIFactory::CreateSwapChain(device, desc, **swapchain)
void factory_create_swap_chain(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGIFactory*>(to_host(a[0]));
    IUnknown* dev = to_host(a[1]);
    DXGI_SWAP_CHAIN_DESC d{};
    SwapInfo info;
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = E_INVALIDARG;
    if (self && dev && a[2] && prepare_swap_chain(a[2], d, info)) hr = self->CreateSwapChain(dev, &d, &sc);
    if (FAILED(hr)) rt::log("d3d11: CreateSwapChain failed: %08X", uint32_t(hr));
    const uint32_t p = wrap(sc);
    if (p) {
        std::lock_guard lk(g_mu);
        g_swaps[p] = info;
    }
    if (a[3]) wr32(a[3], p);
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
    if (SUCCEEDED(hr) && sc && dev) {
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
            shim::d3d11::preload_switch_shaders(device, sc);
            device->Release();
        }
    }
#endif
    c->eax = uint32_t(hr);
    c->esp += 4 * 5;
}

void swap_get_desc(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGISwapChain*>(to_host(a[0]));
    DXGI_SWAP_CHAIN_DESC d{};
    const HRESULT hr = self && a[1] ? self->GetDesc(&d) : E_INVALIDARG;
    if (SUCCEEDED(hr)) {
        std::lock_guard lk(g_mu);
        const SwapInfo& info = g_swaps[a[0]];
        d.Windowed = !info.fullscreen;
        scd_out(a[1], d, info.guest_hwnd);
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 3;
}

// DIRT2_SCREENSHOT_DIR (+ _EVERY, _FROM, _COUNT): the back buffer as a BMP,
// as the D3D9 bridge does, so both renderers' scripted runs can be compared.
void capture(IDXGISwapChain* sc, uint32_t frame) {
    static const char* dir = std::getenv("DIRT2_SCREENSHOT_DIR");
    static const uint32_t every = std::getenv("DIRT2_SCREENSHOT_EVERY") ? std::atoi(std::getenv("DIRT2_SCREENSHOT_EVERY")) : 120;
    static const uint32_t from = std::getenv("DIRT2_SCREENSHOT_FROM") ? std::atoi(std::getenv("DIRT2_SCREENSHOT_FROM")) : 0;
    static const uint32_t count = std::getenv("DIRT2_SCREENSHOT_COUNT") ? std::atoi(std::getenv("DIRT2_SCREENSHOT_COUNT")) : 0;
    if (!dir || !every || frame < from || frame % every || (count && (frame - from) / every >= count)) return;
    static bool made = false;
    if (!made) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        made = true;
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
    d.SampleDesc = {1, 0};
    ID3D11Texture2D* st = nullptr;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st))) {
        ctx->CopyResource(st, bb);
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
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
                const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                std::vector<uint8_t> line(row);
                for (uint32_t y = 0; y < d.Height; ++y) {
                    std::memcpy(line.data(), static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, row);
                    for (uint32_t x = 0; x < d.Width; ++x) {
                        if (rgba) std::swap(line[4 * x], line[4 * x + 2]);
                        line[4 * x + 3] = 0xFF;
                    }
                    std::fwrite(line.data(), 1, row, f);
                }
                std::fclose(f);
                rt::log("d3d11: saved %s (%ux%u fmt %u)", path, d.Width, d.Height, uint32_t(d.Format));
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    ctx->Release();
    dev->Release();
    bb->Release();
}

// MetalFX target for DXMT: the swap chain window's drawable in pixels, and the
// game's MetalFX setting. The size is queried every 60 frames (it runs on the
// main thread); the setting is read every frame.
void update_upscale_target(uint32_t proxy, bool force = false) {
#ifndef DIRT2_D3D11_DXVK
    if (!platform::metalfx_supported()) return;
    static uint32_t frames = 0;
    static int w = 0, h = 0;
    if (force || frames++ % 60 == 0) {
        platform::Window win = nullptr;
        {
            std::lock_guard lk(g_mu);
            auto it = g_swaps.find(proxy);
            if (it != g_swaps.end()) win = it->second.window;
        }
        if (win) platform::drawable_size(win, &w, &h);
    }
    dxmt_set_spatial_upscale(platform::metalfx_enabled(), uint32_t(std::max(w, 0)), uint32_t(std::max(h, 0)));
#endif
}

#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
void profile_dx11_frame(IDXGISwapChain* self, double present_ms) {
    static const bool enabled = [] { const char* e = std::getenv("DIRT2_DX11_PERF"); return e && e[0] == '1'; }();
    if (!enabled || !self) return;
    using namespace shim::d3d11;
    static PerfStats previous{};
    static bool have = false;
    static auto start = std::chrono::steady_clock::now();
    static unsigned frames = 0;
    static double present_total = 0;
    ++frames; present_total += present_ms;
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - start).count();
    if (have && ms < 5000) return;
    ID3D11Device* device = nullptr;
    if (FAILED(self->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) return;
    PerfStats counters{};
    const bool valid = dirt2D3D11GetPerfStats(device, counters.data(), counters.size()) != 0;
    device->Release();
    if (!valid) return;
    if (have) {
        const auto d = perf_delta(counters, previous);
        const double f = frames;
        rt::log("perf-dx11: %u frames %.1f fps, frame %.3f ms, Present %.3f ms/frame", frames, f * 1000 / ms, ms / f, present_total / f);
        rt::log("perf-dx11: per frame draws %.1f dispatches %.1f passes %.1f barriers %.1f submissions %.1f cs-chunks %.1f", d[Draws]/f, d[Dispatches]/f, d[RenderPasses]/f, d[Barriers]/f, d[Submits]/f, d[CsChunks]/f);
        rt::log("perf-dx11: wait ms/frame GPU %.3f CS %.3f, waits/frame GPU %.1f CS %.1f, queue-idle %.3f ms/frame, cs-idle %.3f ms/frame", d[GpuWaitUs]/(1000*f), d[CsWaitUs]/(1000*f), d[GpuWaits]/f, d[CsWaits]/f, d[GpuIdleUs]/(1000*f), d[CsIdleUs]/(1000*f));
        rt::log("perf-dx11: maps/frame %.1f native-Map %.3f ms/frame, shadow copies read %.3f MiB/frame write %.3f MiB/frame",
            g_map_calls.exchange(0, std::memory_order_relaxed)/f, g_map_ns.exchange(0, std::memory_order_relaxed)/(1e6*f),
            g_map_read_bytes.exchange(0, std::memory_order_relaxed)/(1048576*f), g_map_write_bytes.exchange(0, std::memory_order_relaxed)/(1048576*f));
        rt::log("perf-dx11: compiler %llu/%llu completed (window +%llu/+%llu); queue-idle is not a hardware GPU occupancy measurement", (unsigned long long)counters[CompileDone], (unsigned long long)counters[CompileTotal], (unsigned long long)d[CompileDone], (unsigned long long)d[CompileTotal]);
    }
    if (!have) {
        g_map_calls = 0; g_map_ns = 0; g_map_read_bytes = 0; g_map_write_bytes = 0;
    }
    previous = counters; have = true; frames = 0; present_total = 0; start = now;
}
#endif

// Present(sync interval, flags): the render thread parks here in the background.
void swap_present(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGISwapChain*>(to_host(a[0]));
    ++g_present_frames;
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
    static bool first = true;
    if (first) { first = false; rt::log("d3d11: entering first Present"); }
#endif
    platform::gpu_checkpoint();
#if defined(__ANDROID__)
    frame_capture::boundary();
#endif
    update_upscale_target(a[0]);
    static uint32_t frames = 0;
    if (self) capture(self, ++frames);
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
    static const bool timing = [] { const char* e = std::getenv("DIRT2_DX11_PERF"); return e && e[0] == '1'; }();
    const auto present_start = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
#ifdef DIRT2_TOUCH_CONTROLLER
    if (!(a[2] & DXGI_PRESENT_TEST)) platform::touch::present_d3d11(self);
#endif
    const HRESULT hr = self ? self->Present(a[1], a[2]) : E_INVALIDARG;
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
    if (timing) profile_dx11_frame(self, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_start).count());
#endif
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
    if (SUCCEEDED(hr)) dirt2_dx11_present_succeeded();
#endif
    static uint32_t n = 0;
    if (n == 0 || (FAILED(hr) && n < 32))
        rt::log("d3d11: Present(sync %u, flags %X) -> %08X", a[1], a[2], uint32_t(hr));
    static auto t0 = std::chrono::steady_clock::now();
    if (++n % 300 == 0) {
        const auto t = std::chrono::steady_clock::now();
        rt::log("d3d11: %u presents, %.1f fps", n, 300.0 / std::chrono::duration<double>(t - t0).count());
        t0 = t;
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 4;
}

// Fullscreen is borderless desktop fullscreen on the swap chain's window.
void swap_set_fullscreen_state(X86* c) {
    const Args a{c};
    platform::Window w = nullptr;
    {
        std::lock_guard lk(g_mu);
        SwapInfo& info = g_swaps[a[0]];
        info.fullscreen = a[1] != 0;
        w = info.window;
    }
    static const bool force_windowed = std::getenv("DIRT2_WINDOWED") != nullptr;
    if (w && !force_windowed) platform::set_fullscreen(w, a[1] != 0);
    update_upscale_target(a[0], true);
    c->eax = uint32_t(S_OK);
    c->esp += 4 * 4;
}

// ResizeBuffers(count, width, height, format, flags): 0 means the window's
// size, which DXMT's native (headless) build answers with 1024x1024.
void swap_resize_buffers(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGISwapChain*>(to_host(a[0]));
    uint32_t w = a[2], h = a[3];
    if (self && (!w || !h)) {
        platform::Window win = nullptr;
        shim::d3d11::RenderSize target{};
        {
            std::lock_guard lk(g_mu);
            if (auto it = g_swaps.find(a[0]); it != g_swaps.end()) {
                win = it->second.window;
#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
                target = {it->second.target_width, it->second.target_height};
#endif
            }
        }
        int dw = 0, dh = 0;
        if (win) platform::drawable_size(win, &dw, &dh);
        const auto size = shim::d3d11::resize_size({w, h}, target, {uint32_t(dw), uint32_t(dh)});
        w = size.width; h = size.height;
    }
    const HRESULT hr = self ? self->ResizeBuffers(a[1], w, h, DXGI_FORMAT(a[4]), a[5]) : E_INVALIDARG;
    rt::log("d3d11: ResizeBuffers(%u, %ux%u -> %ux%u, format %u) -> %08X", a[1], a[2], a[3], w, h, a[4], uint32_t(hr));
    update_upscale_target(a[0], true);
    c->eax = uint32_t(hr);
    c->esp += 4 * 7;
}

#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
void swap_resize_target(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGISwapChain*>(to_host(a[0]));
    const auto* mode = a.ptr<DXGI_MODE_DESC>(1);
    // Native DXVK resizes the SDL window here. Retain a fixed physical output;
    // the original requested size is applied to the game's rendering buffers.
    DXGI_MODE_DESC physical = mode ? *mode : DXGI_MODE_DESC{};
    if (mode && mode->Width && mode->Height) {
        int width=0, height=0; platform::display_size(&width,&height);
        physical.Width = uint32_t(width); physical.Height = uint32_t(height);
    }
    const HRESULT hr = self ? self->ResizeTarget(mode ? &physical : nullptr) : E_INVALIDARG;
    if (SUCCEEDED(hr) && mode && mode->Width && mode->Height) {
        std::lock_guard lk(g_mu);
        g_swaps[a[0]].target_width = mode->Width;
        g_swaps[a[0]].target_height = mode->Height;
    }
    rt::log("d3d11: ResizeTarget(%ux%u) -> %08X", mode ? mode->Width : 0, mode ? mode->Height : 0, uint32_t(hr));
    c->eax = uint32_t(hr); c->esp += 4 * 3;
}

void output_get_modes(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGIOutput*>(to_host(a[0]));
    auto* count = a.ptr<UINT>(3);
    auto* desc = a.ptr<DXGI_MODE_DESC>(4);
    UINT physical = 0;
    HRESULT hr = self && count ? self->GetDisplayModeList(DXGI_FORMAT(a[1]), a[2], &physical, nullptr) : DXGI_ERROR_INVALID_CALL;
    if (SUCCEEDED(hr) && count) {
        const auto& modes = platform::display_modes();
        const UINT required = physical ? UINT(modes.size()) : 0;
        const UINT capacity = *count;
        if (desc) {
            for (UINT i = 0; i < std::min(required, capacity); ++i)
                desc[i] = {modes[i].width, modes[i].height, {modes[i].refresh, 1}, DXGI_FORMAT(a[1]),
                           DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED};
        }
        if (desc && capacity < required) hr = DXGI_ERROR_MORE_DATA;
        else *count = required;
    }
    c->eax = uint32_t(hr); c->esp += 4 * 6;
}

void output_find_mode(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGIOutput*>(to_host(a[0]));
    const auto* requested = a.ptr<DXGI_MODE_DESC>(1);
    auto* result = a.ptr<DXGI_MODE_DESC>(2);
    const DXGI_MODE_DESC input = requested ? *requested : DXGI_MODE_DESC{};
    const HRESULT hr = self ? self->FindClosestMatchingMode(requested, result, to_host(a[3])) : E_INVALIDARG;
    if (SUCCEEDED(hr) && requested && result) {
        const auto& modes = platform::display_modes();
        const platform::DisplayMode* best = nullptr;
        uint64_t score = UINT64_MAX;
        for (const auto& mode : modes) {
            const int64_t dx = int64_t(mode.width) - (input.Width ? input.Width : 1280);
            const int64_t dy = int64_t(mode.height) - (input.Height ? input.Height : 720);
            const uint64_t distance = uint64_t(dx * dx + dy * dy);
            if (distance < score) { score = distance; best = &mode; }
        }
        if (best) {
            result->Width = best->width; result->Height = best->height;
            result->RefreshRate = {best->refresh, 1};
        }
        rt::log("d3d11: closest render mode %ux%u -> %ux%u", input.Width, input.Height, result->Width, result->Height);
    }
    c->eax = uint32_t(hr); c->esp += 4 * 5;
}
#endif

void swap_get_fullscreen_state(X86* c) {
    const Args a{c};
    bool fs;
    {
        std::lock_guard lk(g_mu);
        fs = g_swaps[a[0]].fullscreen;
    }
    if (a[1]) wr32(a[1], fs ? 1 : 0);
    if (a[2]) wr32(a[2], 0);
    c->eax = uint32_t(S_OK);
    c->esp += 4 * 4;
}

// DXGI_ADAPTER_DESC: Description[128] WCHAR, 4 ids, 3 SIZE_T (4 bytes in the guest), LUID.
void adapter_desc_out(uint32_t g, const DXGI_ADAPTER_DESC& d) {
    const auto description = shim::d3d11::guest_string(d.Description);
    std::memcpy(rt::host_ptr(g), description.data(), 256);
    wr32(g + 256, d.VendorId);
    wr32(g + 260, d.DeviceId);
    wr32(g + 264, d.SubSysId);
    wr32(g + 268, d.Revision);
    auto mb = [](SIZE_T v) { return uint32_t(std::min<SIZE_T>(v, 0x7FF00000)); };  // a 32-bit game reads 32 bits
    wr32(g + 272, mb(d.DedicatedVideoMemory));
    wr32(g + 276, mb(d.DedicatedSystemMemory));
    wr32(g + 280, mb(d.SharedSystemMemory));
    wr32(g + 284, d.AdapterLuid.LowPart);
    wr32(g + 288, uint32_t(d.AdapterLuid.HighPart));
}

void adapter_get_desc(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGIAdapter*>(to_host(a[0]));
    DXGI_ADAPTER_DESC d{};
    const HRESULT hr = self && a[1] ? self->GetDesc(&d) : E_INVALIDARG;
    if (SUCCEEDED(hr)) adapter_desc_out(a[1], d);
    c->eax = uint32_t(hr);
    c->esp += 4 * 3;
}

void adapter1_get_desc1(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGIAdapter1*>(to_host(a[0]));
    DXGI_ADAPTER_DESC1 d{};
    const HRESULT hr = self && a[1] ? self->GetDesc1(&d) : E_INVALIDARG;
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC d0{};
        std::memcpy(d0.Description, d.Description, sizeof d0.Description);
        d0.VendorId = d.VendorId, d0.DeviceId = d.DeviceId, d0.SubSysId = d.SubSysId, d0.Revision = d.Revision;
        d0.DedicatedVideoMemory = d.DedicatedVideoMemory, d0.DedicatedSystemMemory = d.DedicatedSystemMemory;
        d0.SharedSystemMemory = d.SharedSystemMemory, d0.AdapterLuid = d.AdapterLuid;
        adapter_desc_out(a[1], d0);
        wr32(a[1] + 292, d.Flags);
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 3;
}

// DXGI_OUTPUT_DESC: DeviceName[32] WCHAR, RECT, BOOL, rotation, HMONITOR (a guest handle).
void output_get_desc(X86* c) {
    const Args a{c};
    auto* self = static_cast<IDXGIOutput*>(to_host(a[0]));
    DXGI_OUTPUT_DESC d{};
    const HRESULT hr = self && a[1] ? self->GetDesc(&d) : E_INVALIDARG;
    if (SUCCEEDED(hr)) {
#if defined(DIRT2_D3D11_DXVK) && (defined(__SWITCH__) || defined(__ANDROID__))
        int w = 0, h = 0;
        platform::display_size(&w, &h);
        d.DesktopCoordinates = {0, 0, w, h};
#endif
        const uint32_t g = a[1];
        const auto name = shim::d3d11::guest_string(d.DeviceName);
        std::memcpy(rt::host_ptr(g), name.data(), 64);
        wr32(g + 64, uint32_t(d.DesktopCoordinates.left));
        wr32(g + 68, uint32_t(d.DesktopCoordinates.top));
        wr32(g + 72, uint32_t(d.DesktopCoordinates.right));
        wr32(g + 76, uint32_t(d.DesktopCoordinates.bottom));
        wr32(g + 80, uint32_t(d.AttachedToDesktop));
        wr32(g + 84, uint32_t(d.Rotation));
        wr32(g + 88, 0x00010001);  // the monitor handle user32 hands out
    }
    c->eax = uint32_t(hr);
    c->esp += 4 * 3;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

// D3D11CreateDeviceAndSwapChain(adapter, driver, software, flags, levels, nlevels,
//                               sdk, swapdesc, **swapchain, **device, *level, **context)
STDCALL(d3d11, D3D11CreateDeviceAndSwapChain, 12) {
    IDXGIAdapter* adapter = static_cast<IDXGIAdapter*>(to_host(a[0]));
    const auto* levels = a.ptr<const D3D_FEATURE_LEVEL>(4);
    DXGI_SWAP_CHAIN_DESC d{};
    SwapInfo info;
    const bool swap = a[7] && a[8];
    if (swap && !prepare_swap_chain(a[7], d, info)) return uint32_t(E_INVALIDARG);
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, D3D_DRIVER_TYPE(a[1]), nullptr, a[3], levels, a[5], a[6],
                                                     swap ? &d : nullptr, swap ? &sc : nullptr, a[9] ? &dev : nullptr,
                                                     &level, a[11] ? &ctx : nullptr);
    rt::log("d3d11: D3D11CreateDeviceAndSwapChain(driver %u, flags %X, %u levels) -> %08X, level %X", a[1], a[3], a[5],
            uint32_t(hr), uint32_t(level));
    if (a[8]) {
        const uint32_t p = wrap(sc);
        if (p) {
            std::lock_guard lk(g_mu);
            g_swaps[p] = info;
        }
        wr32(a[8], p);
    }
    if (a[9]) wr32(a[9], wrap(dev));
    if (a[10]) wr32(a[10], uint32_t(level));
    if (a[11]) wr32(a[11], wrap(ctx));
#if defined(DIRT2_D3D11_DXVK) && defined(__SWITCH__)
    if (SUCCEEDED(hr) && sc && dev) shim::d3d11::preload_switch_shaders(dev, sc);
#endif
    return uint32_t(hr);
}

// D3D11CreateDevice(adapter, driver, software, flags, levels, nlevels, sdk, **device, *level, **context)
STDCALL(d3d11, D3D11CreateDevice, 10) {
    IDXGIAdapter* adapter = static_cast<IDXGIAdapter*>(to_host(a[0]));
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE(a[1]), nullptr, a[3], a.ptr<const D3D_FEATURE_LEVEL>(4),
                                         a[5], a[6], a[7] ? &dev : nullptr, &level, a[9] ? &ctx : nullptr);
    rt::log("d3d11: D3D11CreateDevice -> %08X, level %X", uint32_t(hr), uint32_t(level));
    if (a[7]) wr32(a[7], wrap(dev));
    if (a[8]) wr32(a[8], uint32_t(level));
    if (a[9]) wr32(a[9], wrap(ctx));
    return uint32_t(hr);
}

// CreateDXGIFactory(riid, **factory)
STDCALL(dxgi, CreateDXGIFactory, 2) {
    const GUID& iid = *reinterpret_cast<const GUID*>(rt::host_ptr(a[0]));
    void* f = nullptr;
    const HRESULT hr = CreateDXGIFactory(iid, &f);
    rt::log("dxgi: CreateDXGIFactory -> %08X", uint32_t(hr));
    if (a[1]) wr32(a[1], SUCCEEDED(hr) ? wrap_by_iid(iid, f) : 0);
    return uint32_t(hr);
}
STDCALL(dxgi, CreateDXGIFactory1, 2) {
    const GUID& iid = *reinterpret_cast<const GUID*>(rt::host_ptr(a[0]));
    void* f = nullptr;
    const HRESULT hr = CreateDXGIFactory1(iid, &f);
    if (a[1]) wr32(a[1], SUCCEEDED(hr) ? wrap_by_iid(iid, f) : 0);
    return uint32_t(hr);
}
