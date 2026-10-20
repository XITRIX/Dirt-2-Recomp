// Shaders on deko3d: the offline-translated pack (dirt2.dkshpack), shader
// objects and vertex declarations.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include "device.h"
#include "../shader_bytecode.h"
#include "recomp/host.h"
#include "recomp/sha256.h"

namespace deko {

namespace {

struct PackEntry {
    uint32_t dksh_off, dksh_size, meta_off, meta_size;
    bool loaded = false;
    uint64_t generation = 0;
    PackMeta meta;
    DkShader shader{};
};

std::vector<uint8_t> g_pack;
std::unordered_map<std::string, PackEntry> g_entries;  // hex SHA-256 of the d3dbc -> entry
std::mutex g_mu;

struct DkshHeader {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
};

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

bool load_entry(PackEntry& e) {
    e.meta = {};
    const uint8_t* m = g_pack.data() + e.meta_off;
    e.meta.pixel = m[0] == 1;
    const uint8_t ni = m[1], ns = m[2], nu = m[3];
    e.meta.block_bytes = rd32(m + 4);
    const uint8_t* p = m + 8;
    for (int i = 0; i < ni; ++i, p += 4) e.meta.inputs.push_back({p[0], p[1], p[2]});
    for (int i = 0; i < ns; ++i, p += 4) e.meta.samplers.push_back({p[0], p[1]});
    for (int i = 0; i < nu; ++i, p += 8) e.meta.uniforms.push_back({p[0], rd16(p + 2), rd16(p + 4)});
    e.meta.color_outputs = rd32(p);
    p += 4;
    // Optional appended flat variant keeps existing version-2 packs readable.
    const size_t consumed = size_t(p - m);
    if (e.meta_size >= consumed + 4) {
        // The variant record was introduced with the draw-state shader wrappers.
        // A zero-length flat variant still marks the extended shader interface.
        e.meta.has_draw_state = true;
        const uint32_t flat_size = rd32(p);
        if (flat_size && flat_size <= e.meta_size - consumed - 4) {
            std::vector<uint8_t> flat(p + 4, p + 4 + flat_size);
            if (!load_shader_binary(flat, &e.meta.flat_shader)) return false;
            e.meta.has_flat_shader = true;
        }
    }

    DkshHeader h;
    std::memcpy(&h, g_pack.data() + e.dksh_off, sizeof h);
    Gpu& g = gpu();
    const uint32_t off = (g.code_used + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (off + h.code_sz + DK_SHADER_CODE_UNUSABLE_SIZE > dkMemBlockGetSize(g.code)) {
        rt::log("deko3d: shader code memory full");
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g.code)) + off, g_pack.data() + e.dksh_off + h.control_sz,
                h.code_sz);
    g.code_used = off + h.code_sz;
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, g.code, off);
    sm.control = g_pack.data() + e.dksh_off;
    dkShaderInitialize(&e.shader, &sm);
    e.loaded = dkShaderIsValid(&e.shader);
    e.generation = g.generation;
    return e.loaded;
}

}  // namespace

bool load_shader_pack(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        rt::log("deko3d: no shader pack at %s", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    g_pack.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(g_pack.data(), 1, g_pack.size(), f) == g_pack.size();
    std::fclose(f);
    if (!ok || g_pack.size() < 12 || std::memcmp(g_pack.data(), "DKSP", 4) != 0 || rd32(&g_pack[4]) != 2) {
        rt::log("deko3d: %s is not a version 2 shader pack (rerun tools/scripts/dx9_shader_census.py)", path.c_str());
        return false;
    }
    const uint32_t n = rd32(&g_pack[8]);
    static const char* hex = "0123456789abcdef";
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = &g_pack[12 + i * 48];
        std::string key(64, ' ');
        for (int k = 0; k < 32; ++k) {
            key[k * 2] = hex[e[k] >> 4];
            key[k * 2 + 1] = hex[e[k] & 15];
        }
        g_entries[key] = PackEntry{rd32(e + 32), rd32(e + 36), rd32(e + 40), rd32(e + 44)};
    }
    rt::log("deko3d: shader pack %s: %u shaders", path.c_str(), n);
    return true;
}

