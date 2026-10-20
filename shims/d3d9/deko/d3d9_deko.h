// D3D9 on deko3d (Switch): the host IDirect3D9 the D3D9 bridge talks to
// (shims/d3d9/bridge.cpp), in place of DXVK. docs/SWITCH.md has the design.
//
// Each class derives from its generated Stub<Interface> (stubs.inc), which
// implements IUnknown and logs-and-fails every method; the classes here
// override what the game uses (DIRT2_COUNT_CALLS lists it).
#pragma once

#include <d3d9.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "gpu.h"

#include "object_refs.h"

namespace deko {

void log_unimplemented(const char* method);

template <typename T>
T stub_result(const char* method) {
    log_unimplemented(method);
    if constexpr (std::is_same_v<T, HRESULT>) return D3DERR_INVALIDCALL;
    else return T{};
}

#include "stubs.inc"

class Device;

// ---- Formats ----

struct FormatInfo {
    DkImageFormat dk = DkImageFormat_None;
    uint8_t block = 1;      // block width in pixels (4 for BC, 2 for packed YUV)
    uint8_t block_height = 1;
    uint8_t bytes = 4;      // bytes per block (or pixel)
    bool depth = false;
    DkImageSwizzle swizzle[4] = {DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue, DkImageSwizzle_Alpha};
};
const FormatInfo* format_info(D3DFORMAT f);  // nullptr: unsupported
uint32_t row_pitch(const FormatInfo& f, uint32_t width);
uint32_t level_rows(const FormatInfo& f, uint32_t height);
uint32_t mip(uint32_t v, uint32_t level);

// ---- Buffers ----

// Vertex and index buffer storage with versions: a DISCARD lock, or any lock
// while an unfinished frame still reads the current version, moves to a free
// version, so the CPU never writes what the GPU is reading.
struct BufferStore {
    struct Version {
        Alloc mem;
        uint64_t last_used = 0;  // frame_count of the last draw that read it (+1; 0 = never)
    };
    std::vector<Version> versions;
    int current = 0;
    uint32_t size = 0;

