// D3D9 resources on deko3d: formats, vertex/index buffers, textures, surfaces.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

#include "device.h"
#include "recomp/host.h"

namespace deko {

void log_unimplemented(const char* method) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard lk(mu);
    if (seen.insert(method).second) rt::log("deko3d: %s is not implemented", method);
}

// ---- Formats ----

namespace {

constexpr D3DFORMAT fourcc(char a, char b, char c, char d) {
    return D3DFORMAT(uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24);
}

FormatInfo make(DkImageFormat f, uint8_t block, uint8_t bytes, bool depth = false,
                std::array<DkImageSwizzle, 4> sw = {DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue,
                                                    DkImageSwizzle_Alpha}) {
    FormatInfo i;
    i.dk = f;
    i.block = block;
    i.bytes = bytes;
    i.depth = depth;
    for (int k = 0; k < 4; ++k) i.swizzle[k] = sw[k];
    return i;
}

}  // namespace

const FormatInfo* format_info(D3DFORMAT f) {
    using S = DkImageSwizzle;
    static const std::unordered_map<uint32_t, FormatInfo> table = {
        {D3DFMT_A8R8G8B8, make(DkImageFormat_BGRA8_Unorm, 1, 4)},
        {D3DFMT_X8R8G8B8, make(DkImageFormat_BGRX8_Unorm, 1, 4)},
        {D3DFMT_A8B8G8R8, make(DkImageFormat_RGBA8_Unorm, 1, 4)},
        {D3DFMT_X8B8G8R8, make(DkImageFormat_RGBX8_Unorm, 1, 4)},
        {D3DFMT_A2B10G10R10, make(DkImageFormat_RGB10A2_Unorm, 1, 4)},
        {D3DFMT_A2R10G10B10, make(DkImageFormat_RGB10A2_Unorm, 1, 4, false,
                                  {S::DkImageSwizzle_Blue, S::DkImageSwizzle_Green, S::DkImageSwizzle_Red, S::DkImageSwizzle_Alpha})},
        {D3DFMT_R5G6B5, make(DkImageFormat_BGR565_Unorm, 1, 2)},
        {D3DFMT_A8, make(DkImageFormat_R8_Unorm, 1, 1, false,
                         {S::DkImageSwizzle_Zero, S::DkImageSwizzle_Zero, S::DkImageSwizzle_Zero, S::DkImageSwizzle_Red})},
        {D3DFMT_L8, make(DkImageFormat_R8_Unorm, 1, 1, false,
                         {S::DkImageSwizzle_Red, S::DkImageSwizzle_Red, S::DkImageSwizzle_Red, S::DkImageSwizzle_One})},
        {D3DFMT_A8L8, make(DkImageFormat_RG8_Unorm, 1, 2, false,
                           {S::DkImageSwizzle_Red, S::DkImageSwizzle_Red, S::DkImageSwizzle_Red, S::DkImageSwizzle_Green})},
        {D3DFMT_L16, make(DkImageFormat_R16_Unorm, 1, 2, false,
                          {S::DkImageSwizzle_Red, S::DkImageSwizzle_Red, S::DkImageSwizzle_Red, S::DkImageSwizzle_One})},
        {D3DFMT_G16R16, make(DkImageFormat_RG16_Unorm, 1, 4)},
        {D3DFMT_A16B16G16R16, make(DkImageFormat_RGBA16_Unorm, 1, 8)},
        {D3DFMT_R16F, make(DkImageFormat_R16_Float, 1, 2)},
        {D3DFMT_G16R16F, make(DkImageFormat_RG16_Float, 1, 4)},
        {D3DFMT_A16B16G16R16F, make(DkImageFormat_RGBA16_Float, 1, 8)},
        {D3DFMT_R32F, make(DkImageFormat_R32_Float, 1, 4)},
        {D3DFMT_G32R32F, make(DkImageFormat_RG32_Float, 1, 8)},
        {D3DFMT_A32B32G32R32F, make(DkImageFormat_RGBA32_Float, 1, 16)},
        {D3DFMT_DXT1, make(DkImageFormat_RGBA_BC1, 4, 8)},
        {D3DFMT_DXT2, make(DkImageFormat_RGBA_BC2, 4, 16)},
        {D3DFMT_DXT3, make(DkImageFormat_RGBA_BC2, 4, 16)},
        {D3DFMT_DXT4, make(DkImageFormat_RGBA_BC3, 4, 16)},
        {D3DFMT_DXT5, make(DkImageFormat_RGBA_BC3, 4, 16)},
        {fourcc('A', 'T', 'I', '1'), make(DkImageFormat_R_BC4_Unorm, 4, 8)},
        {fourcc('A', 'T', 'I', '2'), make(DkImageFormat_RG_BC5_Unorm, 4, 16)},
        {D3DFMT_D24S8, make(DkImageFormat_Z24S8, 1, 4, true)},
        {D3DFMT_D24X8, make(DkImageFormat_Z24S8, 1, 4, true)},
        {D3DFMT_D16, make(DkImageFormat_Z16, 1, 2, true)},
        {D3DFMT_D16_LOCKABLE, make(DkImageFormat_Z16, 1, 2, true)},
        {D3DFMT_D32F_LOCKABLE, make(DkImageFormat_ZF32, 1, 4, true)},
        {fourcc('I', 'N', 'T', 'Z'), make(DkImageFormat_Z24S8, 1, 4, true)},
        {fourcc('D', 'F', '2', '4'), make(DkImageFormat_Z24S8, 1, 4, true)},
        {fourcc('D', 'F', '1', '6'), make(DkImageFormat_Z16, 1, 2, true)},
    };
    auto it = table.find(uint32_t(f));
    if (it == table.end()) {
        static std::mutex mu;
        static std::set<uint32_t> seen;
        std::lock_guard lk(mu);
        if (seen.insert(uint32_t(f)).second) rt::log("deko3d: unsupported format %u (0x%08X)", uint32_t(f), uint32_t(f));
        return nullptr;
    }
    return &it->second;
}

