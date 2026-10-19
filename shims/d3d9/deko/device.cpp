// IDirect3DDevice9 on deko3d (see device.h). deko3d's default device already
// has D3D's conventions (depth 0..1, upper-left origin with NDC +Y at the top),
// so the viewport only carries D3D9's half-pixel offset; front faces are
// clockwise; depth bias and the rest follow DXVK's D3D9 (build/src/dxvk/src/d3d9).
// No <switch.h> here: libnx and DXVK's Windows headers do not mix.
#include "device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "recomp/host.h"

namespace deko {

namespace {

float as_float(DWORD v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void argb_to_float(D3DCOLOR c, float out[4]) {
    out[0] = float((c >> 16) & 0xFF) / 255.0f;
    out[1] = float((c >> 8) & 0xFF) / 255.0f;
    out[2] = float(c & 0xFF) / 255.0f;
    out[3] = float((c >> 24) & 0xFF) / 255.0f;
}

DkPrimitive primitive(D3DPRIMITIVETYPE t) {
    switch (t) {
        case D3DPT_POINTLIST: return DkPrimitive_Points;
        case D3DPT_LINELIST: return DkPrimitive_Lines;
        case D3DPT_LINESTRIP: return DkPrimitive_LineStrip;
        case D3DPT_TRIANGLESTRIP: return DkPrimitive_TriangleStrip;
        case D3DPT_TRIANGLEFAN: return DkPrimitive_TriangleFan;
        default: return DkPrimitive_Triangles;
    }
}

uint32_t vertex_count(D3DPRIMITIVETYPE t, uint32_t prims) {
    switch (t) {
        case D3DPT_POINTLIST: return prims;
        case D3DPT_LINELIST: return prims * 2;
        case D3DPT_LINESTRIP: return prims + 1;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return prims + 2;
        default: return prims * 3;
    }
}

// D3D9 blend factors 1..11 are deko3d's; the rest need mapping.
DkBlendFactor blend_factor(DWORD b) {
    switch (b) {
        case D3DBLEND_BOTHSRCALPHA: return DkBlendFactor_SrcAlpha;
        case D3DBLEND_BOTHINVSRCALPHA: return DkBlendFactor_InvSrcAlpha;
        case D3DBLEND_BLENDFACTOR: return DkBlendFactor_ConstColor;
        case D3DBLEND_INVBLENDFACTOR: return DkBlendFactor_InvConstColor;
        default: return b >= 1 && b <= 11 ? DkBlendFactor(b) : DkBlendFactor_One;
    }
}

DkWrapMode wrap_mode(DWORD a) {
    switch (a) {
        case D3DTADDRESS_MIRROR: return DkWrapMode_MirroredRepeat;
        case D3DTADDRESS_CLAMP: return DkWrapMode_ClampToEdge;
        case D3DTADDRESS_BORDER: return DkWrapMode_ClampToBorder;
        case D3DTADDRESS_MIRRORONCE: return DkWrapMode_MirrorClampToEdge;
        default: return DkWrapMode_Repeat;
    }
}

// D3DDECLTYPE -> deko3d attribute format.
bool attrib_format(BYTE type, DkVtxAttribState& a) {
    a.isBgra = 0;
    switch (type) {
        case D3DDECLTYPE_FLOAT1: a.size = DkVtxAttribSize_1x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT2: a.size = DkVtxAttribSize_2x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT3: a.size = DkVtxAttribSize_3x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT4: a.size = DkVtxAttribSize_4x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_D3DCOLOR: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Unorm; a.isBgra = 1; return true;
        case D3DDECLTYPE_UBYTE4: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Uscaled; return true;
        case D3DDECLTYPE_SHORT2: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Sscaled; return true;
        case D3DDECLTYPE_SHORT4: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Sscaled; return true;
        case D3DDECLTYPE_UBYTE4N: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Unorm; return true;
        case D3DDECLTYPE_SHORT2N: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Snorm; return true;
        case D3DDECLTYPE_SHORT4N: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Snorm; return true;
        case D3DDECLTYPE_USHORT2N: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Unorm; return true;
        case D3DDECLTYPE_USHORT4N: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Unorm; return true;
        case D3DDECLTYPE_UDEC3: a.size = DkVtxAttribSize_10_10_10_2; a.type = DkVtxAttribType_Uscaled; return true;
        case D3DDECLTYPE_DEC3N: a.size = DkVtxAttribSize_10_10_10_2; a.type = DkVtxAttribType_Snorm; return true;
        case D3DDECLTYPE_FLOAT16_2: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT16_4: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Float; return true;
        default: return false;
    }
}

TextureCore* core_of(IDirect3DBaseTexture9* t) {
    if (!t) return nullptr;
    switch (t->GetType()) {
        case D3DRTYPE_TEXTURE: return &static_cast<Texture*>(t)->core;
        case D3DRTYPE_CUBETEXTURE: return &static_cast<CubeTexture*>(t)->core;
        case D3DRTYPE_VOLUMETEXTURE: return &static_cast<VolumeTexture*>(t)->core;
        default: return nullptr;
    }
}

int sampler_index(DWORD s) { return s >= D3DVERTEXTEXTURESAMPLER0 ? int(s - D3DVERTEXTEXTURESAMPLER0) + 16 : int(s); }

constexpr uint32_t kUboSize = 8192;  // 256 float4 + 16 int4 + 16 bool registers, as MojoShader packs them

// FVF (d3dx9 helpers draw with FVFs) -> vertex elements.
std::vector<D3DVERTEXELEMENT9> fvf_elements(DWORD fvf) {
    std::vector<D3DVERTEXELEMENT9> e;
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        e.push_back(D3DVERTEXELEMENT9{0, off, type, D3DDECLMETHOD_DEFAULT, usage, index});
        off = WORD(off + size);
    };
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ: add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12); break;
        case D3DFVF_XYZRHW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16); break;
        case D3DFVF_XYZW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16); break;
        default: break;
    }
    if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    const DWORD tex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (DWORD i = 0; i < tex; ++i) {
        static const BYTE types[] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
        static const WORD sizes[] = {8, 12, 16, 4};
        const DWORD fmt = (fvf >> (16 + i * 2)) & 3;
        add(types[fmt], D3DDECLUSAGE_TEXCOORD, BYTE(i), sizes[fmt]);
    }
    e.push_back(D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0});
    return e;
}

}  // namespace

template <typename T>
void Device::set_ref(T*& slot, T* obj) {
    if (slot == obj) return;
    if (obj) obj->AddRef();
    if (slot) slot->Release();
    slot = obj;
}