    bool init(uint32_t bytes);
    void release();
    uint8_t* lock(uint32_t offset, DWORD flags);
    DkGpuAddr addr() const { return versions[current].mem.gpu(); }
    void used() { versions[current].last_used = gpu().frame_count + 1; }
};

class VertexBuffer : public StubIDirect3DVertexBuffer9 {
public:
    VertexBuffer(Device* d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);
    ~VertexBuffer() override;
    bool ok() const { return !store.versions.empty(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { ApiLock api_lock(__func__); return D3DRTYPE_VERTEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void** data, DWORD flags) override;
    STDMETHOD(Unlock)() override { ApiLock api_lock(__func__); return D3D_OK; }
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC* desc) override;
    BufferStore store;

private:
    Device* dev_;
    DWORD usage_, fvf_;
    D3DPOOL pool_;
};

class IndexBuffer : public StubIDirect3DIndexBuffer9 {
public:
    IndexBuffer(Device* d, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~IndexBuffer() override;
    bool ok() const { return !store.versions.empty(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { ApiLock api_lock(__func__); return D3DRTYPE_INDEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void** data, DWORD flags) override;
    STDMETHOD(Unlock)() override { ApiLock api_lock(__func__); return D3D_OK; }
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC* desc) override;
    BufferStore store;
    D3DFORMAT format;

private:
    Device* dev_;
    DWORD usage_;
    D3DPOOL pool_;
};

// ---- Textures and surfaces ----

class Surface;

// Shared by 2D, cube and volume textures: the image and its per-level surfaces.
struct TextureCore {
    Device* dev = nullptr;
    GpuImage img;
    const FormatInfo* fmt = nullptr;
    D3DFORMAT d3dfmt = D3DFMT_UNKNOWN;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    DWORD lod = 0;
    D3DTEXTUREFILTERTYPE autogen_filter = D3DTEXF_LINEAR;
    bool autogen_dirty = true;
    uint32_t api_levels() const { return usage & D3DUSAGE_AUTOGENMIPMAP ? 1 : img.levels; }
    HRESULT set_autogen_filter(D3DTEXTUREFILTERTYPE filter);
    void generate_mips();
    std::vector<Surface*> surfaces;  // [layer * levels + level], created on demand
    // An in-progress lock of (layer, level): the staging memory it hands out.
    struct Staging {
        Alloc mem;
        uint32_t pitch = 0;
        RECT rect{};
        bool readback = false;  // a render target's contents, read back (readback pool)
        bool readonly = false;
    };
    std::unordered_map<uint32_t, Staging> locks;
    // CPU-visible contents must survive Unlock and partial subsequent locks.
    // Allocate lazily; GPU uploads use independent snapshots of these bytes.
    struct CpuLevel {
        size_t offset = 0, bytes = 0;
        uint32_t pitch = 0, slice = 0, api_pitch = 0, api_slice = 0;
    };
    // Native managed locks expose one contiguous backing allocation for the
    // mip chain. Separate vectors let legacy full-chain writes corrupt heaps.
    std::vector<CpuLevel> cpu_layout;
    std::vector<uint8_t> cpu_storage;
    void ensure_cpu_storage();
    HRESULT upload_cpu_level(uint32_t layer, uint32_t level);
    std::unordered_map<uint32_t, bool> box_readonly;
    HRESULT lock_box(uint32_t level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags);
    HRESULT unlock_box(uint32_t level);

    HRESULT lock(uint32_t layer, uint32_t level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags);
    HRESULT unlock(uint32_t layer, uint32_t level);
    Surface* surface(IUnknown* container, uint32_t layer, uint32_t level);
    void release();
};

// Resolve backend-owned textures without calling their public COM vtable.
TextureCore* texture_core(IDirect3DBaseTexture9* texture);
void texture_add_ref(IDirect3DBaseTexture9* texture);
void texture_release(IDirect3DBaseTexture9* texture);
void texture_add_private(IDirect3DBaseTexture9* texture);
void texture_release_private(IDirect3DBaseTexture9* texture);

class Texture : public StubIDirect3DTexture9 {
public:
    TextureCore core;
    bool init(Device* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
    ~Texture() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { ApiLock api_lock(__func__); return D3DRTYPE_TEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override;
    STDMETHOD_(DWORD, GetLOD)() override { ApiLock api_lock(__func__); return core.lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { ApiLock api_lock(__func__); return core.api_levels(); }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE filter) override { ApiLock lock(__func__); return core.set_autogen_filter(filter); }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { ApiLock lock(__func__); return core.autogen_filter; }
    STDMETHOD_(void, GenerateMipSubLevels)() override { ApiLock lock(__func__); core.generate_mips(); }
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC* desc) override;
    STDMETHOD(GetSurfaceLevel)(UINT level, IDirect3DSurface9** out) override;
    STDMETHOD(LockRect)(UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(UINT level) override;
    STDMETHOD(AddDirtyRect)(const RECT*) override { ApiLock api_lock(__func__); return D3D_OK; }
};

class CubeTexture : public StubIDirect3DCubeTexture9 {
public:
    TextureCore core;
    bool init(Device* d, UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
    ~CubeTexture() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { ApiLock api_lock(__func__); return D3DRTYPE_CUBETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override;
    STDMETHOD_(DWORD, GetLOD)() override { ApiLock api_lock(__func__); return core.lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { ApiLock api_lock(__func__); return core.api_levels(); }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE filter) override { ApiLock lock(__func__); return core.set_autogen_filter(filter); }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { ApiLock lock(__func__); return core.autogen_filter; }
    STDMETHOD_(void, GenerateMipSubLevels)() override { ApiLock lock(__func__); core.generate_mips(); }
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC* desc) override;
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** out) override;
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES face, UINT level) override;
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, const RECT*) override { ApiLock api_lock(__func__); return D3D_OK; }
};

class Volume;
class VolumeTexture : public StubIDirect3DVolumeTexture9 {
public:
    ULONG TextureParentAddRef() { return StubIDirect3DVolumeTexture9::AddRef(); }
    ULONG TextureParentRelease() { return StubIDirect3DVolumeTexture9::Release(); }
    TextureCore core;
    bool init(Device* d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
    ~VolumeTexture() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { ApiLock api_lock(__func__); return D3DRTYPE_VOLUMETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override;
    STDMETHOD_(DWORD, GetLevelCount)() override { ApiLock api_lock(__func__); return core.api_levels(); }
    STDMETHOD_(DWORD, GetLOD)() override { ApiLock api_lock(__func__); return core.lod; }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE filter) override { ApiLock lock(__func__); return core.set_autogen_filter(filter); }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { ApiLock lock(__func__); return core.autogen_filter; }
    STDMETHOD_(void, GenerateMipSubLevels)() override { ApiLock lock(__func__); core.generate_mips(); }
    STDMETHOD(GetLevelDesc)(UINT level, D3DVOLUME_DESC* desc) override;
    STDMETHOD(GetVolumeLevel)(UINT level,IDirect3DVolume9** volume) override;
    std::vector<Volume*> volumes;
    STDMETHOD(LockBox)(UINT level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags) override;
    STDMETHOD(UnlockBox)(UINT level) override;
};

class Volume : public StubIDirect3DVolume9 {
public:
    Volume(VolumeTexture* texture,UINT level) : texture_(texture),level_(level) {}
    STDMETHOD_(ULONG,AddRef)() override { ApiLock api_lock(__func__);return texture_->TextureParentAddRef(); }
    STDMETHOD_(ULONG,Release)() override { ApiLock api_lock(__func__);return texture_->TextureParentRelease(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** device) override { return texture_->GetDevice(device); }
    STDMETHOD(GetContainer)(REFIID iid,void** out) override { return texture_->QueryInterface(iid,out); }
    STDMETHOD(GetDesc)(D3DVOLUME_DESC* desc) override { return texture_->GetLevelDesc(level_,desc); }
    STDMETHOD(LockBox)(D3DLOCKED_BOX* out,const D3DBOX* box,DWORD flags) override { return texture_->LockBox(level_,out,box,flags); }
    STDMETHOD(UnlockBox)() override { return texture_->UnlockBox(level_); }
private:
    VolumeTexture* texture_;
    UINT level_;
};

DkMsMode multisample_mode(D3DMULTISAMPLE_TYPE type,DWORD quality);
bool multisample_valid(D3DMULTISAMPLE_TYPE type,DWORD quality);

// A surface: a level (and face) of a texture, or standalone (render target,
// depth stencil, offscreen plain). Offscreen plain surfaces in system memory
// live in CPU memory only (GetRenderTargetData's destination).
class Surface : public StubIDirect3DSurface9 {
public:
    Surface(Device* d, IUnknown* container, TextureCore* tex, uint32_t layer, uint32_t level);  // texture level
    Surface(Device* d, UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool, bool cpu_only, D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE, DWORD quality = 0);  // standalone
    ~Surface() override;
    bool ok() const;
    Device* owner() const { return dev_; }
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    void AddRefPrivate();
    void ReleasePrivate();
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { ApiLock api_lock(__func__); return D3DRTYPE_SURFACE; }
    STDMETHOD(GetContainer)(REFIID riid, void** out) override;
    STDMETHOD(GetDesc)(D3DSURFACE_DESC* desc) override;
    STDMETHOD(LockRect)(D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)() override;

    GpuImage* image();  // the GPU image holding this surface, or nullptr (CPU only)
    DkImageView view();
    uint32_t width, height;
    D3DFORMAT d3dfmt;
    const FormatInfo* fmt;
    DWORD usage;
    D3DMULTISAMPLE_TYPE multisample = D3DMULTISAMPLE_NONE;
    DWORD multisample_quality = 0;
    D3DPOOL pool;
    TextureCore* tex = nullptr;  // level of a texture (the texture owns the reference count)
    uint32_t layer = 0, level = 0;
    GpuImage own;                // standalone
    Alloc cpu;                   // CPU-only surface memory (readback pool)
    uint32_t cpu_pitch = 0;
    bool locked = false;
    TextureCore::Staging rt_lock;  // a locked standalone render target (read back)

private:
    Device* dev_;
    IUnknown* container_ = nullptr;
};

// ---- Shaders and declarations ----

// A shader's interface, from the pack (dx9_shader_census.py: shader_meta).
struct PackMeta {
    bool pixel = false;
    bool fixed_function = false;
    // Extended packs implement alpha/fog and clip planes through draw-state UBOs.
    // Original packs use the hardware alpha test and do not read those UBOs.
    bool has_draw_state = false;
    bool has_flat_shader = false;
    DkShader flat_shader{};
    struct Input {
        uint8_t usage, index, reg;  // D3DDECLUSAGE, usage index, input register (= attribute location)
    };
    struct Sampler {
        uint8_t reg, type;  // sN; 0 2D, 1 cube, 2 volume, 3 legacy bound dimension
        uint8_t binding = 0xff; // optional distinct binding for generic FFP samplers
    };
    struct Uniform {
        uint8_t type;  // 0 float4, 1 int4, 2 bool
        uint16_t reg, count;
    };
    std::vector<Input> inputs;
    std::vector<Sampler> samplers;
    std::vector<Uniform> uniforms;  // in the uniform block's order, 16 bytes per register
    uint32_t block_bytes = 0;
    uint32_t color_outputs = 1;  // pixel shaders: bit n set when it writes oCn
    std::string name;  // first 12 hex digits of the bytecode's SHA-256 (the pack key), for logs
};

class VertexShader : public StubIDirect3DVertexShader9 {
public:
    VertexShader(Device* d, const DWORD* code, uint32_t bytes, const PackMeta* m, const DkShader* sh);
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetFunction)(void* data, UINT* size) override;
    const PackMeta* meta;
    const DkShader* shader;  // nullptr: not in the pack (draws with it are skipped)

private:
    Device* dev_;
    std::vector<DWORD> code_;
};

class PixelShader : public StubIDirect3DPixelShader9 {
public:
    unsigned major_version() const { return (code_[0] >> 8) & 255; }
    PixelShader(Device* d, const DWORD* code, uint32_t bytes, const PackMeta* m, const DkShader* sh);
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetFunction)(void* data, UINT* size) override;
    const PackMeta* meta;
    const DkShader* shader;

private:
    Device* dev_;
    std::vector<DWORD> code_;
};

class VertexDeclaration : public StubIDirect3DVertexDeclaration9 {
public:
    VertexDeclaration(Device* d, const D3DVERTEXELEMENT9* elements);
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9* elements, UINT* count) override;
    DWORD fvf = 0;
    std::vector<D3DVERTEXELEMENT9> elements;  // without the end marker

private:
    Device* dev_;
};

struct DrawShader { const PackMeta* meta=nullptr;const DkShader* shader=nullptr; };
bool load_shader_binary(const std::vector<uint8_t>& binary,DkShader* shader);

// Shader pack (dirt2.dkshpack, tools/scripts/dx9_shader_census.py).
bool load_shader_pack(const std::string& path);
// The pack's shader for this bytecode: its interface and its loaded DkShader.
bool find_shader(const DWORD* code, uint32_t* bytes, const PackMeta** meta, const DkShader** shader);
uint32_t shader_bytes(const DWORD* code);

// ---- IDirect3D9 ----

class D3D9 : public StubIDirect3D9 {
public:
    STDMETHOD_(UINT, GetAdapterCount)() override { ApiLock api_lock(__func__); return 1; }
    STDMETHOD(GetAdapterIdentifier)(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override;
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT adapter, D3DFORMAT format) override;
    STDMETHOD(EnumAdapterModes)(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) override;
    STDMETHOD(GetAdapterDisplayMode)(UINT adapter, D3DDISPLAYMODE* mode) override;
    STDMETHOD(CheckDeviceType)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { ApiLock api_lock(__func__); return D3D_OK; }
    STDMETHOD(CheckDeviceFormat)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage,
                                 D3DRESOURCETYPE rtype, D3DFORMAT format) override;
    STDMETHOD(CheckDeviceMultiSampleType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                          D3DMULTISAMPLE_TYPE ms, DWORD* quality) override;
    STDMETHOD(CheckDepthStencilMatch)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { ApiLock api_lock(__func__); return D3D_OK; }
    STDMETHOD(GetDeviceCaps)(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override;
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT) override { ApiLock api_lock(__func__); return reinterpret_cast<HMONITOR>(1); }
    STDMETHOD(CreateDevice)(UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                            D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) override;
};

void fill_caps(D3DCAPS9* caps);

}  // namespace deko