uint32_t mip(uint32_t v, uint32_t level) { return std::max<uint32_t>(v >> level, 1); }
uint32_t row_pitch(const FormatInfo& f, uint32_t width) { return (width + f.block - 1) / f.block * f.bytes; }
uint32_t level_rows(const FormatInfo& f, uint32_t height) { return (height + f.block - 1) / f.block; }

// ---- Buffers ----

bool BufferStore::init(uint32_t bytes) {
    size = std::max<uint32_t>(bytes, 4);
    Alloc a = gpu().buffers.alloc(size, 256);
    if (!a) return false;
    versions.push_back({a, 0});
    return true;
}

void BufferStore::release() {
    std::vector<Version> v = std::move(versions);
    versions.clear();
    gpu().defer([v] {
        for (const Version& x : v) gpu().buffers.free_now(x.mem);
    });
}

uint8_t* BufferStore::lock(uint32_t offset, DWORD flags) {
    Gpu& g = gpu();
    // Frames before `done` have finished on the GPU (gpu.h: kFrames in flight).
    const uint64_t done = g.frame_count >= kFrames - 1 ? g.frame_count - (kFrames - 1) : 0;
    auto busy = [&](const Version& v) { return v.last_used != 0 && v.last_used - 1 >= done; };
    Version& cur = versions[current];
    if ((flags & D3DLOCK_NOOVERWRITE) || (!(flags & D3DLOCK_DISCARD) && !busy(cur))) return cur.mem.cpu() + offset;
    int next = -1;
    for (int i = 0; i < int(versions.size()); ++i)
        if (i != current && !busy(versions[i])) {
            next = i;
            break;
        }
    if (next < 0) {
        Alloc a = g.buffers.alloc(size, 256);
        if (!a) return nullptr;
        versions.push_back({a, 0});
        next = int(versions.size()) - 1;
        if (versions.size() == 17) rt::log("deko3d: a %u-byte buffer is renamed 16 times in flight", size);
    }
    if (!(flags & D3DLOCK_DISCARD)) std::memcpy(versions[next].mem.cpu(), versions[current].mem.cpu(), size);
    current = next;
    return versions[current].mem.cpu() + offset;
}