Device::Device(D3D9* parent, HWND window, DWORD flags, const D3DPRESENT_PARAMETERS& pp)
    : parent_(parent), window_(window), flags_(flags), pp_(pp) {
    parent_->AddRef();
    // D3D9's default render states.
    rs_[D3DRS_ZENABLE] = pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    rs_[D3DRS_FILLMODE] = D3DFILL_SOLID;
    rs_[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs_[D3DRS_ZWRITEENABLE] = TRUE;
    rs_[D3DRS_LASTPIXEL] = TRUE;
    rs_[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs_[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs_[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs_[D3DRS_STENCILFAIL] = rs_[D3DRS_STENCILZFAIL] = rs_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs_[D3DRS_STENCILMASK] = rs_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs_[D3DRS_CCW_STENCILFAIL] = rs_[D3DRS_CCW_STENCILZFAIL] = rs_[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs_[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs_[D3DRS_COLORWRITEENABLE] = rs_[D3DRS_COLORWRITEENABLE1] = rs_[D3DRS_COLORWRITEENABLE2] =
        rs_[D3DRS_COLORWRITEENABLE3] = 0xF;
    rs_[D3DRS_BLENDOP] = rs_[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    rs_[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs_[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    rs_[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    for (auto& s : ss_) {
        s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        s[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        s[D3DSAMP_MAXANISOTROPY] = 1;
    }
}

Device::~Device() {
    dkQueueWaitIdle(gpu().queue);
    for (auto*& t : textures_) set_ref(t, static_cast<IDirect3DBaseTexture9*>(nullptr));
    for (auto& s : streams_) set_ref(s.vb, static_cast<VertexBuffer*>(nullptr));
    set_ref(indices_, static_cast<IndexBuffer*>(nullptr));
    set_ref(decl_, static_cast<VertexDeclaration*>(nullptr));
    set_ref(vs_, static_cast<VertexShader*>(nullptr));
    set_ref(ps_, static_cast<PixelShader*>(nullptr));
    for (auto*& t : targets_) set_ref(t, static_cast<Surface*>(nullptr));
    set_ref(depth_, static_cast<Surface*>(nullptr));
    if (back_buffer_) back_buffer_->Release();
    if (auto_depth_) auto_depth_->Release();
    parent_->Release();
}

bool Device::init() {
    Gpu& g = gpu();
    if (!g.device && !g.init()) {
        rt::log("deko3d: cannot create the device");
        return false;
    }
    static bool pack_loaded = false;
    if (!pack_loaded) {
        const char* data = std::getenv("DIRT2_DATA_DIR");
        pack_loaded = load_shader_pack(std::string(data ? data : ".") + "/dirt2.dkshpack");
    }
    for (Alloc& u : ubo_) u = g.buffers.alloc(kUboSize, DK_UNIFORM_BUF_ALIGNMENT);
    staging_.resize(kUboSize);
    dummy_.create(DkImageType_2D, DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, 1, DkImageFlags_Usage2DEngine);
    Alloc black = g.cur().transient.alloc(256, 256);
    std::memset(black.cpu(), 0, 256);
    upload(dummy_, 0, 0, black, 4, RECT{0, 0, 1, 1});
    make_back_buffer();
    if (std::getenv("DIRT2_DEKO_SELFTEST")) self_test();
    return back_buffer_ && back_buffer_->ok();
}

void Device::self_test() {
    Gpu& g = gpu();
    rt::log("deko3d: self-test: 180 frames of cleared swap images");
    auto t0 = std::chrono::steady_clock::now(), last = t0;
    for (int i = 1; i <= 180; ++i) {
        const int slot = g.acquire_swap_image();
        DkImageView v;
        dkImageViewDefaults(&v, &g.swap_images[slot]);
        dkCmdBufBindRenderTarget(g.cmd, &v, nullptr);
        const DkScissor sc{0, 0, g.swap_w, g.swap_h};
        dkCmdBufSetScissors(g.cmd, 0, &sc, 1);
        const float t = float(i % 60) / 60.0f;
        dkCmdBufClearColorFloat(g.cmd, 0, DkColorMask_RGBA, t, 0.5f * (1.0f - t), 1.0f - t, 1.0f);
        g.end_frame(slot);
        if (i % 30 == 0) {
            const auto now = std::chrono::steady_clock::now();
            rt::log("deko3d: self-test frame %d, %.1f ms/frame", i,
                    std::chrono::duration<double, std::milli>(now - last).count() / 30.0);
            last = now;
        }
    }
    rt::log("deko3d: self-test done in %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    dirty_ = kAll;
}

void Device::sync(const char* fmt, ...) {
    static const bool on = std::getenv("DIRT2_DEKO_SYNC") != nullptr;
    if (!on) return;
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    gpu().flush(true, what);
}

void Device::sync_after_draw(const char* kind, uint32_t prims, uint32_t instances, int base, uint32_t start) {
    ++draw_total_;
    static const bool sync = std::getenv("DIRT2_DEKO_SYNC") != nullptr;
    if (!sync) return;
    char what[256];
    std::snprintf(what, sizeof what, "draw %llu (%s: vs %s ps %s, %u primitives, %u instances, base %d, start %u, frame %u)",
                  (unsigned long long)draw_total_, kind, vs_->meta->name.c_str(), ps_->meta->name.c_str(), prims,
                  instances, base, start, present_count_);
    gpu().flush(true, what);
}

void Device::make_back_buffer() {
    if (back_buffer_) back_buffer_->Release();
    if (auto_depth_) auto_depth_->Release();
    back_buffer_ = auto_depth_ = nullptr;
    const UINT w = pp_.BackBufferWidth ? pp_.BackBufferWidth : gpu().swap_w;
    const UINT h = pp_.BackBufferHeight ? pp_.BackBufferHeight : gpu().swap_h;
    pp_.BackBufferWidth = w;
    pp_.BackBufferHeight = h;
    if (pp_.BackBufferFormat == D3DFMT_UNKNOWN) pp_.BackBufferFormat = D3DFMT_X8R8G8B8;
    back_buffer_ = new Surface(this, w, h, pp_.BackBufferFormat, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, false);
    if (pp_.EnableAutoDepthStencil)
        auto_depth_ = new Surface(this, w, h, pp_.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, false);
    for (auto*& t : targets_) set_ref(t, static_cast<Surface*>(nullptr));
    set_ref(targets_[0], back_buffer_);
    set_ref(depth_, auto_depth_);
    viewport_ = D3DVIEWPORT9{0, 0, w, h, 0.0f, 1.0f};
    scissor_ = RECT{0, 0, LONG(w), LONG(h)};
    dirty_ = kAll;
    rt::log("deko3d: back buffer %ux%u format %u, depth %u", w, h, uint32_t(pp_.BackBufferFormat),
            pp_.EnableAutoDepthStencil ? uint32_t(pp_.AutoDepthStencilFormat) : 0u);
}

// ---- device ----

HRESULT Device::GetDirect3D(IDirect3D9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    parent_->AddRef();
    *out = parent_;
    return D3D_OK;
}
HRESULT Device::GetDeviceCaps(D3DCAPS9* caps) {
    if (!caps) return D3DERR_INVALIDCALL;
    fill_caps(caps);
    return D3D_OK;
}
HRESULT Device::GetDisplayMode(UINT, D3DDISPLAYMODE* mode) { return parent_->GetAdapterDisplayMode(0, mode); }
HRESULT Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) {
    if (!p) return D3DERR_INVALIDCALL;
    p->AdapterOrdinal = 0;
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = window_;
    p->BehaviorFlags = flags_;
    return D3D_OK;
}
HRESULT Device::Reset(D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    pp_ = *pp;
    make_back_buffer();
    *pp = pp_;
    return D3D_OK;
}
HRESULT Device::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    back_buffer_->AddRef();
    *out = back_buffer_;
    return D3D_OK;
}

HRESULT Device::Present(const RECT*, const RECT*, HWND, const RGNDATA*) {
    Gpu& g = gpu();
    const int slot = g.acquire_swap_image();
    if (GpuImage* bb = back_buffer_->image()) {
        DkImageView src = bb->view(0, 0, false);
        DkImageView dst;
        dkImageViewDefaults(&dst, &g.swap_images[slot]);
        const DkImageRect sr{0, 0, 0, bb->width, bb->height, 1};
        const DkImageRect dr{0, 0, 0, g.swap_w, g.swap_h, 1};
        dkCmdBufBlitImage(g.cmd, &src, &sr, &dst, &dr, DkBlitFlag_FilterLinear, 0);
        sync("Present blit (format %d %ux%u -> swap image %d)", int(bb->format), bb->width, bb->height, slot);
    }
    g.end_frame(slot);
    dirty_ = kAll;
    // Per-frame cache maintenance: CPU writes to memory the GPU may hold in L2.
    dkCmdBufBarrier(g.cmd, DkBarrier_None, DkInvalidateFlags_L2Cache | DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
    ++present_count_;
    if (present_count_ <= 5) rt::log("deko3d: Present %u (%u draws, %u skipped so far)", present_count_, draws_, skipped_draws_);
    // Frame rate every 300 presents or 5 s, whichever comes first (loading screens present rarely).
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!fps_t0_) fps_t0_ = now;
    const uint32_t frames = present_count_ - fps_presents_;
    if (frames >= 300 || now - fps_t0_ >= 5'000'000'000ull) {
        const double secs = double(now - fps_t0_) / 1e9;
        rt::log("deko3d: %u presents, %.1f fps, %u draws/frame, %u skipped", present_count_, frames / secs,
                frames ? draws_ / frames : 0, frames ? skipped_draws_ / frames : 0);
        fps_t0_ = now;
        fps_presents_ = present_count_;
        draws_ = skipped_draws_ = 0;
    }
    return D3D_OK;
}

// ---- resources ----

HRESULT Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                              IDirect3DTexture9** out, HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* t = new Texture;
    if (!t->init(this, w, h, levels, usage, f, pool)) {
        rt::log("deko3d: CreateTexture(%ux%u, %u levels, usage %X, format %u) failed", w, h, levels, usage, uint32_t(f));
        t->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
HRESULT Device::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                    IDirect3DVolumeTexture9** out, HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* t = new VolumeTexture;
    if (!t->init(this, w, h, d, levels, usage, f, pool)) {
        t->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
HRESULT Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                  IDirect3DCubeTexture9** out, HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* t = new CubeTexture;
    if (!t->init(this, edge, levels, usage, f, pool)) {
        t->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
HRESULT Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out,
                                   HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* b = new VertexBuffer(this, length, usage, fvf, pool);
    if (!b->ok()) {
        b->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = b;
    return D3D_OK;
}
HRESULT Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9** out,
                                  HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* b = new IndexBuffer(this, length, usage, f, pool);
    if (!b->ok()) {
        b->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = b;
    return D3D_OK;
}
HRESULT Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                   IDirect3DSurface9** out, HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* s = new Surface(this, w, h, f, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, false);
    if (!s->ok()) {
        s->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = s;
    return D3D_OK;
}
HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                          IDirect3DSurface9** out, HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* s = new Surface(this, w, h, f, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, false);
    if (!s->ok()) {
        s->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = s;
    return D3D_OK;
}
HRESULT Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL pool, IDirect3DSurface9** out,
                                            HANDLE*) {
    if (!out) return D3DERR_INVALIDCALL;
    auto* s = new Surface(this, w, h, f, 0, pool, pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH);
    if (!s->ok()) {
        s->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = s;
    return D3D_OK;
}

void Device::clear_new_image(GpuImage& img, bool depth) {
    DkCmdBuf cmd = gpu().cmd;
    const uint32_t faces = img.type == DkImageType_Cubemap ? 6 : 1;
    for (uint32_t f = 0; f < faces; ++f) {
        const DkImageView v = img.view(0, f, false);
        if (depth) dkCmdBufBindRenderTargets(cmd, nullptr, 0, &v);
        else dkCmdBufBindRenderTarget(cmd, &v, nullptr);
        const DkScissor sc{0, 0, img.width, img.height};
        dkCmdBufSetScissors(cmd, 0, &sc, 1);
        if (depth) dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
        else dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    dirty_ |= kTargets | kViewport;
}

void Device::read_image(GpuImage& img, uint32_t layer, uint32_t level, const Alloc& out, uint32_t pitch_bytes,
                        const RECT& r) {
    const DkImageView v = img.view(level, layer, false);
    const DkImageRect rect{uint32_t(r.left), uint32_t(r.top), 0, uint32_t(r.right - r.left), uint32_t(r.bottom - r.top), 1};
    const DkCopyBuf dst{out.gpu(), pitch_bytes, 0};
    dkCmdBufCopyImageToBuffer(gpu().cmd, &v, &rect, &dst, 0);
    gpu().flush(true, "a render target read-back");
    dkMemBlockFlushCpuCache(out.block, out.offset, out.size);
}

// DkCopyBuf's rowLength and imageHeight are byte strides (deko3d dk_image.cpp),
// not pixel counts: a pixel pitch made the copy engine walk the wrong rows and
// hung the GPU on block-compressed uploads.
void Device::upload(GpuImage& img, uint32_t layer, uint32_t level, const Alloc& data, uint32_t pitch_bytes,
                    const RECT& r, uint32_t z, uint32_t depth, uint32_t slice_bytes) {
    const DkImageView v = img.view(level, layer, false);
    const DkImageRect rect{uint32_t(r.left), uint32_t(r.top), z, uint32_t(r.right - r.left), uint32_t(r.bottom - r.top),
                           depth};
    const DkCopyBuf src{data.gpu(), pitch_bytes, slice_bytes};
    dkCmdBufCopyBufferToImage(gpu().cmd, &src, &v, &rect, 0);
    sync("upload (image format %d %ux%ux%u, %u levels, level %u layer %u, rect %ld,%ld %ldx%ld depth %u, pitch %u bytes)",
         int(img.format), img.width, img.height, img.depth, img.levels, level, layer, long(r.left), long(r.top),
         long(r.right - r.left), long(r.bottom - r.top), depth, pitch_bytes);
}

void Device::blit(Surface* src, const RECT* sr, Surface* dst, const RECT* dr, bool linear) {
    GpuImage* si = src->image();
    GpuImage* di = dst->image();
    if (!si || !di) {
        log_unimplemented("StretchRect with a CPU surface");
        return;
    }
    const DkImageView sv = src->view(), dv = dst->view();
    const RECT s = sr ? *sr : RECT{0, 0, LONG(src->width), LONG(src->height)};
    const RECT d = dr ? *dr : RECT{0, 0, LONG(dst->width), LONG(dst->height)};
    const DkImageRect srect{uint32_t(s.left), uint32_t(s.top), 0, uint32_t(s.right - s.left), uint32_t(s.bottom - s.top), 1};
    const DkImageRect drect{uint32_t(d.left), uint32_t(d.top), 0, uint32_t(d.right - d.left), uint32_t(d.bottom - d.top), 1};
    if (srect.width == drect.width && srect.height == drect.height && si->format == di->format)
        dkCmdBufCopyImage(gpu().cmd, &sv, &srect, &dv, &drect, 0);
    else
        dkCmdBufBlitImage(gpu().cmd, &sv, &srect, &dv, &drect, linear ? DkBlitFlag_FilterLinear : 0, 0);
    sync("blit (format %d %ux%u -> format %d %ux%u)", int(si->format), srect.width, srect.height, int(di->format),
         drect.width, drect.height);
}

HRESULT Device::StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                            D3DTEXTUREFILTERTYPE filter) {
    if (!src || !dst) return D3DERR_INVALIDCALL;
    blit(static_cast<Surface*>(src), sr, static_cast<Surface*>(dst), dr, filter == D3DTEXF_LINEAR);
    return D3D_OK;
}

HRESULT Device::UpdateSurface(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const POINT* dp) {
    auto* s = static_cast<Surface*>(src);
    auto* d = static_cast<Surface*>(dst);
    if (!s || !d) return D3DERR_INVALIDCALL;
    const RECT r = sr ? *sr : RECT{0, 0, LONG(s->width), LONG(s->height)};
    const LONG x = dp ? dp->x : r.left, y = dp ? dp->y : r.top;
    const RECT dr{x, y, x + (r.right - r.left), y + (r.bottom - r.top)};
    if (s->cpu && d->image()) {
        // CPU surface -> GPU: through staging (the source may be rewritten before the GPU copies it).
        const uint32_t pitch = row_pitch(*s->fmt, uint32_t(r.right - r.left));
        const uint32_t rows = level_rows(*s->fmt, uint32_t(r.bottom - r.top));
        Alloc st = gpu().cur().transient.alloc(pitch * rows, 256);
        for (uint32_t row = 0; row < rows; ++row)
            std::memcpy(st.cpu() + row * pitch,
                        s->cpu.cpu() + (size_t(r.top) / s->fmt->block + row) * s->cpu_pitch +
                            size_t(r.left) / s->fmt->block * s->fmt->bytes,
                        pitch);
        upload(*d->image(), d->layer, d->level, st, pitch, dr);
        return D3D_OK;
    }
    blit(s, &r, d, &dr, false);
    return D3D_OK;
}

HRESULT Device::UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
    TextureCore* s = core_of(src);
    TextureCore* d = core_of(dst);
    if (!s || !d) return D3DERR_INVALIDCALL;
    const uint32_t layers = s->img.type == DkImageType_Cubemap ? 6 : 1;
    const uint32_t skip = s->img.levels > d->img.levels ? s->img.levels - d->img.levels : 0;
    for (uint32_t l = 0; l < layers; ++l)
        for (uint32_t m = 0; m < d->img.levels && m + skip < s->img.levels; ++m) {
            const DkImageView sv = s->img.view(m + skip, l, false), dv = d->img.view(m, l, false);
            const DkImageRect r{0, 0, 0, mip(d->img.width, m), mip(d->img.height, m), mip(d->img.depth, m)};
            dkCmdBufCopyImage(gpu().cmd, &sv, &r, &dv, &r, 0);
        }
    return D3D_OK;
}

HRESULT Device::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) {
    auto* s = static_cast<Surface*>(rt);
    auto* d = static_cast<Surface*>(dst);
    if (!s || !d || !s->image() || !d->cpu) return D3DERR_INVALIDCALL;
    const DkImageView v = s->view();
    const DkImageRect r{0, 0, 0, std::min(s->width, d->width), std::min(s->height, d->height), 1};
    const DkCopyBuf out{d->cpu.gpu(), d->cpu_pitch, 0};  // byte pitch
    dkCmdBufCopyImageToBuffer(gpu().cmd, &v, &r, &out, 0);
    // The game reads it right away: submit and wait (a stall; the luminance
    // read-back does this every frame, see docs/SWITCH.md).
    gpu().flush(true);
    return D3D_OK;
}

HRESULT Device::ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
    auto* s = static_cast<Surface*>(surface);
    if (!s || !s->image()) return D3DERR_INVALIDCALL;
    DkCmdBuf cmd = gpu().cmd;
    const DkImageView v = s->view();
    dkCmdBufBindRenderTarget(cmd, &v, nullptr);
    const RECT r = rect ? *rect : RECT{0, 0, LONG(s->width), LONG(s->height)};
    const DkScissor sc{uint32_t(r.left), uint32_t(r.top), uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    float c[4];
    argb_to_float(color, c);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
    dirty_ |= kTargets | kViewport;
    sync("ColorFill (format %d %ux%u)", int(s->image()->format), s->width, s->height);
    return D3D_OK;
}

// ---- targets and viewport ----

HRESULT Device::SetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    if (index >= 4 || (index == 0 && !surface)) return D3DERR_INVALIDCALL;
    set_ref(targets_[index], static_cast<Surface*>(surface));
    if (index == 0 && surface) {  // D3D9 resets the viewport to the new target
        auto* s = static_cast<Surface*>(surface);
        viewport_ = D3DVIEWPORT9{0, 0, s->width, s->height, 0.0f, 1.0f};
        scissor_ = RECT{0, 0, LONG(s->width), LONG(s->height)};
        dirty_ |= kViewport;
    }
    dirty_ |= kTargets;
    return D3D_OK;
}
HRESULT Device::GetRenderTarget(DWORD index, IDirect3DSurface9** out) {
    if (index >= 4 || !out) return D3DERR_INVALIDCALL;
    *out = targets_[index];
    if (!*out) return D3DERR_NOTFOUND;
    (*out)->AddRef();
    return D3D_OK;
}
HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9* surface) {
    set_ref(depth_, static_cast<Surface*>(surface));
    dirty_ |= kTargets | kDepthStencil | kRaster;
    return D3D_OK;
}
HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = depth_;
    if (!depth_) return D3DERR_NOTFOUND;
    depth_->AddRef();
    return D3D_OK;
}

HRESULT Device::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    Gpu& g = gpu();
    if (list_seen_ != g.list_id) {
        list_seen_ = g.list_id;
        dirty_ = kAll;
    }
    if (dirty_ & kTargets) flush_targets();
    DkCmdBuf cmd = g.cmd;
    // D3D9 clears the viewport, or each rectangle within it.
    const RECT vp{LONG(viewport_.X), LONG(viewport_.Y), LONG(viewport_.X + viewport_.Width), LONG(viewport_.Y + viewport_.Height)};
    std::vector<RECT> areas;
    if (count && rects)
        for (DWORD i = 0; i < count; ++i)
            areas.push_back(RECT{std::max(vp.left, rects[i].x1), std::max(vp.top, rects[i].y1),
                                 std::min(vp.right, rects[i].x2), std::min(vp.bottom, rects[i].y2)});
    else
        areas.push_back(vp);
    float c[4];
    argb_to_float(color, c);
    for (const RECT& r : areas) {
        if (r.right <= r.left || r.bottom <= r.top) continue;
        const DkScissor sc{uint32_t(r.left), uint32_t(r.top), uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
        dkCmdBufSetScissors(cmd, 0, &sc, 1);
        if (flags & D3DCLEAR_TARGET)
            for (uint32_t i = 0; i < 4; ++i)
                if (targets_[i] && targets_[i]->image()) dkCmdBufClearColorFloat(cmd, i, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depth_)
            dkCmdBufClearDepthStencil(cmd, (flags & D3DCLEAR_ZBUFFER) != 0, z, (flags & D3DCLEAR_STENCIL) ? 0xFF : 0,
                                      uint8_t(stencil));
    }
    dirty_ |= kViewport;  // the scissor
    sync("Clear (flags %lX, %lu rects, target format %d)", (unsigned long)flags, (unsigned long)count,
         targets_[0] && targets_[0]->image() ? int(targets_[0]->image()->format) : -1);
    return D3D_OK;
}

HRESULT Device::SetViewport(const D3DVIEWPORT9* vp) {
    if (!vp) return D3DERR_INVALIDCALL;
    viewport_ = *vp;
    dirty_ |= kViewport;
    return D3D_OK;
}
HRESULT Device::GetViewport(D3DVIEWPORT9* vp) {
    if (!vp) return D3DERR_INVALIDCALL;
    *vp = viewport_;
    return D3D_OK;
}
HRESULT Device::SetScissorRect(const RECT* r) {
    if (!r) return D3DERR_INVALIDCALL;
    scissor_ = *r;
    dirty_ |= kViewport;
    return D3D_OK;
}
HRESULT Device::GetScissorRect(RECT* r) {
    if (!r) return D3DERR_INVALIDCALL;
    *r = scissor_;
    return D3D_OK;
}

// ---- state ----

HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    if (uint32_t(state) >= 256) return D3DERR_INVALIDCALL;
    if (rs_[state] == value) return D3D_OK;
    rs_[state] = value;
    switch (state) {
        case D3DRS_FILLMODE: case D3DRS_CULLMODE: case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
            dirty_ |= kRaster;
            break;
        case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP:
        case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA:
        case D3DRS_BLENDOPALPHA: case D3DRS_COLORWRITEENABLE: case D3DRS_COLORWRITEENABLE1:
        case D3DRS_COLORWRITEENABLE2: case D3DRS_COLORWRITEENABLE3: case D3DRS_ALPHATESTENABLE:
        case D3DRS_ALPHAFUNC: case D3DRS_ALPHAREF: case D3DRS_BLENDFACTOR:
            dirty_ |= kBlend;
            break;
        case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC: case D3DRS_STENCILENABLE:
        case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC:
        case D3DRS_STENCILREF: case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_TWOSIDEDSTENCILMODE:
        case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC:
            dirty_ |= kDepthStencil;
            break;
        case D3DRS_SCISSORTESTENABLE:
            dirty_ |= kViewport;
            break;
        case D3DRS_SRGBWRITEENABLE:
            if (value) log_unimplemented("D3DRS_SRGBWRITEENABLE");
            break;
        default:
            break;
    }
    return D3D_OK;
}
HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    if (!value || uint32_t(state) >= 256) return D3DERR_INVALIDCALL;
    *value = rs_[state];
    return D3D_OK;
}
HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    const int s = sampler_index(sampler);
    if (s < 0 || s >= kSamplers || uint32_t(type) >= 14) return D3DERR_INVALIDCALL;
    if (ss_[s][type] != value) {
        ss_[s][type] = value;
        dirty_ |= kTextures;
        if (type == D3DSAMP_SRGBTEXTURE && value) log_unimplemented("D3DSAMP_SRGBTEXTURE");
    }
    return D3D_OK;
}
HRESULT Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) {
    const int s = sampler_index(sampler);
    if (!value || s < 0 || s >= kSamplers || uint32_t(type) >= 14) return D3DERR_INVALIDCALL;
    *value = ss_[s][type];
    return D3D_OK;
}
HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
    const int s = sampler_index(stage);
    if (s < 0 || s >= kSamplers) return D3DERR_INVALIDCALL;
    if (textures_[s] != texture) {
        set_ref(textures_[s], texture);
        dirty_ |= kTextures;
    }
    return D3D_OK;
}
HRESULT Device::GetTexture(DWORD stage, IDirect3DBaseTexture9** out) {
    const int s = sampler_index(stage);
    if (!out || s < 0 || s >= kSamplers) return D3DERR_INVALIDCALL;
    *out = textures_[s];
    if (*out) (*out)->AddRef();
    return D3D_OK;
}
HRESULT Device::SetFVF(DWORD fvf) {
    if (!fvf) return D3D_OK;
    VertexDeclaration*& d = fvf_decls_[fvf];
    if (!d) d = new VertexDeclaration(this, fvf_elements(fvf).data());
    return SetVertexDeclaration(d);
}
HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) {
    if (!elements || !out) return D3DERR_INVALIDCALL;
    *out = new VertexDeclaration(this, elements);
    return D3D_OK;
}
HRESULT Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) {
    if (decl_ != decl) {
        set_ref(decl_, static_cast<VertexDeclaration*>(decl));
        dirty_ |= kVertexInput;
    }
    return D3D_OK;
}
HRESULT Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = decl_;
    if (decl_) decl_->AddRef();
    return D3D_OK;
}
HRESULT Device::CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out) {
    if (!code || !out) return D3DERR_INVALIDCALL;
    uint32_t bytes;
    const PackMeta* m;
    const DkShader* sh;
    find_shader(code, &bytes, &m, &sh);
    *out = new VertexShader(this, code, bytes, m, sh);
    return D3D_OK;
}
HRESULT Device::SetVertexShader(IDirect3DVertexShader9* shader) {
    if (vs_ != shader) {
        set_ref(vs_, static_cast<VertexShader*>(shader));
        dirty_ |= kShaders | kVertexInput | kTextures | kVsConst;
    }
    return D3D_OK;
}
HRESULT Device::GetVertexShader(IDirect3DVertexShader9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = vs_;
    if (vs_) vs_->AddRef();
    return D3D_OK;
}
HRESULT Device::CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out) {
    if (!code || !out) return D3DERR_INVALIDCALL;
    uint32_t bytes;
    const PackMeta* m;
    const DkShader* sh;
    find_shader(code, &bytes, &m, &sh);
    *out = new PixelShader(this, code, bytes, m, sh);
    return D3D_OK;
}
HRESULT Device::SetPixelShader(IDirect3DPixelShader9* shader) {
    if (ps_ != shader) {
        set_ref(ps_, static_cast<PixelShader*>(shader));
        dirty_ |= kShaders | kTextures | kPsConst;
    }
    return D3D_OK;
}
HRESULT Device::GetPixelShader(IDirect3DPixelShader9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = ps_;
    if (ps_) ps_->AddRef();
    return D3D_OK;
}

