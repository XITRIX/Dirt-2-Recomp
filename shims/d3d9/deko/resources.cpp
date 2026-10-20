// D3D9 resources on deko3d: formats, vertex/index buffers, textures, surfaces.
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>

#include "device.h"
#include "recomp/host.h"

namespace deko {

void log_unimplemented(const char* method) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard lk(mu);
    if (seen.insert(method).second) rt::log("deko3d: %s is not implemented", method);
}

// Binding uses implementation methods directly. Native D3D9 does not dispatch
// through the application's public texture vtable when retaining a binding.
namespace {
struct TextureEntry {
    TextureCore* core;
    void (*add_ref)(IDirect3DBaseTexture9*);
    void (*release)(IDirect3DBaseTexture9*);
    void (*add_private)(IDirect3DBaseTexture9*);
    void (*release_private)(IDirect3DBaseTexture9*);
};
std::unordered_map<IDirect3DBaseTexture9*, TextureEntry>& texture_registry() {
    static std::unordered_map<IDirect3DBaseTexture9*, TextureEntry> entries;
    return entries;
}
template<class T> void register_texture(T* texture) {
    texture_registry().emplace(texture, TextureEntry{&texture->core,
        [](IDirect3DBaseTexture9* base) { static_cast<T*>(base)->T::AddRef(); },
        [](IDirect3DBaseTexture9* base) { static_cast<T*>(base)->T::Release(); },
        [](IDirect3DBaseTexture9* base) { static_cast<T*>(base)->T::AddRefPrivate(); },
        [](IDirect3DBaseTexture9* base) { static_cast<T*>(base)->T::ReleasePrivate(); }});
}
void unregister_texture(IDirect3DBaseTexture9* texture) {
    texture_registry().erase(texture);
}
} // namespace
TextureCore* texture_core(IDirect3DBaseTexture9* texture) {
    ApiLock api_lock(__func__);
    auto& entries = texture_registry();
    const auto it = entries.find(texture);
    return it == entries.end() ? nullptr : it->second.core;
}
void texture_add_ref(IDirect3DBaseTexture9* texture) {
    ApiLock api_lock(__func__);
    const auto it = texture_registry().find(texture);
    if (it != texture_registry().end()) it->second.add_ref(texture);
}
void texture_release(IDirect3DBaseTexture9* texture) {
    ApiLock api_lock(__func__);
    const auto it = texture_registry().find(texture);
    if (it != texture_registry().end()) {
        const auto release = it->second.release;
        release(texture); // May erase its own entry when the texture dies.
    }
}

void texture_add_private(IDirect3DBaseTexture9* texture) {
    ApiLock api_lock(__func__);
    const auto it=texture_registry().find(texture);
    if (it!=texture_registry().end()) it->second.add_private(texture);
}
void texture_release_private(IDirect3DBaseTexture9* texture) {
    ApiLock api_lock(__func__);
    const auto it=texture_registry().find(texture);
    if (it!=texture_registry().end()) {
        const auto release=it->second.release_private;
        release(texture);
    }
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
    i.block_height = f == DkImageFormat_None ? 1 : block;
    i.bytes = bytes;
    i.depth = depth;
    for (int k = 0; k < 4; ++k) i.swizzle[k] = sw[k];
    return i;
}

}  // namespace