VertexBuffer::VertexBuffer(Device* d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
    : dev_(d), usage_(usage), fvf_(fvf), pool_(pool) {
    store.init(length);
    dev_->AddRef();
}
VertexBuffer::~VertexBuffer() {
    store.release();
    dev_->Release();
}
HRESULT VertexBuffer::GetDevice(IDirect3DDevice9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT VertexBuffer::Lock(UINT offset, UINT size, void** data, DWORD flags) {
    if (!data || offset > store.size) return D3DERR_INVALIDCALL;
    uint8_t* p = store.lock(offset, flags);
    *data = p;
    return p ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}
HRESULT VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC* d) {
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = D3DFMT_VERTEXDATA;
    d->Type = D3DRTYPE_VERTEXBUFFER;
    d->Usage = usage_;
    d->Pool = pool_;
    d->Size = store.size;
    d->FVF = fvf_;
    return D3D_OK;
}

IndexBuffer::IndexBuffer(Device* d, UINT length, DWORD usage, D3DFORMAT f, D3DPOOL pool)
    : format(f), dev_(d), usage_(usage), pool_(pool) {
    store.init(length);
    dev_->AddRef();
}
IndexBuffer::~IndexBuffer() {
    store.release();
    dev_->Release();
}
HRESULT IndexBuffer::GetDevice(IDirect3DDevice9** out) {
    if (!out) return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT IndexBuffer::Lock(UINT offset, UINT size, void** data, DWORD flags) {
    if (!data || offset > store.size) return D3DERR_INVALIDCALL;
    uint8_t* p = store.lock(offset, flags);
    *data = p;
    return p ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}
HRESULT IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC* d) {
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = format;
    d->Type = D3DRTYPE_INDEXBUFFER;
    d->Usage = usage_;
    d->Pool = pool_;
    d->Size = store.size;
    return D3D_OK;
}

// ---- Textures ----

namespace {

uint32_t image_flags(DWORD usage, const FormatInfo& f) {
    uint32_t flags = DkImageFlags_Usage2DEngine;  // blits and copies
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || f.depth)
        flags |= DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    return flags;
}

uint32_t full_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t n = 1, m = std::max({w, h, d});
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

bool init_core(TextureCore& c, Device* d, DkImageType type, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
               D3DFORMAT f, D3DPOOL pool) {
    c.dev = d;
    c.d3dfmt = f;
    c.usage = usage;
    c.pool = pool;
    c.fmt = format_info(f);
    if (!c.fmt) return false;
    if (levels == 0 || (usage & D3DUSAGE_AUTOGENMIPMAP)) levels = (usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : full_levels(w, h, depth);
    for (int i = 0; i < 4; ++i) c.img.swizzle[i] = c.fmt->swizzle[i];
    const uint32_t layers = type == DkImageType_Cubemap ? 6 : 1;
    if (!c.img.create(type, c.fmt->dk, w, h, depth, levels, layers, image_flags(usage, *c.fmt))) return false;
    c.surfaces.assign(size_t(layers) * c.img.levels, nullptr);
    return true;
}

void level_desc(const TextureCore& c, UINT level, D3DSURFACE_DESC* d) {
    d->Format = c.d3dfmt;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = c.usage;
    d->Pool = c.pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE;
    d->MultiSampleQuality = 0;
    d->Width = mip(c.img.width, level);
    d->Height = mip(c.img.height, level);
}

}  // namespace

HRESULT TextureCore::lock(uint32_t layer, uint32_t level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    if (!out || level >= img.levels) return D3DERR_INVALIDCALL;
    const uint32_t w = mip(img.width, level), h = mip(img.height, level);
    RECT r = rect ? *rect : RECT{0, 0, LONG(w), LONG(h)};
    // Block formats lock whole blocks.
    const uint32_t b = fmt->block;
    r.left = r.left / b * b;
    r.top = r.top / b * b;
    r.right = std::min<LONG>(LONG(w), (r.right + b - 1) / b * b);
    r.bottom = std::min<LONG>(LONG(h), (r.bottom + b - 1) / b * b);
    Staging s;
    s.rect = r;
    s.pitch = row_pitch(*fmt, uint32_t(r.right - r.left));
    s.mem = gpu().cur().transient.alloc(s.pitch * level_rows(*fmt, uint32_t(r.bottom - r.top)), 256);
    if (flags & D3DLOCK_READONLY) log_unimplemented("texture read-back (LockRect D3DLOCK_READONLY)");
    out->pBits = s.mem.cpu();
    out->Pitch = INT(s.pitch);
    locks[layer << 8 | level] = s;
    return D3D_OK;
}

HRESULT TextureCore::unlock(uint32_t layer, uint32_t level) {
    auto it = locks.find(layer << 8 | level);
    if (it == locks.end()) return D3DERR_INVALIDCALL;
    const Staging s = it->second;
    locks.erase(it);
    dev->upload(img, layer, level, s.mem, (s.pitch / fmt->bytes) * fmt->block, s.rect);
    return D3D_OK;
}

Surface* TextureCore::surface(IUnknown* container, uint32_t layer, uint32_t level) {
    Surface*& s = surfaces[size_t(layer) * img.levels + level];
    if (!s) s = new Surface(dev, container, this, layer, level);
    return s;
}

void TextureCore::release() {
    for (Surface* s : surfaces) delete s;
    surfaces.clear();
    img.destroy();
}

bool Texture::init(Device* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    return init_core(core, d, DkImageType_2D, w, h, 1, levels, usage, f, pool);
}
HRESULT Texture::GetDevice(IDirect3DDevice9** out) {
    core.dev->AddRef();
    *out = core.dev;
    return D3D_OK;
}
DWORD Texture::SetLOD(DWORD lod) {
    const DWORD old = core.lod;
    core.lod = std::min<DWORD>(lod, core.img.levels - 1);
    return old;
}
HRESULT Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC* d) {
    if (!d || level >= core.img.levels) return D3DERR_INVALIDCALL;
    level_desc(core, level, d);
    return D3D_OK;
}
HRESULT Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9** out) {
    if (!out || level >= core.img.levels) return D3DERR_INVALIDCALL;
    *out = core.surface(this, 0, level);
    AddRef();  // a level's reference count is its texture's
    return D3D_OK;
}
HRESULT Texture::LockRect(UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    return core.lock(0, level, out, rect, flags);
}
HRESULT Texture::UnlockRect(UINT level) { return core.unlock(0, level); }