#define CONSTANT_SETTER(Name, Array, Limit, Type, Width, Flag)                       \
    HRESULT Device::Name(UINT reg, const Type* data, UINT count) {                  \
        if (!data || reg + count > Limit) return D3DERR_INVALIDCALL;                \
        std::memcpy(&Array[reg], data, sizeof(Type) * Width * count);               \
        dirty_ |= Flag;                                                             \
        return D3D_OK;                                                              \
    }
CONSTANT_SETTER(SetVertexShaderConstantF, vsf_, 256, float, 4, kVsConst)
CONSTANT_SETTER(SetVertexShaderConstantI, vsi_, 16, int, 4, kVsConst)
CONSTANT_SETTER(SetVertexShaderConstantB, vsb_, 16, BOOL, 1, kVsConst)
CONSTANT_SETTER(SetPixelShaderConstantF, psf_, 224, float, 4, kPsConst)
CONSTANT_SETTER(SetPixelShaderConstantI, psi_, 16, int, 4, kPsConst)
CONSTANT_SETTER(SetPixelShaderConstantB, psb_, 16, BOOL, 1, kPsConst)
#undef CONSTANT_SETTER

HRESULT Device::GetVertexShaderConstantF(UINT reg, float* data, UINT count) {
    if (!data || reg + count > 256) return D3DERR_INVALIDCALL;
    std::memcpy(data, &vsf_[reg], 16 * count);
    return D3D_OK;
}
HRESULT Device::GetPixelShaderConstantF(UINT reg, float* data, UINT count) {
    if (!data || reg + count > 224) return D3DERR_INVALIDCALL;
    std::memcpy(data, &psf_[reg], 16 * count);
    return D3D_OK;
}