// Native RGBX render formats force destination alpha to one during blending.
// Using RGBA storage with only a sampling swizzle leaves destination alpha wrong.
const FormatInfo* format_info(D3DFORMAT f) {
    using S = DkImageSwizzle;
    static const std::unordered_map<uint32_t, FormatInfo> table = {
        {D3DFMT_A8R8G8B8, make(DkImageFormat_BGRA8_Unorm, 1, 4)},
        {D3DFMT_X8R8G8B8, make(DkImageFormat_BGRX8_Unorm, 1, 4, false, {S::DkImageSwizzle_Red, S::DkImageSwizzle_Green, S::DkImageSwizzle_Blue, S::DkImageSwizzle_One})},
        {D3DFMT_A8B8G8R8, make(DkImageFormat_RGBA8_Unorm, 1, 4)},
        {D3DFMT_X8B8G8R8, make(DkImageFormat_RGBX8_Unorm, 1, 4, false, {S::DkImageSwizzle_Red, S::DkImageSwizzle_Green, S::DkImageSwizzle_Blue, S::DkImageSwizzle_One})},
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
        {D3DFMT_V16U16, make(DkImageFormat_RG16_Snorm,1,4,false,{S::DkImageSwizzle_Red,S::DkImageSwizzle_Green,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_G16R16, make(DkImageFormat_RG16_Unorm, 1, 4, false, {S::DkImageSwizzle_Red,S::DkImageSwizzle_Green,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_A16B16G16R16, make(DkImageFormat_RGBA16_Unorm, 1, 8)},
        {D3DFMT_R16F, make(DkImageFormat_R16_Float, 1, 2, false, {S::DkImageSwizzle_Red,S::DkImageSwizzle_One,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_G16R16F, make(DkImageFormat_RG16_Float, 1, 4, false, {S::DkImageSwizzle_Red,S::DkImageSwizzle_Green,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_A16B16G16R16F, make(DkImageFormat_RGBA16_Float, 1, 8)},
        {D3DFMT_R32F, make(DkImageFormat_R32_Float, 1, 4, false, {S::DkImageSwizzle_Red,S::DkImageSwizzle_One,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_G32R32F, make(DkImageFormat_RG32_Float, 1, 8, false, {S::DkImageSwizzle_Red,S::DkImageSwizzle_Green,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_A32B32G32R32F, make(DkImageFormat_RGBA32_Float, 1, 16)},
        {D3DFMT_DXT1, make(DkImageFormat_RGBA_BC1, 4, 8)},
        {D3DFMT_DXT2, make(DkImageFormat_RGBA_BC2, 4, 16)},
        {D3DFMT_DXT3, make(DkImageFormat_RGBA_BC2, 4, 16)},
        {D3DFMT_DXT4, make(DkImageFormat_RGBA_BC3, 4, 16)},
        {D3DFMT_DXT5, make(DkImageFormat_RGBA_BC3, 4, 16)},
        {fourcc('A', 'T', 'I', '1'), make(DkImageFormat_R_BC4_Unorm, 4, 8)},
        {fourcc('A', 'T', 'I', '2'), make(DkImageFormat_RG_BC5_Unorm, 4, 16, false, {S::DkImageSwizzle_Green,S::DkImageSwizzle_Red,S::DkImageSwizzle_One,S::DkImageSwizzle_One})},
        {D3DFMT_D24S8, make(DkImageFormat_Z24S8, 1, 4, true)},
        {D3DFMT_D24X8, make(DkImageFormat_Z24S8, 1, 4, true)},
        {D3DFMT_D16, make(DkImageFormat_Z16, 1, 2, true)},
        {D3DFMT_D16_LOCKABLE, make(DkImageFormat_Z16, 1, 2, true)},
        {D3DFMT_D32F_LOCKABLE, make(DkImageFormat_ZF32, 1, 4, true)},
        {fourcc('I', 'N', 'T', 'Z'), make(DkImageFormat_Z24S8, 1, 4, true, {S::DkImageSwizzle_Red,S::DkImageSwizzle_Red,S::DkImageSwizzle_Red,S::DkImageSwizzle_Red})},
        {fourcc('D', 'F', '2', '4'), make(DkImageFormat_Z24S8, 1, 4, true, {S::DkImageSwizzle_Red,S::DkImageSwizzle_Zero,S::DkImageSwizzle_Zero,S::DkImageSwizzle_One})},
        {fourcc('D', 'F', '1', '6'), make(DkImageFormat_Z16, 1, 2, true, {S::DkImageSwizzle_Red,S::DkImageSwizzle_Zero,S::DkImageSwizzle_Zero,S::DkImageSwizzle_One})},
        // Packed YUV supports CPU-only SCRATCH storage. GPU support is not advertised.
        {D3DFMT_YUY2, make(DkImageFormat_None, 2, 4)},
        {D3DFMT_UYVY, make(DkImageFormat_None, 2, 4)},
        // Render target without colour storage, for depth-only passes: no image.
        {fourcc('N', 'U', 'L', 'L'), make(DkImageFormat_None, 1, 0)},
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
uint32_t row_pitch(const FormatInfo& f, uint32_t width) { return (((width + f.block - 1) / f.block * f.bytes)+3u)&~3u; }
uint32_t level_rows(const FormatInfo& f, uint32_t height) { return (height + f.block_height - 1) / f.block_height; }

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
    // NOOVERWRITE takes precedence when both hints are specified.
    if (flags & D3DLOCK_NOOVERWRITE) flags &= ~D3DLOCK_DISCARD;
    // Frames before `done` have finished on the GPU (gpu.h: kFrames in flight).
    const uint64_t done = g.frame_count >= kFrames - 1 ? g.frame_count - (kFrames - 1) : 0;
    auto busy = [&](const Version& v) { return v.last_used != 0 && v.last_used - 1 >= done; };
    Version& cur = versions[current];
    if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) && busy(cur)) {
        // A normal lock preserves the resource's native allocation and pointer.
        // Waiting also preserves GPU writes; copying CPU bytes into a rename
        // cannot do that for a GPU-writable resource.
        g.flush(true,"buffer lock waits for outstanding draws");
        cur.last_used=0;
    }
    if (!(flags & D3DLOCK_DISCARD)) return cur.mem.cpu() + offset;
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
    SetDeviceParent(d);
}
VertexBuffer::~VertexBuffer() {
    store.release();
}
HRESULT VertexBuffer::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    if (!out) return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT VertexBuffer::Lock(UINT offset, UINT size, void** data, DWORD flags) {
    ApiLock api_lock(__func__);
    if (!data || offset > store.size) return D3DERR_INVALIDCALL;
    // Runtime-managed and software-vertex buffers retain their CPU storage.
    if (pool_ != D3DPOOL_DEFAULT || !(usage_ & D3DUSAGE_DYNAMIC) || dev_->software_vertex_processing())
        flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    uint8_t* p = store.lock(offset, flags);
    *data = p;
    return p ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}
HRESULT VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC* d) {
    ApiLock api_lock(__func__);
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
    SetDeviceParent(d);
}
IndexBuffer::~IndexBuffer() {
    store.release();
}
HRESULT IndexBuffer::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    if (!out) return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT IndexBuffer::Lock(UINT offset, UINT size, void** data, DWORD flags) {
    ApiLock api_lock(__func__);
    if (!data || offset > store.size) return D3DERR_INVALIDCALL;
    // Runtime-managed and software-vertex buffers retain their CPU storage.
    if (pool_ != D3DPOOL_DEFAULT || !(usage_ & D3DUSAGE_DYNAMIC) || dev_->software_vertex_processing())
        flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    uint8_t* p = store.lock(offset, flags);
    *data = p;
    return p ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}
HRESULT IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC* d) {
    ApiLock api_lock(__func__);
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

// The sampling swizzle: the format's, except that a render target holds what
// the GPU wrote in the image's own channel order, so a swizzle that corrects
// D3D's layout of uploaded data (A2R10G10B10's red/blue) must not apply.
void set_swizzle(DkImageSwizzle out[4], const FormatInfo& f, DWORD usage) {
    for (int i = 0; i < 4; ++i) out[i] = f.swizzle[i];
    if ((usage & D3DUSAGE_RENDERTARGET) && out[0] == DkImageSwizzle_Blue && out[2] == DkImageSwizzle_Red) {
        out[0] = DkImageSwizzle_Red;
        out[2] = DkImageSwizzle_Blue;
    }
}

uint32_t image_flags(DWORD usage, const FormatInfo& f) {
    uint32_t flags = DkImageFlags_Usage2DEngine;  // blits and copies
    // No DkImageFlags_HwCompression: a compressed surface must be decompressed
    // before it is sampled (deko3d does so only for swap chain images), and
    // render targets sampled compressed came out in dark 4x4 blocks.
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || f.depth) flags |= DkImageFlags_UsageRender;
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
    static const bool log_resources = std::getenv("DIRT2_DEKO_LOG_RESOURCES") != nullptr;
    if (log_resources) {
        // Each kind of texture once (type, format, usage, pool, size).
        static std::mutex mu;
        static std::set<std::tuple<int, uint32_t, DWORD, int, UINT, UINT, UINT>> seen;
        std::lock_guard lk(mu);
        if (seen.insert({int(type), uint32_t(f), usage, int(pool), w, h, depth}).second && seen.size() <= 200)
            rt::log("deko3d: texture %s %ux%ux%u, %u levels, format %u (0x%X), usage %lX, pool %d",
                    type == DkImageType_3D ? "3D" : type == DkImageType_Cubemap ? "cube" : "2D", w, h, depth, levels,
                    uint32_t(f), uint32_t(f), (unsigned long)usage, int(pool));
    }
    if (levels == 0 || (usage & D3DUSAGE_AUTOGENMIPMAP)) levels = full_levels(w, h, depth);
    set_swizzle(c.img.swizzle, *c.fmt, usage);
    const uint32_t layers = type == DkImageType_Cubemap ? 6 : 1;
    if (c.fmt->dk == DkImageFormat_None) {  // NULL format: sizes only, nothing to draw into or sample
        c.img.type = type;
        c.img.width = std::max<UINT>(w, 1);
        c.img.height = std::max<UINT>(h, 1);
        c.img.depth = std::max<UINT>(depth, 1);
        c.img.levels = std::max<UINT>(levels, 1);
        c.img.layers = layers;
        c.surfaces.assign(size_t(layers) * c.img.levels, nullptr);
        return true;
    }
    // Native BC2 sampling tears the UI's smooth alpha edges into repeated
    // rectangular gaps on Switch. RGBA8 storage fixes the captured alert;
    // preserve the compressed D3D API layout and expand at upload instead.
    // Keep an opt-out for future driver/compiler comparisons.
    const char* expand_bc2 = std::getenv("DIRT2_DEKO_EXPAND_BC2");
    c.img.expanded_bc = c.fmt->dk == DkImageFormat_RGBA_BC2 &&
                       !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
                       (!expand_bc2 || std::strcmp(expand_bc2, "0") != 0) ? 2 : 0;
    // The remaining clipped HUD symbols are single-mip BC3 images with intact
    // source alpha. Use the same RGBA upload path for this UI texture class.
    // Leave mipmapped scene textures compressed to keep their memory footprint.
    const char* expand_bc3 = std::getenv("DIRT2_DEKO_EXPAND_BC3_UI");
    if (c.fmt->dk == DkImageFormat_RGBA_BC3 && levels == 1 && type == DkImageType_2D &&
        !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
        (!expand_bc3 || std::strcmp(expand_bc3, "0") != 0)) c.img.expanded_bc = 3;
    if (!c.img.create(type, c.img.expanded_bc ? DkImageFormat_RGBA8_Unorm : c.fmt->dk,
                      w, h, depth, levels, layers, image_flags(usage, *c.fmt))) return false;
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || c.fmt->depth) d->clear_new_image(c.img, c.fmt->depth);
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

HRESULT TextureCore::set_autogen_filter(D3DTEXTUREFILTERTYPE filter) {
    if(filter!=D3DTEXF_POINT && filter!=D3DTEXF_LINEAR)return D3DERR_INVALIDCALL;
    autogen_filter=filter;autogen_dirty=true;return D3D_OK;
}
void TextureCore::generate_mips() {
    if(!(usage&D3DUSAGE_AUTOGENMIPMAP) || !autogen_dirty || !img.mem || img.levels<2)return;
    // Mip levels share an allocation but use disjoint image subresources.
    // Finish each source level before the next level samples it for filtering.
    gpu().state_owner=nullptr;
    for(uint32_t layer=0;layer<img.layers;++layer)for(uint32_t level=1;level<img.levels;++level){
        const auto src=img.view(level-1,layer),dst=img.view(level,layer);
        const DkImageRect sr{0,0,0,mip(img.width,level-1),mip(img.height,level-1),mip(img.depth,level-1)};
        const DkImageRect dr{0,0,0,mip(img.width,level),mip(img.height,level),mip(img.depth,level)};
        dkCmdBufBarrier(gpu().cmd,DkBarrier_Full,DkInvalidateFlags_Image);
        dkCmdBufBlitImage(gpu().cmd,&src,&sr,&dst,&dr,autogen_filter==D3DTEXF_LINEAR?DkBlitFlag_FilterLinear:0,0);
    }
    dkCmdBufBarrier(gpu().cmd,DkBarrier_Full,DkInvalidateFlags_Image);
    autogen_dirty=false;
}

void TextureCore::ensure_cpu_storage() {
    if (!cpu_layout.empty()) return;
    const bool ati = d3dfmt == fourcc('A','T','I','1') || d3dfmt == fourcc('A','T','I','2');
    size_t total = 0;
    for (uint32_t layer=0;layer<img.layers;++layer) for (uint32_t level=0;level<img.levels;++level) {
        const uint32_t w=mip(img.width,level),h=mip(img.height,level),d=mip(img.depth,level);
        CpuLevel layout;
        layout.offset=total;
        layout.pitch=row_pitch(*fmt,w);
        layout.slice=layout.pitch*level_rows(*fmt,h);
        // ATI's exposed native pitch is intentionally unrelated to BC storage.
        layout.api_pitch=ati?(w+3u)&~3u:layout.pitch;
        layout.api_slice=ati?layout.api_pitch*h:layout.slice;
        layout.bytes=size_t(std::max(layout.slice,layout.api_slice))*d;
        total+=layout.bytes;
        cpu_layout.push_back(layout);
    }
    cpu_storage.resize(total);
}

HRESULT TextureCore::upload_cpu_level(uint32_t layer,uint32_t level) {
    autogen_dirty=true;
    const auto& layout=cpu_layout[size_t(layer)*img.levels+level];
    Alloc snapshot=gpu().cur().transient.alloc(layout.bytes,256);
    if (!snapshot) return D3DERR_OUTOFVIDEOMEMORY;
    std::memcpy(snapshot.cpu(),cpu_storage.data()+layout.offset,layout.bytes);
    const uint32_t w=mip(img.width,level),h=mip(img.height,level),d=mip(img.depth,level);
    dev->upload(img,layer,level,snapshot,layout.pitch,RECT{0,0,LONG(w),LONG(h)},0,d,layout.slice);
    return D3D_OK;
}

HRESULT TextureCore::lock(uint32_t layer, uint32_t level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    if (!out || layer >= img.layers || level >= api_levels()) return D3DERR_INVALIDCALL;
    const uint32_t key = layer << 8 | level;
    if (locks.count(key)) return D3DERR_INVALIDCALL;
    const uint32_t w = mip(img.width, level), h = mip(img.height, level);
    const RECT r = rect ? *rect : RECT{0, 0, LONG(w), LONG(h)};
    // D3D9 validates DEFAULT locks, but CPU-pool surfaces expose a pointer
    // even for inverted/out-of-bounds rectangles. Unlock still uploads only
    // the actual level, so these legacy API offsets never reach the GPU.
    if (pool == D3DPOOL_DEFAULT && (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
        r.right > LONG(w) || r.bottom > LONG(h))) return D3DERR_INVALIDCALL;
    const uint32_t b = fmt->block;
    if (rect && !level && pool == D3DPOOL_DEFAULT && b > 1 &&
        (r.left % b || r.top % b || r.right % b || r.bottom % b)) return D3DERR_INVALIDCALL;
    if (pool == D3DPOOL_DEFAULT && (flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) ==
        (D3DLOCK_DISCARD | D3DLOCK_READONLY)) return D3DERR_INVALIDCALL;
    ensure_cpu_storage();
    const auto& layout=cpu_layout[size_t(layer)*img.levels+level];
    const uint32_t pitch=layout.pitch;
    const size_t bytes=size_t(layout.slice)*mip(img.depth,level);
    Staging staging;
    staging.pitch = pitch;
    staging.rect = RECT{0, 0, LONG(w), LONG(h)};
    staging.readonly = (flags & D3DLOCK_READONLY) != 0;
    // Render targets and other GPU-writable DEFAULT images need current GPU
    // contents. Expanded BC images retain their original compressed CPU data.
    const bool readback = img.mem && pool == D3DPOOL_DEFAULT && !img.expanded_bc &&
        !(flags & D3DLOCK_DISCARD);
    uint8_t* bytes_cpu=cpu_storage.data()+layout.offset;
    if (readback) {
        Alloc memory = gpu().readback.alloc(bytes, 256);
        if (!memory) return D3DERR_OUTOFVIDEOMEMORY;
        dev->read_image(img, layer, level, memory, pitch, staging.rect);
        std::memcpy(bytes_cpu, memory.cpu(), bytes);
        gpu().readback.free_now(memory); // read_image submitted and waited.
    }
    const bool ati=d3dfmt==fourcc('A','T','I','1') || d3dfmt==fourcc('A','T','I','2');
    const intptr_t offset=ati?intptr_t(r.top)*layout.api_pitch+r.left
        :intptr_t(r.top/LONG(fmt->block_height))*layout.api_pitch+intptr_t(r.left/LONG(b))*fmt->bytes;
    out->pBits=reinterpret_cast<void*>(uintptr_t(bytes_cpu)+uintptr_t(offset));
    out->Pitch=INT(layout.api_pitch);
    locks.emplace(key, staging);
    return D3D_OK;
}

HRESULT TextureCore::unlock(uint32_t layer, uint32_t level) {
    const uint32_t key = layer << 8 | level;
    auto it = locks.find(key);
    if (it == locks.end()) return D3DERR_INVALIDCALL;
    const Staging staging = it->second;
    if (!staging.readonly && img.mem) {
        // Managed data may include writes to the contiguous mip chain through
        // one lock. Preserve them and upload every affected level of this face.
        const uint32_t begin=pool==D3DPOOL_MANAGED?0:level;
        const uint32_t end=pool==D3DPOOL_MANAGED?img.levels:level+1;
        for(uint32_t mip_level=begin;mip_level<end;++mip_level) {
            const HRESULT hr=upload_cpu_level(layer,mip_level);
            if(FAILED(hr))return hr;
        }
    }
    locks.erase(it);
    return D3D_OK;
}

HRESULT TextureCore::lock_box(uint32_t level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags) {
    if (!out) return D3DERR_INVALIDCALL;
    out->pBits = nullptr;
    if (level >= api_levels() || box_readonly.count(level) ||
        (pool == D3DPOOL_DEFAULT && !(usage & D3DUSAGE_DYNAMIC))) return D3DERR_INVALIDCALL;
    const uint32_t w = mip(img.width, level), h = mip(img.height, level), d = mip(img.depth, level);
    const D3DBOX r = box ? *box : D3DBOX{0, 0, w, h, 0, d};
    if (r.Left >= r.Right || r.Top >= r.Bottom || r.Front >= r.Back ||
        r.Right > w || r.Bottom > h || r.Back > d) return D3DERR_INVALIDCALL;
    const bool ati = d3dfmt == fourcc('A','T','I','1') || d3dfmt == fourcc('A','T','I','2');
    const uint32_t b = fmt->block;
    if (box && !level && b > 1 && (r.Left % b || r.Top % fmt->block_height || r.Right % b || r.Bottom % fmt->block_height))
        return D3DERR_INVALIDCALL;
    ensure_cpu_storage();
    const auto& layout=cpu_layout[level];
    const uint32_t pitch=layout.api_pitch,slice=layout.api_slice;
    uint8_t* bytes=cpu_storage.data()+layout.offset;
    if (img.mem && pool == D3DPOOL_DEFAULT && !(flags & D3DLOCK_DISCARD)) {
        const size_t storage_bytes=size_t(layout.slice)*d;
        Alloc memory = gpu().readback.alloc(storage_bytes, 256);
        if (!memory) return D3DERR_OUTOFVIDEOMEMORY;
        dev->read_image(img, 0, level, memory, layout.pitch, RECT{0,0,LONG(w),LONG(h)}, d);
        std::memcpy(bytes, memory.cpu(), storage_bytes);
        gpu().readback.free_now(memory);
    }
    out->pBits = bytes + size_t(r.Front) * slice +
        (ati ? size_t(r.Top) * pitch + r.Left : size_t(r.Top / fmt->block_height) * pitch + size_t(r.Left / b) * fmt->bytes);
    out->RowPitch = INT(pitch);
    out->SlicePitch = INT(slice);
    box_readonly.emplace(level, (flags & D3DLOCK_READONLY) != 0);
    return D3D_OK;
}

HRESULT TextureCore::unlock_box(uint32_t level) {
    auto it = box_readonly.find(level);
    if (it == box_readonly.end()) return D3DERR_INVALIDCALL;
    if (!it->second && img.mem) {
        const uint32_t begin=pool==D3DPOOL_MANAGED?0:level;
        const uint32_t end=pool==D3DPOOL_MANAGED?img.levels:level+1;
        for(uint32_t mip_level=begin;mip_level<end;++mip_level) {
            const HRESULT hr=upload_cpu_level(0,mip_level);
            if(FAILED(hr))return hr;
        }
    }
    box_readonly.erase(it);
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

Texture::~Texture() {
    ApiLock api_lock(__func__);
    unregister_texture(this);
    core.release();
}
bool Texture::init(Device* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    ApiLock api_lock(__func__);
    const bool initialized = init_core(core, d, DkImageType_2D, w, h, 1, levels, usage, f, pool);
    if (initialized) { SetDeviceParent(d);register_texture(this); }
    return initialized;
}
HRESULT Texture::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    core.dev->AddRef();
    *out = core.dev;
    return D3D_OK;
}
DWORD Texture::SetLOD(DWORD lod) {
    ApiLock api_lock(__func__);
    const DWORD old = core.lod;
    core.lod = std::min<DWORD>(lod, core.img.levels - 1);
    if (core.lod != old) core.dev->texture_lod_changed(&core);
    return old;
}
HRESULT Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC* d) {
    ApiLock api_lock(__func__);
    if (!d || level >= core.api_levels()) return D3DERR_INVALIDCALL;
    level_desc(core, level, d);
    return D3D_OK;
}
HRESULT Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9** out) {
    ApiLock api_lock(__func__);
    if (!out || level >= core.api_levels()) return D3DERR_INVALIDCALL;
    *out = core.surface(this, 0, level);
    AddRef();  // a level's reference count is its texture's
    return D3D_OK;
}
HRESULT Texture::LockRect(UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    ApiLock api_lock(__func__);
    return core.lock(0, level, out, rect, flags);
}
HRESULT Texture::UnlockRect(UINT level) {
    ApiLock api_lock(__func__); return core.unlock(0, level); }

CubeTexture::~CubeTexture() {
    ApiLock api_lock(__func__);
    unregister_texture(this);
    core.release();
}
bool CubeTexture::init(Device* d, UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    ApiLock api_lock(__func__);
    const bool initialized = init_core(core, d, DkImageType_Cubemap, edge, edge, 1, levels, usage, f, pool);
    if (initialized) { SetDeviceParent(d);register_texture(this); }
    return initialized;
}
HRESULT CubeTexture::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    core.dev->AddRef();
    *out = core.dev;
    return D3D_OK;
}
DWORD CubeTexture::SetLOD(DWORD lod) {
    ApiLock api_lock(__func__);
    const DWORD old = core.lod;
    core.lod = std::min<DWORD>(lod, core.img.levels - 1);
    if (core.lod != old) core.dev->texture_lod_changed(&core);
    return old;
}
HRESULT CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* d) {
    ApiLock api_lock(__func__);
    if (!d || level >= core.api_levels()) return D3DERR_INVALIDCALL;
    level_desc(core, level, d);
    return D3D_OK;
}
HRESULT CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** out) {
    ApiLock api_lock(__func__);
    if (!out || level >= core.api_levels() || uint32_t(face) > 5) return D3DERR_INVALIDCALL;
    *out = core.surface(this, uint32_t(face), level);
    AddRef();
    return D3D_OK;
}
HRESULT CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    ApiLock api_lock(__func__);
    return core.lock(uint32_t(face), level, out, rect, flags);
}
HRESULT CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level) {
    ApiLock api_lock(__func__); return core.unlock(uint32_t(face), level); }

VolumeTexture::~VolumeTexture() {
    ApiLock api_lock(__func__);
    for(auto* volume:volumes)delete volume;
    unregister_texture(this);
    core.release();
}
bool VolumeTexture::init(Device* d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    ApiLock api_lock(__func__);
    const bool initialized = init_core(core, d, DkImageType_3D, w, h, depth, levels, usage, f, pool);
    if (initialized) { SetDeviceParent(d);register_texture(this); }
    return initialized;
}
HRESULT VolumeTexture::GetVolumeLevel(UINT level,IDirect3DVolume9** out) {
    ApiLock api_lock(__func__);
    if(!out || level>=core.img.levels)return D3DERR_INVALIDCALL;
    if(volumes.empty())volumes.resize(core.img.levels,nullptr);
    if(!volumes[level])volumes[level]=new Volume(this,level);
    *out=volumes[level];TextureParentAddRef();return D3D_OK;
}
HRESULT VolumeTexture::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    core.dev->AddRef();
    *out = core.dev;
    return D3D_OK;
}
DWORD VolumeTexture::SetLOD(DWORD lod) {
    ApiLock api_lock(__func__);
    const DWORD old = core.lod;
    core.lod = std::min<DWORD>(lod, core.img.levels - 1);
    if (core.lod != old) core.dev->texture_lod_changed(&core);
    return old;
}
HRESULT VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC* d) {
    ApiLock api_lock(__func__);
    if (!d || level >= core.api_levels()) return D3DERR_INVALIDCALL;
    d->Format = core.d3dfmt;
    d->Type = D3DRTYPE_VOLUME;
    d->Usage = core.usage;
    d->Pool = core.pool;
    d->Width = mip(core.img.width, level);
    d->Height = mip(core.img.height, level);
    d->Depth = mip(core.img.depth, level);
    return D3D_OK;
}
HRESULT VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags) {
    ApiLock api_lock(__func__);
    return core.lock_box(level, out, box, flags);
}
HRESULT VolumeTexture::UnlockBox(UINT level) {
    ApiLock api_lock(__func__);
    return core.unlock_box(level);
}