bool CubeTexture::init(Device* d, UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    return init_core(core, d, DkImageType_Cubemap, edge, edge, 1, levels, usage, f, pool);
}
HRESULT CubeTexture::GetDevice(IDirect3DDevice9** out) {
    core.dev->AddRef();
    *out = core.dev;
    return D3D_OK;
}
DWORD CubeTexture::SetLOD(DWORD lod) {
    const DWORD old = core.lod;
    core.lod = std::min<DWORD>(lod, core.img.levels - 1);
    return old;
}
HRESULT CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* d) {
    if (!d || level >= core.img.levels) return D3DERR_INVALIDCALL;
    level_desc(core, level, d);
    return D3D_OK;
}
HRESULT CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** out) {
    if (!out || level >= core.img.levels || uint32_t(face) > 5) return D3DERR_INVALIDCALL;
    *out = core.surface(this, uint32_t(face), level);
    AddRef();
    return D3D_OK;
}
HRESULT CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    return core.lock(uint32_t(face), level, out, rect, flags);
}
HRESULT CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level) { return core.unlock(uint32_t(face), level); }

bool VolumeTexture::init(Device* d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    return init_core(core, d, DkImageType_3D, w, h, depth, levels, usage, f, pool);
}
HRESULT VolumeTexture::GetDevice(IDirect3DDevice9** out) {
    core.dev->AddRef();
    *out = core.dev;
    return D3D_OK;
}
DWORD VolumeTexture::SetLOD(DWORD lod) {
    const DWORD old = core.lod;
    core.lod = std::min<DWORD>(lod, core.img.levels - 1);
    return old;
}
HRESULT VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags) {
    if (!out || level >= core.img.levels || box) return D3DERR_INVALIDCALL;  // whole levels only
    const uint32_t w = mip(core.img.width, level), h = mip(core.img.height, level), d = mip(core.img.depth, level);
    const uint32_t pitch = row_pitch(*core.fmt, w), slice = pitch * level_rows(*core.fmt, h);
    staging = gpu().cur().transient.alloc(slice * d, 256);
    staging_level = level;
    out->pBits = staging.cpu();
    out->RowPitch = INT(pitch);
    out->SlicePitch = INT(slice);
    return D3D_OK;
}
HRESULT VolumeTexture::UnlockBox(UINT level) {
    if (!staging || level != staging_level) return D3DERR_INVALIDCALL;
    const uint32_t w = mip(core.img.width, level), h = mip(core.img.height, level), d = mip(core.img.depth, level);
    core.dev->upload(core.img, 0, level, staging, w, RECT{0, 0, LONG(w), LONG(h)}, 0, d);
    staging = {};
    return D3D_OK;
}