HRESULT Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
    if (stream >= kStreams) return D3DERR_INVALIDCALL;
    Stream& s = streams_[stream];
    set_ref(s.vb, static_cast<VertexBuffer*>(vb));
    if (s.stride != stride) dirty_ |= kVertexInput;
    s.offset = offset;
    s.stride = stride;
    return D3D_OK;
}
HRESULT Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** vb, UINT* offset, UINT* stride) {
    if (stream >= kStreams || !vb || !offset || !stride) return D3DERR_INVALIDCALL;
    const Stream& s = streams_[stream];
    *vb = s.vb;
    if (s.vb) s.vb->AddRef();
    *offset = s.offset;
    *stride = s.stride;
    return D3D_OK;
}
HRESULT Device::SetStreamSourceFreq(UINT stream, UINT divider) {
    if (stream >= kStreams) return D3DERR_INVALIDCALL;
    if (streams_[stream].freq != divider) {
        streams_[stream].freq = divider;
        dirty_ |= kVertexInput;
    }
    return D3D_OK;
}
HRESULT Device::SetIndices(IDirect3DIndexBuffer9* ib) {
    set_ref(indices_, static_cast<IndexBuffer*>(ib));
    return D3D_OK;
}
HRESULT Device::GetIndices(IDirect3DIndexBuffer9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = indices_;
    if (indices_) indices_->AddRef();
    return D3D_OK;
}