DkMsMode multisample_mode(D3DMULTISAMPLE_TYPE type,DWORD quality) {
    if(type==D3DMULTISAMPLE_NONMASKABLE)return quality==0?DkMsMode_2x:quality==1?DkMsMode_4x:DkMsMode_8x;
    return type==D3DMULTISAMPLE_2_SAMPLES?DkMsMode_2x:type==D3DMULTISAMPLE_4_SAMPLES?DkMsMode_4x:type==D3DMULTISAMPLE_8_SAMPLES?DkMsMode_8x:DkMsMode_1x;
}
bool multisample_valid(D3DMULTISAMPLE_TYPE type,DWORD quality) {
    return type==D3DMULTISAMPLE_NONMASKABLE?quality<3:quality==0&&(type==D3DMULTISAMPLE_NONE||type==D3DMULTISAMPLE_2_SAMPLES||type==D3DMULTISAMPLE_4_SAMPLES||type==D3DMULTISAMPLE_8_SAMPLES);
}

// ---- Surfaces ----

Surface::Surface(Device* d, IUnknown* container, TextureCore* t, uint32_t ly, uint32_t lv)
    : width(mip(t->img.width, lv)), height(mip(t->img.height, lv)), d3dfmt(t->d3dfmt), fmt(t->fmt), usage(t->usage),
      pool(t->pool), tex(t), layer(ly), level(lv), dev_(d), container_(container) {}

