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
    uint8_t block = 1;      // block width/height in pixels (4 for BC)
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
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VERTEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void** data, DWORD flags) override;
    STDMETHOD(Unlock)() override { return D3D_OK; }
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
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_INDEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void** data, DWORD flags) override;
    STDMETHOD(Unlock)() override { return D3D_OK; }
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

    HRESULT lock(uint32_t layer, uint32_t level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags);
    HRESULT unlock(uint32_t layer, uint32_t level);
    Surface* surface(IUnknown* container, uint32_t layer, uint32_t level);
    void release();
};

class Texture : public StubIDirect3DTexture9 {
public:
    TextureCore core;
    bool init(Device* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
    ~Texture() override { core.release(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override;
    STDMETHOD_(DWORD, GetLOD)() override { return core.lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return core.img.levels; }
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC* desc) override;
    STDMETHOD(GetSurfaceLevel)(UINT level, IDirect3DSurface9** out) override;
    STDMETHOD(LockRect)(UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(UINT level) override;
    STDMETHOD(AddDirtyRect)(const RECT*) override { return D3D_OK; }
};

class CubeTexture : public StubIDirect3DCubeTexture9 {
public:
    TextureCore core;
    bool init(Device* d, UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
    ~CubeTexture() override { core.release(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_CUBETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override;
    STDMETHOD_(DWORD, GetLOD)() override { return core.lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return core.img.levels; }
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC* desc) override;
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** out) override;
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES face, UINT level) override;
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, const RECT*) override { return D3D_OK; }
};

class VolumeTexture : public StubIDirect3DVolumeTexture9 {
public:
    TextureCore core;
    bool init(Device* d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool);
    ~VolumeTexture() override { core.release(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VOLUMETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod) override;
    STDMETHOD_(DWORD, GetLevelCount)() override { return core.img.levels; }
    STDMETHOD_(DWORD, GetLOD)() override { return core.lod; }
    STDMETHOD(GetLevelDesc)(UINT level, D3DVOLUME_DESC* desc) override;
    STDMETHOD(LockBox)(UINT level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags) override;
    STDMETHOD(UnlockBox)(UINT level) override;
    Alloc staging;
    uint32_t staging_level = 0;
};

// A surface: a level (and face) of a texture, or standalone (render target,
// depth stencil, offscreen plain). Offscreen plain surfaces in system memory
// live in CPU memory only (GetRenderTargetData's destination).
class Surface : public StubIDirect3DSurface9 {
public:
    Surface(Device* d, IUnknown* container, TextureCore* tex, uint32_t layer, uint32_t level);  // texture level
    Surface(Device* d, UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool, bool cpu_only);  // standalone
    ~Surface() override;
    bool ok() const;
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_SURFACE; }
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
    D3DPOOL pool;
    TextureCore* tex = nullptr;  // level of a texture (the texture owns the reference count)
    uint32_t layer = 0, level = 0;
    GpuImage own;                // standalone
    Alloc cpu;                   // CPU-only surface memory (readback pool)
    uint32_t cpu_pitch = 0;
    TextureCore::Staging rt_lock;  // a locked standalone render target (read back)

private:
    Device* dev_;
    IUnknown* container_ = nullptr;
};

// ---- Shaders and declarations ----

// A shader's interface, from the pack (dx9_shader_census.py: shader_meta).
struct PackMeta {
    bool pixel = false;
    struct Input {
        uint8_t usage, index, reg;  // D3DDECLUSAGE, usage index, input register (= attribute location)
    };
    struct Sampler {
        uint8_t reg, type;  // sN; 0 2D, 1 cube, 2 volume
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
    VertexShader(Device* d, const DWORD* code, uint32_t bytes, const PackMeta* m, const DkShader* sh)
        : meta(m), shader(sh), dev_(d), code_(code, code + bytes / 4) {}
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
    PixelShader(Device* d, const DWORD* code, uint32_t bytes, const PackMeta* m, const DkShader* sh)
        : meta(m), shader(sh), dev_(d), code_(code, code + bytes / 4) {}
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
    std::vector<D3DVERTEXELEMENT9> elements;  // without the end marker

private:
    Device* dev_;
};

// Shader pack (dirt2.dkshpack, tools/scripts/dx9_shader_census.py).
bool load_shader_pack(const std::string& path);
// The pack's shader for this bytecode: its interface and its loaded DkShader.
bool find_shader(const DWORD* code, uint32_t* bytes, const PackMeta** meta, const DkShader** shader);
uint32_t shader_bytes(const DWORD* code);

// ---- IDirect3D9 ----

class D3D9 : public StubIDirect3D9 {
public:
    STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
    STDMETHOD(GetAdapterIdentifier)(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override;
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT adapter, D3DFORMAT format) override;
    STDMETHOD(EnumAdapterModes)(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) override;
    STDMETHOD(GetAdapterDisplayMode)(UINT adapter, D3DDISPLAYMODE* mode) override;
    STDMETHOD(CheckDeviceType)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { return D3D_OK; }
    STDMETHOD(CheckDeviceFormat)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage,
                                 D3DRESOURCETYPE rtype, D3DFORMAT format) override;
    STDMETHOD(CheckDeviceMultiSampleType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                          D3DMULTISAMPLE_TYPE ms, DWORD* quality) override;
    STDMETHOD(CheckDepthStencilMatch)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
    STDMETHOD(GetDeviceCaps)(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override;
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT) override { return reinterpret_cast<HMONITOR>(1); }
    STDMETHOD(CreateDevice)(UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                            D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) override;
};

void fill_caps(D3DCAPS9* caps);

}  // namespace deko