// ---- state -> deko3d ----

void Device::flush_targets() {
    DkImageView views[4];
    DkImageView const* ptrs[4];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!targets_[i] || !targets_[i]->image()) break;  // targets are contiguous from 0
        views[i] = targets_[i]->view();
        ptrs[i] = &views[i];
        n = i + 1;
    }
    DkImageView dv;
    const bool depth = depth_ && depth_->image();
    if (depth) dv = depth_->view();
    dkCmdBufBindRenderTargets(gpu().cmd, ptrs, n, depth ? &dv : nullptr);
}

void Device::flush_viewport() {
    const D3DVIEWPORT9& vp = viewport_;
    const float zbias = vp.MinZ >= 0.5f ? 0.0f : 0.001f;  // as DXVK: no degenerate depth range
    // deko3d's upper-left origin is D3D's convention (it already maps NDC +Y to
    // the top); DXVK's Vulkan-style negative height would flip twice and wrap
    // the viewport rectangle. Only D3D9's half-pixel offset is added.
    const DkViewport v{float(vp.X) + 0.5f, float(vp.Y) + 0.5f, float(vp.Width), float(vp.Height),
                       std::clamp(vp.MinZ, 0.0f, 1.0f), std::clamp(std::max(vp.MaxZ, vp.MinZ + zbias), 0.0f, 1.0f)};
    dkCmdBufSetViewports(gpu().cmd, 0, &v, 1);
    RECT r{LONG(vp.X), LONG(vp.Y), LONG(vp.X + vp.Width), LONG(vp.Y + vp.Height)};
    if (rs_[D3DRS_SCISSORTESTENABLE]) {
        r.left = std::max(r.left, scissor_.left);
        r.top = std::max(r.top, scissor_.top);
        r.right = std::max(r.left, std::min(r.right, scissor_.right));
        r.bottom = std::max(r.top, std::min(r.bottom, scissor_.bottom));
    }
    const DkScissor s{uint32_t(std::max<LONG>(r.left, 0)), uint32_t(std::max<LONG>(r.top, 0)),
                      uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
    dkCmdBufSetScissors(gpu().cmd, 0, &s, 1);
}

void Device::flush_raster_state() {
    DkRasterizerState r;
    dkRasterizerStateDefaults(&r);
    const DWORD fill = rs_[D3DRS_FILLMODE];
    r.polygonModeFront = r.polygonModeBack =
        fill == D3DFILL_POINT ? DkPolygonMode_Point : fill == D3DFILL_WIREFRAME ? DkPolygonMode_Line : DkPolygonMode_Fill;
    r.frontFace = DkFrontFace_CW;
    const DWORD cull = rs_[D3DRS_CULLMODE];
    r.cullMode = cull == D3DCULL_CW ? DkFace_Front : cull == D3DCULL_CCW ? DkFace_Back : DkFace_None;
    const float bias = as_float(rs_[D3DRS_DEPTHBIAS]), slope = as_float(rs_[D3DRS_SLOPESCALEDEPTHBIAS]);
    r.depthBiasEnableMask = (bias != 0.0f || slope != 0.0f) ? DkPolygonFlag_All : 0;
    dkCmdBufBindRasterizerState(gpu().cmd, &r);
    if (r.depthBiasEnableMask) {
        // D3D9's bias is in depth units; deko3d's in the format's smallest step.
        const float scale = depth_ && depth_->fmt && depth_->fmt->dk == DkImageFormat_Z16 ? float(1 << 16) : float(1 << 24);
        dkCmdBufSetDepthBias(gpu().cmd, bias * scale, 0.0f, slope);
    }
}

void Device::flush_blend_state() {
    DkCmdBuf cmd = gpu().cmd;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = rs_[D3DRS_ALPHABLENDENABLE] ? 0xFF : 0;
    cs.alphaCompareOp = rs_[D3DRS_ALPHATESTENABLE] ? DkCompareOp(rs_[D3DRS_ALPHAFUNC]) : DkCompareOp_Always;
    dkCmdBufBindColorState(cmd, &cs);
    if (rs_[D3DRS_ALPHATESTENABLE]) dkCmdBufSetAlphaRef(cmd, float(rs_[D3DRS_ALPHAREF] & 0xFF) / 255.0f);

    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    const D3DRENDERSTATETYPE masks[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
                                         D3DRS_COLORWRITEENABLE3};
    for (uint32_t i = 0; i < 4; ++i) dkColorWriteStateSetMask(&cw, i, rs_[masks[i]] & 0xF);
    dkCmdBufBindColorWriteState(cmd, &cw);

    if (rs_[D3DRS_ALPHABLENDENABLE]) {
        DkBlendState b;
        dkBlendStateDefaults(&b);
        DWORD src = rs_[D3DRS_SRCBLEND], dst = rs_[D3DRS_DESTBLEND];
        if (src == D3DBLEND_BOTHSRCALPHA) dst = D3DBLEND_INVSRCALPHA;
        if (src == D3DBLEND_BOTHINVSRCALPHA) dst = D3DBLEND_SRCALPHA;
        const bool sep = rs_[D3DRS_SEPARATEALPHABLENDENABLE];
        const DWORD asrc = sep ? rs_[D3DRS_SRCBLENDALPHA] : src, adst = sep ? rs_[D3DRS_DESTBLENDALPHA] : dst;
        dkBlendStateSetOps(&b, DkBlendOp(rs_[D3DRS_BLENDOP]), DkBlendOp(sep ? rs_[D3DRS_BLENDOPALPHA] : rs_[D3DRS_BLENDOP]));
        dkBlendStateSetFactors(&b, blend_factor(src), blend_factor(dst), blend_factor(asrc), blend_factor(adst));
        const DkBlendState all[4] = {b, b, b, b};
        dkCmdBufBindBlendStates(cmd, 0, all, 4);
        float c[4];
        argb_to_float(rs_[D3DRS_BLENDFACTOR], c);
        dkCmdBufSetBlendConst(cmd, c[0], c[1], c[2], c[3]);
    }
}

void Device::flush_depth_stencil() {
    DkDepthStencilState d;
    dkDepthStencilStateDefaults(&d);
    const bool has_depth = depth_ && depth_->image();
    d.depthTestEnable = has_depth && rs_[D3DRS_ZENABLE] != D3DZB_FALSE;
    d.depthWriteEnable = has_depth && rs_[D3DRS_ZWRITEENABLE];
    d.depthCompareOp = DkCompareOp(rs_[D3DRS_ZFUNC]);
    d.stencilTestEnable = has_depth && rs_[D3DRS_STENCILENABLE];
    // D3D9's stencil ops and compare functions are deko3d's values.
    d.stencilFrontFailOp = DkStencilOp(rs_[D3DRS_STENCILFAIL]);
    d.stencilFrontDepthFailOp = DkStencilOp(rs_[D3DRS_STENCILZFAIL]);
    d.stencilFrontPassOp = DkStencilOp(rs_[D3DRS_STENCILPASS]);
    d.stencilFrontCompareOp = DkCompareOp(rs_[D3DRS_STENCILFUNC]);
    const bool two = rs_[D3DRS_TWOSIDEDSTENCILMODE];
    d.stencilBackFailOp = DkStencilOp(rs_[two ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL]);
    d.stencilBackDepthFailOp = DkStencilOp(rs_[two ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL]);
    d.stencilBackPassOp = DkStencilOp(rs_[two ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS]);
    d.stencilBackCompareOp = DkCompareOp(rs_[two ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC]);
    dkCmdBufBindDepthStencilState(gpu().cmd, &d);
    if (d.stencilTestEnable)
        dkCmdBufSetStencil(gpu().cmd, DkFace_FrontAndBack, uint8_t(rs_[D3DRS_STENCILWRITEMASK]), uint8_t(rs_[D3DRS_STENCILREF]),
                           uint8_t(rs_[D3DRS_STENCILMASK]));
}

bool Device::flush_vertex_input(uint32_t* instances) {
    DkCmdBuf cmd = gpu().cmd;
    // Instancing: stream 0 says how many instances, streams with INSTANCEDATA step per instance.
    *instances = 1;
    const bool instanced = (streams_[0].freq & D3DSTREAMSOURCE_INDEXEDDATA) != 0;
    if (instanced) *instances = std::max<UINT>(streams_[0].freq & 0x3FFFFFFF, 1);
    uint32_t nbuf = up_.active ? 1 : 0;
    if (dirty_ & (kVertexInput | kShaders)) {
        if (!decl_) {
            log_unimplemented("draw without a vertex declaration");
            return false;
        }
        DkVtxAttribState attrs[16] = {};
        uint32_t nattr = 0;
        for (const PackMeta::Input& in : vs_->meta->inputs) {
            if (in.reg >= 16) continue;
            DkVtxAttribState& a = attrs[in.reg];
            const D3DVERTEXELEMENT9* e = nullptr;
            for (const D3DVERTEXELEMENT9& x : decl_->elements)
                if (x.Usage == in.usage && x.UsageIndex == in.index) {
                    e = &x;
                    break;
                }
            if (e && attrib_format(e->Type, a)) {
                a.bufferId = e->Stream;
                a.offset = e->Offset;
            } else {
                a.isFixed = 1;  // the shader reads (0, 0, 0, 1)
                a.size = DkVtxAttribSize_4x32;
                a.type = DkVtxAttribType_Float;
            }
            nattr = std::max<uint32_t>(nattr, in.reg + 1u);
        }
        for (uint32_t i = 0; i < nattr; ++i)
            if (!attrs[i].size) {
                attrs[i].isFixed = 1;
                attrs[i].size = DkVtxAttribSize_4x32;
                attrs[i].type = DkVtxAttribType_Float;
            }
        DkVtxBufferState bufs[kStreams] = {};
        for (const D3DVERTEXELEMENT9& x : decl_->elements) nbuf = std::max<uint32_t>(nbuf, x.Stream + 1u);
        for (uint32_t i = 0; i < nbuf && i < kStreams; ++i) {
            bufs[i].stride = (up_.active && i == 0) ? up_.stride : streams_[i].stride;
            bufs[i].divisor = instanced && (streams_[i].freq & D3DSTREAMSOURCE_INSTANCEDATA)
                                  ? std::max<UINT>(streams_[i].freq & 0x3FFFFFFF, 1)
                                  : 0;
        }
        dkCmdBufBindVtxAttribState(cmd, attrs, nattr);
        dkCmdBufBindVtxBufferState(cmd, bufs, nbuf);
    } else {
        for (const D3DVERTEXELEMENT9& x : decl_->elements) nbuf = std::max<uint32_t>(nbuf, x.Stream + 1u);
    }
    // Buffer addresses change when a lock renames them: bind on every draw.
    for (uint32_t i = 0; i < nbuf && i < kStreams; ++i) {
        if (up_.active && i == 0) {
            dkCmdBufBindVtxBuffer(cmd, 0, up_.addr, up_.size);
            continue;
        }
        Stream& s = streams_[i];
        if (!s.vb) continue;
        dkCmdBufBindVtxBuffer(cmd, i, s.vb->store.addr() + s.offset, s.vb->store.size - std::min(s.offset, s.vb->store.size));
        s.vb->store.used();
    }
    return true;
}

int Device::sampler_id(int s) {
    const DWORD* st = ss_[s];
    const TextureCore* t = core_of(textures_[s]);
    const std::array<DWORD, 10> key = {st[D3DSAMP_ADDRESSU], st[D3DSAMP_ADDRESSV], st[D3DSAMP_ADDRESSW],
                                       st[D3DSAMP_BORDERCOLOR], st[D3DSAMP_MAGFILTER], st[D3DSAMP_MINFILTER],
                                       st[D3DSAMP_MIPFILTER], st[D3DSAMP_MIPMAPLODBIAS],
                                       std::max<DWORD>(st[D3DSAMP_MAXMIPLEVEL], t ? t->lod : 0), st[D3DSAMP_MAXANISOTROPY]};
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    DkSampler smp;
    dkSamplerDefaults(&smp);
    auto filter = [](DWORD f) { return f == D3DTEXF_POINT || f == D3DTEXF_NONE ? DkFilter_Nearest : DkFilter_Linear; };
    smp.magFilter = filter(key[4]);
    smp.minFilter = filter(key[5]);
    smp.mipFilter = key[6] == D3DTEXF_NONE ? DkMipFilter_None : key[6] == D3DTEXF_POINT ? DkMipFilter_Nearest : DkMipFilter_Linear;
    smp.wrapMode[0] = wrap_mode(key[0]);
    smp.wrapMode[1] = wrap_mode(key[1]);
    smp.wrapMode[2] = wrap_mode(key[2]);
    smp.lodBias = as_float(key[7]);
    smp.lodClampMin = float(key[8]);
    if (key[4] == D3DTEXF_ANISOTROPIC || key[5] == D3DTEXF_ANISOTROPIC)
        smp.maxAnisotropy = float(std::clamp<DWORD>(key[9], 1, 16));
    float border[4];
    argb_to_float(key[3], border);
    for (int i = 0; i < 4; ++i) smp.borderColor[i].value_f = border[i];
    Gpu& g = gpu();
    const int id = g.sampler_ids.take();
    dkSamplerDescriptorInitialize(&g.sampler_desc()[id], &smp);
    g.descriptors_dirty = true;
    samplers_[key] = id;
    return id;
}

void Device::flush_textures() {
    DkCmdBuf cmd = gpu().cmd;
    auto bind_stage = [&](const PackMeta* m, DkStage stage, int base) {
        for (const PackMeta::Sampler& s : m->samplers) {
            const int idx = base + s.reg;
            if (idx >= kSamplers) continue;
            TextureCore* t = core_of(textures_[idx]);
            const int image = t && t->img.mem ? t->img.descriptor() : dummy_.descriptor();
            const DkResHandle h = dkMakeTextureHandle(uint32_t(image), uint32_t(sampler_id(idx)));
            dkCmdBufBindTextures(cmd, stage, s.reg, &h, 1);
        }
    };
    bind_stage(ps_->meta, DkStage_Fragment, 0);
    bind_stage(vs_->meta, DkStage_Vertex, 16);
}

void Device::flush_constants() {
    DkCmdBuf cmd = gpu().cmd;
    auto push = [&](const PackMeta* m, int stage, const float (*f)[4], const int (*iv)[4], const BOOL* b) {
        if (!m->block_bytes || m->block_bytes > kUboSize) return;
        uint8_t* out = staging_.data();
        uint32_t off = 0;
        for (const PackMeta::Uniform& u : m->uniforms) {
            const uint32_t n = std::max<uint32_t>(u.count, 1);
            if (u.type == 0) std::memcpy(out + off, f[u.reg], 16 * n);
            else if (u.type == 1) std::memcpy(out + off, iv[u.reg], 16 * n);
            else
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t v = b[u.reg + j] ? 1 : 0;
                    std::memcpy(out + off + j * 16, &v, 4);
                }
            off += 16 * n;
        }
        dkCmdBufPushConstants(cmd, ubo_[stage].gpu(), kUboSize, 0, off, out);
    };
    if (dirty_ & (kVsConst | kShaders)) push(vs_->meta, 0, vsf_, vsi_, vsb_);
    if (dirty_ & (kPsConst | kShaders)) push(ps_->meta, 1, psf_, psi_, psb_);
}