// ---- Surfaces ----

Surface::Surface(Device* d, IUnknown* container, TextureCore* t, uint32_t ly, uint32_t lv)
    : width(mip(t->img.width, lv)), height(mip(t->img.height, lv)), d3dfmt(t->d3dfmt), fmt(t->fmt), usage(t->usage),
      pool(t->pool), tex(t), layer(ly), level(lv), dev_(d), container_(container) {}

Surface::Surface(Device* d, UINT w, UINT h, D3DFORMAT f, DWORD u, D3DPOOL p, bool cpu_only)
    : width(w), height(h), d3dfmt(f), fmt(format_info(f)), usage(u), pool(p), dev_(d) {
    if (!fmt) return;
    if (cpu_only) {
        cpu_pitch = row_pitch(*fmt, w);
        cpu = gpu().readback.alloc(cpu_pitch * level_rows(*fmt, h), 256);
    } else {
        for (int i = 0; i < 4; ++i) own.swizzle[i] = fmt->swizzle[i];
        own.create(DkImageType_2D, fmt->dk, w, h, 1, 1, 1, image_flags(u, *fmt));
    }
    dev_->AddRef();
}

Surface::~Surface() {
    if (tex) return;  // a texture level: the texture owns everything
    own.destroy();
    if (cpu) {
        const Alloc a = cpu;
        gpu().defer([a] { gpu().readback.free_now(a); });
    }
    if (fmt) dev_->Release();
}

bool Surface::ok() const { return tex || own.mem || cpu; }

HRESULT Surface::QueryInterface(REFIID riid, void** out) { return StubIDirect3DSurface9::QueryInterface(riid, out); }
ULONG Surface::AddRef() { return container_ ? container_->AddRef() : StubIDirect3DSurface9::AddRef(); }
ULONG Surface::Release() { return container_ ? container_->Release() : StubIDirect3DSurface9::Release(); }

HRESULT Surface::GetDevice(IDirect3DDevice9** out) {
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT Surface::GetContainer(REFIID riid, void** out) {
    if (!out) return D3DERR_INVALIDCALL;
    if (container_) return container_->QueryInterface(riid, out);
    return dev_->QueryInterface(riid, out);
}
HRESULT Surface::GetDesc(D3DSURFACE_DESC* d) {
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = d3dfmt;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = usage;
    d->Pool = pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE;
    d->MultiSampleQuality = 0;
    d->Width = width;
    d->Height = height;
    return D3D_OK;
}
HRESULT Surface::LockRect(D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    if (!out) return D3DERR_INVALIDCALL;
    if (tex) {
        D3DLOCKED_RECT r;
        const HRESULT hr = tex->lock(layer, level, &r, rect, flags);
        *out = r;
        return hr;
    }
    if (cpu) {
        dkMemBlockFlushCpuCache(cpu.block, cpu.offset, cpu.size);  // GPU-written (GetRenderTargetData)
        const uint32_t x = rect ? uint32_t(rect->left) : 0, y = rect ? uint32_t(rect->top) : 0;
        out->pBits = cpu.cpu() + size_t(y / fmt->block) * cpu_pitch + x / fmt->block * fmt->bytes;
        out->Pitch = INT(cpu_pitch);
        return D3D_OK;
    }
    log_unimplemented("IDirect3DSurface9::LockRect on a render target");
    return D3DERR_INVALIDCALL;
}
HRESULT Surface::UnlockRect() {
    if (tex) return tex->unlock(layer, level);
    return D3D_OK;
}

GpuImage* Surface::image() {
    if (tex) return &tex->img;
    return own.mem ? &own : nullptr;
}

DkImageView Surface::view() {
    GpuImage* g = image();
    return g->view(tex ? level : 0, tex ? layer : 0, false);
}

}  // namespace deko