Surface::Surface(Device* d, UINT w, UINT h, D3DFORMAT f, DWORD u, D3DPOOL p, bool cpu_only, D3DMULTISAMPLE_TYPE ms, DWORD quality)
    : width(w), height(h), d3dfmt(f), fmt(format_info(f)), usage(u), multisample(ms), multisample_quality(quality), pool(p), dev_(d) {
    if (!fmt) return;
    if (cpu_only && fmt->bytes) {
        const bool ati = f == fourcc('A','T','I','1') || f == fourcc('A','T','I','2');
        cpu_pitch = ati ? (w + 3u) & ~3u : row_pitch(*fmt, w);
        cpu = gpu().readback.alloc(cpu_pitch * ((f == fourcc('A','T','I','1') || f == fourcc('A','T','I','2')) ? h : level_rows(*fmt, h)), 256);
    } else if(fmt->dk != DkImageFormat_None) {
        set_swizzle(own.swizzle, *fmt, u);
        // Plain DEFAULT surfaces expose Usage=0, but initialization/ColorFill
        // clears use the render engine internally. Keep API usage unchanged.
        const uint32_t flags=image_flags(u,*fmt)|(fmt->block==1?DkImageFlags_UsageRender:0);
        if (own.create(DkImageType_2D, fmt->dk, w, h, 1, 1, 1, flags, multisample_mode(ms,quality)) &&
            (flags&DkImageFlags_UsageRender)) d->clear_new_image(own, fmt->depth);
    }
    SetDeviceParent(d);
}