bool Device::prepare_draw(D3DPRIMITIVETYPE, uint32_t* instances) {
    if (!vs_ || !ps_ || !vs_->shader || !ps_->shader) {
        ++skipped_draws_;
        return false;
    }
    Gpu& g = gpu();
    DkCmdBuf cmd = g.cmd;
    if (list_seen_ != g.list_id) {
        list_seen_ = g.list_id;
        dirty_ = kAll;
    }
    if (dirty_ == kAll) {
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, ubo_[0].gpu(), kUboSize);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo_[1].gpu(), kUboSize);
    }
    if (dirty_ & kTargets) flush_targets();
    if (dirty_ & kViewport) flush_viewport();
    if (dirty_ & kShaders) {
        DkShader const* shaders[2] = {vs_->shader, ps_->shader};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    }
    if (dirty_ & kRaster) flush_raster_state();
    if (dirty_ & kBlend) flush_blend_state();
    if (dirty_ & kDepthStencil) flush_depth_stencil();
    if (!flush_vertex_input(instances)) return false;
    if (dirty_ & (kTextures | kShaders)) flush_textures();
    if (dirty_ & (kVsConst | kPsConst | kShaders)) flush_constants();
    if (g.descriptors_dirty) {
        dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        g.descriptors_dirty = false;
    }
    dirty_ = 0;
    ++draws_;
    return true;
}

