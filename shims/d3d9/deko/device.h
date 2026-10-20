// IDirect3DDevice9 on deko3d: D3D9 state kept as the game sets it, turned into
// deko3d state at each draw (only what changed). See d3d9_deko.h.
#pragma once

#include <map>
#include <memory>

#include "d3d9_deko.h"
#include "stateblock.h"
#include "constant_upload.h"

namespace deko {
class FixedFunction;
class SwapChain;

class Device : public StubIDirect3DDevice9 {
public:
    Device(D3D9* parent, HWND window, DWORD flags, const D3DPRESENT_PARAMETERS& pp);
    ~Device() override;
    bool init();
    D3D9* parent() const { return parent_; }

    // ---- device ----
    STDMETHOD(TestCooperativeLevel)() override { ApiLock api_lock(__func__); return D3D_OK; }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override;
    STDMETHOD(EvictManagedResources)() override { ApiLock api_lock(__func__); return D3D_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D9** out) override;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9* caps) override;
    STDMETHOD(GetDisplayMode)(UINT swapchain, D3DDISPLAYMODE* mode) override;
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS* p) override;
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pp) override;
    STDMETHOD(Present)(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) override;
    STDMETHOD(GetSwapChain)(UINT index,IDirect3DSwapChain9** out) override;
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS* parameters, IDirect3DSwapChain9** out) override;
    STDMETHOD_(UINT,GetNumberOfSwapChains)() override { return 1; }
    STDMETHOD(GetFrontBufferData)(UINT swapchain, IDirect3DSurface9* out) override;
    STDMETHOD(GetBackBuffer)(UINT swapchain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out) override;
    STDMETHOD_(void, SetGammaRamp)(UINT, DWORD, const D3DGAMMARAMP*) override { ApiLock api_lock(__func__);}
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(ValidateDevice)(DWORD* passes) override;
    STDMETHOD(SetPaletteEntries)(UINT index,const PALETTEENTRY* entries) override;
    STDMETHOD(GetPaletteEntries)(UINT index,PALETTEENTRY* entries) override;
    STDMETHOD(SetCurrentTexturePalette)(UINT index) override;
    STDMETHOD(GetCurrentTexturePalette)(UINT* index) override;

    STDMETHOD(CreateQuery)(D3DQUERYTYPE type, IDirect3DQuery9** out) override;

    // ---- resources ----
    STDMETHOD(CreateTexture)(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                             IDirect3DTexture9** out, HANDLE* shared) override;
    STDMETHOD(CreateVolumeTexture)(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                   IDirect3DVolumeTexture9** out, HANDLE* shared) override;
    STDMETHOD(CreateCubeTexture)(UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                 IDirect3DCubeTexture9** out, HANDLE* shared) override;
    STDMETHOD(CreateVertexBuffer)(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out,
                                  HANDLE* shared) override;
    STDMETHOD(CreateIndexBuffer)(UINT length, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9** out,
                                 HANDLE* shared) override;
    STDMETHOD(CreateRenderTarget)(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL lockable,
                                  IDirect3DSurface9** out, HANDLE* shared) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD quality,
                                         BOOL discard, IDirect3DSurface9** out, HANDLE* shared) override;
    STDMETHOD(CreateOffscreenPlainSurface)(UINT w, UINT h, D3DFORMAT f, D3DPOOL pool, IDirect3DSurface9** out,
                                           HANDLE* shared) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst,
                             const POINT* dst_point) override;
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) override;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* rt, IDirect3DSurface9* dst) override;
    STDMETHOD(StretchRect)(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const RECT* dst_rect,
                           D3DTEXTUREFILTERTYPE filter) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) override;

    // ---- targets and viewport ----
    STDMETHOD(SetRenderTarget)(DWORD index, IDirect3DSurface9* surface) override;
    STDMETHOD(GetRenderTarget)(DWORD index, IDirect3DSurface9** out) override;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* surface) override;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** out) override;
    STDMETHOD(Clear)(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    STDMETHOD(SetViewport)(const D3DVIEWPORT9* vp) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT9* vp) override;
    STDMETHOD(SetScissorRect)(const RECT* rect) override;
    STDMETHOD(GetScissorRect)(RECT* rect) override;

    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9** out) override;
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) override;

    // ---- state ----
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE state, DWORD* value) override;
    STDMETHOD(SetSamplerState)(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    STDMETHOD(GetSamplerState)(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) override;
    STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9* texture) override;
    STDMETHOD(GetTexture)(DWORD stage, IDirect3DBaseTexture9** out) override;
    STDMETHOD(SetTextureStageState)(DWORD stage,D3DTEXTURESTAGESTATETYPE type,DWORD value) override;
    STDMETHOD(GetTextureStageState)(DWORD stage,D3DTEXTURESTAGESTATETYPE type,DWORD* value) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE state,const D3DMATRIX* matrix) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE state,D3DMATRIX* matrix) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE state,const D3DMATRIX* matrix) override;
    STDMETHOD(SetMaterial)(const D3DMATERIAL9* material) override;
    STDMETHOD(GetMaterial)(D3DMATERIAL9* material) override;
    STDMETHOD(SetLight)(DWORD index,const D3DLIGHT9* light) override;
    STDMETHOD(GetLight)(DWORD index,D3DLIGHT9* light) override;
    STDMETHOD(LightEnable)(DWORD index,BOOL enabled) override;
    STDMETHOD(GetLightEnable)(DWORD index,BOOL* enabled) override;
    STDMETHOD(SetClipPlane)(DWORD index,const float* plane) override;
    STDMETHOD(GetClipPlane)(DWORD index,float* plane) override;
    STDMETHOD(GetFVF)(DWORD* fvf) override;
    STDMETHOD(GetStreamSourceFreq)(UINT stream,UINT* divider) override;
    STDMETHOD(SetFVF)(DWORD fvf) override;
    STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) override;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* decl) override;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** out) override;
    STDMETHOD(CreateVertexShader)(const DWORD* code, IDirect3DVertexShader9** out) override;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* shader) override;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** out) override;
    STDMETHOD(CreatePixelShader)(const DWORD* code, IDirect3DPixelShader9** out) override;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* shader) override;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** out) override;
    STDMETHOD(SetVertexShaderConstantF)(UINT reg, const float* data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantF)(UINT reg, float* data, UINT count) override;
    STDMETHOD(SetVertexShaderConstantI)(UINT reg, const int* data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantI)(UINT reg,int* data,UINT count) override;
    STDMETHOD(SetVertexShaderConstantB)(UINT reg, const BOOL* data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantB)(UINT reg,BOOL* data,UINT count) override;
    STDMETHOD(SetPixelShaderConstantF)(UINT reg, const float* data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantF)(UINT reg, float* data, UINT count) override;
    STDMETHOD(SetPixelShaderConstantI)(UINT reg, const int* data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantI)(UINT reg,int* data,UINT count) override;
    STDMETHOD(SetPixelShaderConstantB)(UINT reg, const BOOL* data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantB)(UINT reg,BOOL* data,UINT count) override;
    STDMETHOD(SetStreamSource)(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) override;
    STDMETHOD(GetStreamSource)(UINT stream, IDirect3DVertexBuffer9** vb, UINT* offset, UINT* stride) override;
    STDMETHOD(SetStreamSourceFreq)(UINT stream, UINT divider) override;
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* ib) override;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** out) override;

    // ---- draws ----
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices, UINT start,
                                    UINT count) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count,
                                      const void* indices, D3DFORMAT index_format, const void* data,
                                      UINT stride) override;

    // Clears a new render target (every level and face): colour to 0, depth to 1, as
    // DXVK initialises images. Pool memory is reused, and leftover data in a float
    // target poisoned the game's luminance average (the picture faded to black).
    void clear_new_image(GpuImage& img, bool depth);
    // Reads an image region into CPU-readable memory: copy, wait for the GPU, invalidate the CPU cache.
    void read_image(GpuImage& img, uint32_t layer, uint32_t level, const Alloc& out, uint32_t pitch_bytes, const RECT& r, uint32_t depth = 1);
    // Copies of a surface region (StretchRect, UpdateSurface).
    void blit(Surface* src, const RECT* src_rect, Surface* dst, const RECT* dst_rect, bool linear);
    // Records an upload of staging memory into an image level/layer.
    // pitch_bytes: the staging rows' pitch; slice_bytes: between volume slices (0: rows x pitch).
    void upload(GpuImage& img, uint32_t layer, uint32_t level, const Alloc& data, uint32_t pitch_bytes, const RECT& r,
                uint32_t z = 0, uint32_t depth = 1, uint32_t slice_bytes = 0);
    // Texture-owned LOD affects the sampler even when SetTexture is unchanged.
    void texture_lod_changed(const TextureCore* texture);
    bool software_vertex_processing() const { return (flags_ & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0; }

private:
    friend class StateBlock;
    friend class FixedFunction;
    friend class SwapChain;
    SwapChain* swap_chain_=nullptr;
    std::unique_ptr<FixedFunction> fixed_function_;
    DrawShader draw_vs_,draw_ps_;
    bool ffp_dirty_=true;
    Alloc fragment_state_,vertex_state_;
    std::array<float,24> vertex_state_cache_{};
    void bind_vertex_state();
    std::array<uint32_t,12> fragment_state_cache_{};
    void bind_fragment_state();
    GpuImage feedback_images_[20];
    TextureCore* feedback_sources_[20]{};
    uint32_t active_texture_mask() const;
    bool prepare_feedback(uint32_t sampled_mask);
    DkShader depth_copy_shaders_[3]{};
    bool depth_copy_loaded_=false;
    bool alpha_to_coverage() const;
    bool point_list_=false;
    void resolve_depth_command();
    bool resolve_depth(Surface* source,Surface* target,const RECT* sr=nullptr,const RECT* dr=nullptr);
    StateBlock* recording_ = nullptr;
    static constexpr int kSamplers = 20;  // 16 pixel + 4 vertex (D3DVERTEXTEXTURESAMPLER0 = 257 -> 16)
    static constexpr int kStreams = 16;

    struct Stream {
        VertexBuffer* vb = nullptr;
        UINT offset = 0, stride = 0, freq = 1;
    };

    bool prepare_draw(D3DPRIMITIVETYPE type, uint32_t* instances);
    void capture_draw(D3DPRIMITIVETYPE type, int base, uint32_t start, uint32_t count, uint32_t instances,
                      bool indexed, const void* up_indices = nullptr, D3DFORMAT index_format = D3DFMT_UNKNOWN);
    // DIRT2_DEKO_SYNC=1: waits for the GPU after each draw; a draw still running after 2 s is logged.
    void sync_after_draw(const char* kind, uint32_t prims, uint32_t instances, int base, uint32_t start);
    // DIRT2_DEKO_SYNC=1: the same after any other GPU operation (printf-style description).
    void sync(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void self_test();  // DIRT2_DEKO_SELFTEST=1: clear and present 180 frames, nothing from the game
    // DIRT2_DEKO_DUMP=<frame>: every render target drawn into during that frame
    // is saved as <data>/dump/NN_WxH_fmtF.bmp after it, with value statistics and
    // the shaders that drew into it logged (first-bound order).
    void dump_note_draw();
    void dump_targets();
    void flush_targets(bool publish = true);
    void flush_viewport();
    void flush_raster_state();
    void flush_blend_state();
    void flush_depth_stencil();
    bool flush_vertex_input(uint32_t* instances);
    void flush_textures();
    void flush_constants();
    int sampler_id(int s,bool compare=false);
    void make_back_buffer();
    void advance_back_buffers();
    template <typename T>
    void set_ref(T*& slot, T* obj);

    D3D9* parent_;
    HWND window_;
    DWORD flags_;
    D3DPRESENT_PARAMETERS pp_;

    Surface* back_buffer_ = nullptr;
    std::vector<Surface*> extra_back_buffers_;
    Surface* auto_depth_ = nullptr;
    Surface* targets_[4] = {};
    Surface* depth_ = nullptr;
    D3DVIEWPORT9 viewport_{};
    RECT scissor_{};

    DWORD tss_[8][33] = {};
    D3DMATRIX transforms_[512] = {};
    D3DMATERIAL9 material_{};
    struct Light { D3DLIGHT9 value{};BOOL enabled=FALSE; };
    std::map<DWORD,Light> lights_;
    float clip_planes_[6][4] = {};
    DWORD fvf_ = 0;
    bool in_scene_ = false;
    std::map<UINT,std::array<PALETTEENTRY,256>> palettes_;
    UINT current_palette_ = 0;
    DWORD rs_[256] = {};
    DWORD ss_[kSamplers][14] = {};
    IDirect3DBaseTexture9* textures_[kSamplers] = {};
    Stream streams_[kStreams];
    IndexBuffer* indices_ = nullptr;
    VertexDeclaration* decl_ = nullptr;
    VertexShader* vs_ = nullptr;
    PixelShader* ps_ = nullptr;
    std::map<DWORD, VertexDeclaration*> fvf_decls_;

    float vsf_[256][4] = {}, psf_[224][4] = {};
    int vsi_[16][4] = {}, psi_[16][4] = {};
    BOOL vsb_[16] = {}, psb_[16] = {};

    // Dirty tracking: deko3d state is re-recorded only when the D3D9 state behind it changed.
    enum Dirty : uint32_t {
        kTargets = 1 << 0, kViewport = 1 << 1, kRaster = 1 << 2, kBlend = 1 << 3, kDepthStencil = 1 << 4,
        kVertexInput = 1 << 5, kTextures = 1 << 6, kVsConst = 1 << 7, kPsConst = 1 << 8, kShaders = 1 << 9,
        kAll = 0xFFFFFFFF,
    };
    uint32_t dirty_ = kAll;
    // Isolate state-cache errors from GPU ordering without per-draw CPU waits.
    bool rebind_state_ = false;

    // Per-stage uniform buffers, updated in the command stream (dkCmdBufPushConstants).
    Alloc ubo_[2];
    ConstantUpload<8192> constant_upload_[2];
    uint64_t constant_bytes_full_ = 0, constant_bytes_sent_ = 0;
    std::vector<uint8_t> staging_;
    // Sampler descriptors by D3D9 sampler state.
    std::map<std::array<DWORD, 11>, int> samplers_;
    uint32_t draws_ = 0, skipped_draws_ = 0;
    // A copy, blit, upload or clear wrote an image: the next draw waits for it
    // and invalidates the texture cache first (deko3d leaves hazards to the caller).
    bool copy_barrier_ = false;
    uint64_t list_seen_ = ~0ull;  // gpu().list_id this device last recorded its state into
    GpuImage front_buffer_;
    GpuImage dummy_,dummy_cube_,dummy_volume_;              // 1x1 black texture for samplers without one
    // DrawPrimitiveUP / DrawIndexedPrimitiveUP: stream 0 is transient memory for one draw.
    struct {
        bool active = false;
        DkGpuAddr addr = 0;
        uint32_t size = 0, stride = 0;
        const uint8_t* cpu = nullptr;
    } up_;
    uint32_t present_count_ = 0, fps_presents_ = 0;
    uint64_t draw_total_ = 0;
    uint32_t dump_frame_ = 0;
    // DIRT2_FRAME_DUMP=<frame>: the distinct output states of that frame's MRT draws are logged.
    uint32_t state_log_frame_ = 0;
    std::vector<std::string> state_log_;
    void log_mrt_state();
    void probe_mrt(const char* phase, bool full);
    void probe_ui_inputs();
    uint32_t ui_probe_frame_ = 0, ui_probe_draw_ = 0;
    std::vector<DkGpuAddr> ui_probe_textures_;  // image identity survives CPU object-address reuse
    std::string manual_capture_seen_;
    uint32_t mrt_probe_frame_ = 0, mrt_probe_draw_ = 0;
    struct DumpTarget {
        Surface* surface;
        uint32_t draws = 0;
        std::vector<std::string> shaders;  // "vs/ps", first few distinct
    };
    std::vector<DumpTarget> dump_;
    uint64_t fps_t0_ = 0;
};

}  // namespace deko