Surface::~Surface() {
    if (tex) return;  // a texture level: the texture owns everything
    own.destroy();
    if (cpu) {
        const Alloc a = cpu;
        gpu().defer([a] { gpu().readback.free_now(a); });
    }
}

bool Surface::ok() const {
    ApiLock api_lock(__func__); return tex || own.mem || cpu || (fmt && fmt->dk == DkImageFormat_None && !fmt->bytes); }

HRESULT Surface::QueryInterface(REFIID riid, void** out) {
    ApiLock api_lock(__func__); return StubIDirect3DSurface9::QueryInterface(riid, out); }
ULONG Surface::AddRef() {
    ApiLock api_lock(__func__); return container_ ? container_->AddRef() : StubIDirect3DSurface9::AddRef(); }
ULONG Surface::Release() {
    ApiLock api_lock(__func__); return container_ ? container_->Release() : StubIDirect3DSurface9::Release(); }

void Surface::AddRefPrivate() {
    ApiLock api_lock(__func__);
    if (container_) texture_add_private(static_cast<IDirect3DBaseTexture9*>(container_));
    else StubIDirect3DSurface9::AddRefPrivate();
}
void Surface::ReleasePrivate() {
    ApiLock api_lock(__func__);
    if (container_) texture_release_private(static_cast<IDirect3DBaseTexture9*>(container_));
    else StubIDirect3DSurface9::ReleasePrivate();
}