// ---- draws ----

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
    uint32_t instances;
    if (!prepare_draw(type, &instances)) return D3D_OK;
    dkCmdBufDraw(gpu().cmd, primitive(type), vertex_count(type, count), 1, start, 0);
    sync_after_draw("DrawPrimitive", count, 1, 0, start);
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT, UINT, UINT start, UINT count) {
    if (!indices_) return D3DERR_INVALIDCALL;
    uint32_t instances;
    if (!prepare_draw(type, &instances)) return D3D_OK;
    DkCmdBuf cmd = gpu().cmd;
    dkCmdBufBindIdxBuffer(cmd, indices_->format == D3DFMT_INDEX32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16,
                          indices_->store.addr());
    indices_->store.used();
    dkCmdBufDrawIndexed(cmd, primitive(type), vertex_count(type, count), instances, start, base, 0);
    sync_after_draw("DrawIndexedPrimitive", count, instances, base, start);
    return D3D_OK;
}

HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    if (!data) return D3DERR_INVALIDCALL;
    const uint32_t bytes = vertex_count(type, count) * stride;
    Alloc a = gpu().cur().transient.alloc(bytes, 16);
    std::memcpy(a.cpu(), data, bytes);
    up_ = {true, a.gpu(), bytes, stride};
    dirty_ |= kVertexInput;
    uint32_t instances;
    if (prepare_draw(type, &instances)) {
        dkCmdBufDraw(gpu().cmd, primitive(type), vertex_count(type, count), 1, 0, 0);
        sync_after_draw("DrawPrimitiveUP", count, 1, 0, 0);
    }
    up_.active = false;
    // D3D9 unbinds stream 0 after a UP draw.
    set_ref(streams_[0].vb, static_cast<VertexBuffer*>(nullptr));
    streams_[0].offset = streams_[0].stride = 0;
    dirty_ |= kVertexInput;
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count,
                                       const void* indices, D3DFORMAT index_format, const void* data, UINT stride) {
    if (!data || !indices) return D3DERR_INVALIDCALL;
    const uint32_t vbytes = (min_index + vertices) * stride;
    const uint32_t isize = index_format == D3DFMT_INDEX32 ? 4 : 2, n = vertex_count(type, count);
    Alloc v = gpu().cur().transient.alloc(vbytes, 16);
    Alloc ix = gpu().cur().transient.alloc(n * isize, 16);
    std::memcpy(v.cpu(), data, vbytes);
    std::memcpy(ix.cpu(), indices, n * isize);
    up_ = {true, v.gpu(), vbytes, stride};
    dirty_ |= kVertexInput;
    uint32_t instances;
    if (prepare_draw(type, &instances)) {
        dkCmdBufBindIdxBuffer(gpu().cmd, isize == 4 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, ix.gpu());
        dkCmdBufDrawIndexed(gpu().cmd, primitive(type), n, 1, 0, 0, 0);
        sync_after_draw("DrawIndexedPrimitiveUP", count, 1, 0, 0);
    }
    up_.active = false;
    set_ref(streams_[0].vb, static_cast<VertexBuffer*>(nullptr));
    set_ref(indices_, static_cast<IndexBuffer*>(nullptr));
    streams_[0].offset = streams_[0].stride = 0;
    dirty_ |= kVertexInput;
    return D3D_OK;
}

}  // namespace deko