bool load_shader_binary(const std::vector<uint8_t>& binary,DkShader* shader) {
    struct RawShader { std::vector<uint8_t> binary; DkShader shader; uint64_t generation=0; };
    static std::unordered_map<std::string,RawShader> cached;
    const auto key=recomp::sha256_hex(binary.data(),binary.size());
    auto found=cached.find(key);
    if(found!=cached.end() && found->second.generation==gpu().generation){*shader=found->second.shader;return true;}
    if(binary.size()<sizeof(DkshHeader))return false;
    DkshHeader h;std::memcpy(&h,binary.data(),sizeof(h));
    if(h.magic!=0x48534b44u || h.control_sz>binary.size() || h.code_sz>binary.size()-h.control_sz)return false;
    Gpu& g=gpu();const uint32_t off=(g.code_used+DK_SHADER_CODE_ALIGNMENT-1)&~(DK_SHADER_CODE_ALIGNMENT-1);
    if(off+h.code_sz+DK_SHADER_CODE_UNUSABLE_SIZE>dkMemBlockGetSize(g.code))return false;
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g.code))+off,binary.data()+h.control_sz,h.code_sz);
    g.code_used=off+h.code_sz;
    auto& entry=cached[key];entry.binary=binary;
    DkShaderMaker maker;dkShaderMakerDefaults(&maker,g.code,off);maker.control=entry.binary.data();
    dkShaderInitialize(&entry.shader,&maker);
    entry.generation=g.generation;
    if(!dkShaderIsValid(&entry.shader)){cached.erase(key);return false;}
    *shader=entry.shader;return true;
}

uint32_t shader_bytes(const DWORD* code) {
    return d3d9::shader_bytecode_size(code);
}

bool find_shader(const DWORD* code, uint32_t* bytes, const PackMeta** meta, const DkShader** shader) {
    *bytes = shader_bytes(code);
    *meta = nullptr;
    *shader = nullptr;
    if (!*bytes) return false;
    const std::string key = recomp::sha256_hex(reinterpret_cast<const uint8_t*>(code), *bytes);
    std::lock_guard lk(g_mu);
    auto it = g_entries.find(key);
    if (it == g_entries.end()) {
        // Optional evidence capture for shaders generated at runtime, which
        // cannot be collected from literal arrays by the offline pack builder.
        if (const char* directory = std::getenv("DIRT2_DEKO_CAPTURE_SHADERS")) {
            const std::string path = std::string(directory) + "/" + key + ".d3dbc";
            if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                if (std::fwrite(code,1,*bytes,file) != *bytes) rt::log("deko3d: shader capture write failed: %s",path.c_str());
                std::fclose(file);
            } else rt::log("deko3d: shader capture open failed: %s",path.c_str());
        }
        rt::log("deko3d: shader %s (%u bytes) is not in the pack: draws with it are skipped", key.c_str(), *bytes);
        return false;
    }
    if ((!it->second.loaded || it->second.generation!=gpu().generation) && !load_entry(it->second)) return false;
    it->second.meta.name = key.substr(0, 12);
    *meta = &it->second.meta;
    *shader = &it->second.shader;
    return true;
}

VertexShader::VertexShader(Device* d,const DWORD* code,uint32_t bytes,const PackMeta* m,const DkShader* sh)
    : meta(m),shader(sh),dev_(d),code_(code,code+bytes/4) { SetDeviceParent(d); }
PixelShader::PixelShader(Device* d,const DWORD* code,uint32_t bytes,const PackMeta* m,const DkShader* sh)
    : meta(m),shader(sh),dev_(d),code_(code,code+bytes/4) { SetDeviceParent(d); }
HRESULT VertexShader::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT VertexShader::GetFunction(void* data, UINT* size) {
    ApiLock api_lock(__func__);
    if (!size) return D3DERR_INVALIDCALL;
    const UINT bytes = UINT(code_.size() * 4);
    if (data) std::memcpy(data, code_.data(), std::min(*size, bytes));
    else *size = bytes;
    return D3D_OK;
}
HRESULT PixelShader::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT PixelShader::GetFunction(void* data, UINT* size) {
    ApiLock api_lock(__func__);
    if (!size) return D3DERR_INVALIDCALL;
    const UINT bytes = UINT(code_.size() * 4);
    if (data) std::memcpy(data, code_.data(), std::min(*size, bytes));
    else *size = bytes;
    return D3D_OK;
}

VertexDeclaration::VertexDeclaration(Device* d, const D3DVERTEXELEMENT9* e) : dev_(d) {
    SetDeviceParent(d);
    for (; e->Stream != 0xFF; ++e) elements.push_back(*e);
}
HRESULT VertexDeclaration::GetDevice(IDirect3DDevice9** out) {
    ApiLock api_lock(__func__);
    if(!out)return D3DERR_INVALIDCALL;
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9* out, UINT* count) {
    ApiLock api_lock(__func__);
    if (!count) return D3DERR_INVALIDCALL;
    if (out) {
        std::memcpy(out, elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
        out[elements.size()] = D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0};
    }
    *count = UINT(elements.size() + 1);
    return D3D_OK;
}

}  // namespace deko