HRESULT Surface::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT Surface::GetContainer(REFIID riid, void** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    if (container_) return container_->QueryInterface(riid, out);
    return dev_->QueryInterface(riid, out);
}
HRESULT Surface::GetDesc(D3DSURFACE_DESC* d) {
    ApiLock api_lock(__func__);
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = d3dfmt;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = usage;
    d->Pool = pool;
    d->MultiSampleType = multisample;
    d->MultiSampleQuality = multisample_quality;
    d->Width = width;
    d->Height = height;
    return D3D_OK;
}
HRESULT Surface::LockRect(D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    ApiLock api_lock(__func__);
    if (!out || multisample!=D3DMULTISAMPLE_NONE) return D3DERR_INVALIDCALL;
    if (tex) {
        D3DLOCKED_RECT r{};
        const HRESULT hr = tex->lock(layer, level, &r, rect, flags);
        *out = r;
        return hr;
    }
    if (locked) return D3DERR_INVALIDCALL;
    if (cpu) {
        locked = true;
        dkMemBlockFlushCpuCache(cpu.block, cpu.offset, cpu.size);  // GPU-written (GetRenderTargetData)
        const uint32_t x = rect ? uint32_t(rect->left) : 0, y = rect ? uint32_t(rect->top) : 0;
        const bool ati = d3dfmt == fourcc('A','T','I','1') || d3dfmt == fourcc('A','T','I','2');
        out->pBits = cpu.cpu() + (ati ? size_t(y) * cpu_pitch + x
            : size_t(y / fmt->block_height) * cpu_pitch + x / fmt->block * fmt->bytes);
        out->Pitch = INT(cpu_pitch);
        return D3D_OK;
    }
    if (own.mem && fmt->dk != DkImageFormat_None) {  // a standalone render target: read back
        const RECT r = rect ? *rect : RECT{0, 0, LONG(width), LONG(height)};
        if(r.left<0 || r.top<0 || r.right<=r.left || r.bottom<=r.top ||
            r.right>LONG(width) || r.bottom>LONG(height)) return D3DERR_INVALIDCALL;
        rt_lock = TextureCore::Staging{};
        rt_lock.rect = RECT{0,0,LONG(width),LONG(height)};
        rt_lock.pitch = row_pitch(*fmt, width);
        rt_lock.mem = gpu().readback.alloc(rt_lock.pitch * level_rows(*fmt, height), 256);
        if(!rt_lock.mem)return D3DERR_OUTOFVIDEOMEMORY;
        rt_lock.readback = true;
        rt_lock.readonly = (flags & D3DLOCK_READONLY) != 0;
        dev_->read_image(own, 0, 0, rt_lock.mem, rt_lock.pitch, rt_lock.rect);
        out->pBits = rt_lock.mem.cpu()+size_t(r.top/fmt->block)*rt_lock.pitch+size_t(r.left/fmt->block)*fmt->bytes;
        out->Pitch = INT(rt_lock.pitch);
        locked = true;
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}
HRESULT Surface::UnlockRect() {
    ApiLock api_lock(__func__);
    if (tex) return tex->unlock(layer, level);
    if (!locked) return D3DERR_INVALIDCALL;
    locked = false;
    if (rt_lock.mem) {
        if (!rt_lock.readonly) dev_->upload(own, 0, 0, rt_lock.mem, rt_lock.pitch, rt_lock.rect);
        gpu().defer([m = rt_lock.mem] { gpu().readback.free_now(m); });
        rt_lock = TextureCore::Staging{};
    }
    return D3D_OK;
}

GpuImage* Surface::image() {
    ApiLock api_lock(__func__);
    if (tex) return tex->img.mem ? &tex->img : nullptr;
    return own.mem ? &own : nullptr;
}

DkImageView Surface::view() {
    ApiLock api_lock(__func__);
    GpuImage* g = image();
    return g->view(tex ? level : 0, tex ? layer : 0, false);
}

}  // namespace deko
