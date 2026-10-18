// Shaders on deko3d: the offline-translated pack (dirt2.dkshpack), shader
// objects and vertex declarations.
#include <cstdio>
#include <cstring>
#include <mutex>

#include "device.h"
#include "recomp/host.h"
#include "recomp/sha256.h"

namespace deko {

namespace {

struct PackEntry {
    uint32_t dksh_off, dksh_size, meta_off, meta_size;
    bool loaded = false;
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
    const uint8_t* m = g_pack.data() + e.meta_off;
    e.meta.pixel = m[0] == 1;
    const uint8_t ni = m[1], ns = m[2], nu = m[3];
    e.meta.block_bytes = rd32(m + 4);
    const uint8_t* p = m + 8;
    for (int i = 0; i < ni; ++i, p += 4) e.meta.inputs.push_back({p[0], p[1], p[2]});
    for (int i = 0; i < ns; ++i, p += 4) e.meta.samplers.push_back({p[0], p[1]});
    for (int i = 0; i < nu; ++i, p += 8) e.meta.uniforms.push_back({p[0], rd16(p + 2), rd16(p + 4)});

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
    if (!ok || g_pack.size() < 12 || std::memcmp(g_pack.data(), "DKSP", 4) != 0 || rd32(&g_pack[4]) != 1) {
        rt::log("deko3d: %s is not a version 1 shader pack", path.c_str());
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

uint32_t shader_bytes(const DWORD* code) {
    // Version token, then instructions and comments to the end token.
    uint32_t i = 1;
    for (;;) {
        const DWORD t = code[i];
        if (t == 0x0000FFFF) return (i + 1) * 4;
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + (t >> 16);
        else i += 1 + ((t >> 24) & 0xF);
        if (i > (1u << 20)) return 0;
    }
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
        rt::log("deko3d: shader %s (%u bytes) is not in the pack: draws with it are skipped", key.c_str(), *bytes);
        return false;
    }
    if (!it->second.loaded && !load_entry(it->second)) return false;
    *meta = &it->second.meta;
    *shader = &it->second.shader;
    return true;
}

HRESULT VertexShader::GetDevice(IDirect3DDevice9** out) {
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT VertexShader::GetFunction(void* data, UINT* size) {
    if (!size) return D3DERR_INVALIDCALL;
    if (data) std::memcpy(data, code_.data(), code_.size() * 4);
    *size = UINT(code_.size() * 4);
    return D3D_OK;
}
HRESULT PixelShader::GetDevice(IDirect3DDevice9** out) {
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT PixelShader::GetFunction(void* data, UINT* size) {
    if (!size) return D3DERR_INVALIDCALL;
    if (data) std::memcpy(data, code_.data(), code_.size() * 4);
    *size = UINT(code_.size() * 4);
    return D3D_OK;
}

VertexDeclaration::VertexDeclaration(Device* d, const D3DVERTEXELEMENT9* e) : dev_(d) {
    for (; e->Stream != 0xFF; ++e) elements.push_back(*e);
}
HRESULT VertexDeclaration::GetDevice(IDirect3DDevice9** out) {
    dev_->AddRef();
    *out = dev_;
    return D3D_OK;
}
HRESULT VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9* out, UINT* count) {
    if (!count) return D3DERR_INVALIDCALL;
    if (out) {
        std::memcpy(out, elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
        out[elements.size()] = D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0};
    }
    *count = UINT(elements.size() + 1);
    return D3D_OK;
}

}  // namespace deko
