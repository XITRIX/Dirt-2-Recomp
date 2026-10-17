// d3dx9_42: shader compile, constant tables, input semantics, texture helpers.
//
// D3DXCompileShader serves the game's fixed set of run-time shaders from
// bytecode assembled by tools/scripts/sm3asm.py. ID3DXConstantTable is built
// from a shader's CTAB block and works for any SM1-3 bytecode, including the
// precompiled shader packs.
#include <d3d9.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../guest_com.h"

namespace shim {
void* d3d9_host(uint32_t proxy);
}

using namespace shim;

namespace {

struct BuiltinShader {
    const char* function;
    const char* profile;
    const uint32_t* words;
    uint32_t count;
};
#include "builtin_shaders.inc"

// D3D_OK, D3DERR_INVALIDCALL and E_FAIL come from d3d9.h.

// ---- ID3DXBuffer ----

struct BufferData {
    uint32_t data, size;
};
std::mutex g_mu;
std::unordered_map<uint32_t, BufferData> g_buffers;

uint32_t buf_pointer(X86*, const Args& a) {
    std::lock_guard lk(g_mu);
    return g_buffers[a[0]].data;
}
uint32_t buf_size(X86*, const Args& a) {
    std::lock_guard lk(g_mu);
    return g_buffers[a[0]].size;
}

uint32_t buffer_vtable() {
    static uint32_t vt = make_vtable({&COM_FN<2, com_qi_self>, &COM_FN<0, com_addref>, &COM_FN<0, com_release>,
                                      &COM_FN<0, buf_pointer>, &COM_FN<0, buf_size>});
    return vt;
}

uint32_t new_buffer(const void* data, uint32_t size) {
    const uint32_t o = new_object(buffer_vtable(), 0);
    const uint32_t mem = rt::heap_alloc(size ? size : 4);
    if (data) std::memcpy(rt::host_ptr(mem), data, size);
    std::lock_guard lk(g_mu);
    g_buffers[o] = BufferData{mem, size};
    return o;
}

// ---- ID3DXConstantTable ----

enum RegSet : uint16_t { RS_BOOL = 0, RS_INT4 = 1, RS_FLOAT4 = 2, RS_SAMPLER = 3 };
enum Class : uint16_t { C_SCALAR = 0, C_VECTOR = 1, C_MATRIX_ROWS = 2, C_MATRIX_COLUMNS = 3, C_OBJECT = 4, C_STRUCT = 5 };

struct Constant {
    std::string name;
    uint32_t guest_name = 0;  // name string inside the guest CTAB copy
    uint16_t set, reg, count;
    uint16_t cls, type, rows, cols, elements, members;
    uint32_t default_value = 0;  // guest pointer, 0 if none
};

struct Table {
    bool pixel = false;
    uint32_t ctab = 0, ctab_size = 0;  // guest copy of the CTAB body
    uint32_t creator = 0, version = 0;
    std::vector<Constant> constants;
};
std::unordered_map<uint32_t, Table> g_tables;

// Handles: 0xFFD00000 | constant << 10 | element+1 (0 = whole constant).
constexpr uint32_t kHandleBase = 0xFFD00000;
uint32_t make_handle(uint32_t ci, uint32_t elem_plus1) { return kHandleBase | (ci << 10) | elem_plus1; }

// Resolves a handle, which may also be a guest pointer to a constant's name.
const Constant* resolve(const Table& t, uint32_t h, uint32_t* element) {
    *element = 0;
    if ((h & 0xFFF00000) == kHandleBase) {
        const uint32_t ci = (h >> 10) & 0x3FF;
        if (ci >= t.constants.size()) return nullptr;
        if (h & 0x3FF) *element = (h & 0x3FF);
        return &t.constants[ci];
    }
    if (!h) return nullptr;
    const char* name = rt::host_str(h);
    for (const Constant& c : t.constants)
        if (c.name == name || (name[0] == '$' && c.name == name + 1)) return &c;
    return nullptr;
}

uint32_t regs_per_element(const Constant& c) {
    switch (c.cls) {
        case C_MATRIX_ROWS: return c.rows;
        case C_MATRIX_COLUMNS: return c.cols;
        default: return 1;
    }
}

bool parse_ctab(uint32_t function, Table& t) {
    const uint32_t version = rd32(function);
    t.pixel = (version >> 16) == 0xFFFF;
    for (uint32_t p = function + 4;; ) {
        const uint32_t tok = rd32(p);
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) {
            const uint32_t size = tok >> 16;
            if (rd32(p + 4) == 0x42415443) {  // 'CTAB'
                const uint32_t body = p + 8, bytes = (size - 1) * 4;
                t.ctab = rt::heap_alloc(bytes);
                std::memcpy(rt::host_ptr(t.ctab), rt::host_ptr(body), bytes);
                t.ctab_size = bytes;
                const uint32_t b = t.ctab;
                t.creator = b + rd32(b + 4);
                t.version = rd32(b + 8);
                const uint32_t n = rd32(b + 12), info = b + rd32(b + 16);
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t ci = info + 20 * i;
                    Constant c;
                    c.guest_name = b + rd32(ci);
                    c.name = rt::host_str(c.guest_name);
                    c.set = rd16(ci + 4);
                    c.reg = rd16(ci + 6);
                    c.count = rd16(ci + 8);
                    const uint32_t ti = b + rd32(ci + 12);
                    c.cls = rd16(ti);
                    c.type = rd16(ti + 2);
                    c.rows = rd16(ti + 4);
                    c.cols = rd16(ti + 6);
                    c.elements = rd16(ti + 8);
                    c.members = rd16(ti + 10);
                    c.default_value = rd32(ci + 16) ? b + rd32(ci + 16) : 0;
                    t.constants.push_back(std::move(c));
                }
                return true;
            }
            p += 4 + size * 4;
            continue;
        }
        // Only comments precede the first instruction that matters here.
        if ((tok & 0xFFFF) != 0xFFFE) break;
    }
    return false;
}

Table* table_of(uint32_t obj) {
    auto it = g_tables.find(obj);
    return it == g_tables.end() ? nullptr : &it->second;
}

// Uploads float registers for a constant (FLOAT4 set) or converts for INT/BOOL.
void upload(const Table& t, uint32_t device, const Constant& c, uint32_t first_reg_offset,
            const std::vector<float>& regs /* 4 floats per register */) {
    auto* dev = static_cast<IDirect3DDevice9*>(d3d9_host(device));
    if (!dev || regs.empty()) return;
    const uint32_t nregs = uint32_t(regs.size() / 4);
    if (first_reg_offset >= c.count) return;
    const uint32_t n = std::min<uint32_t>(nregs, c.count - first_reg_offset);
    const uint32_t reg = c.reg + first_reg_offset;
    switch (c.set) {
        case RS_FLOAT4:
            if (t.pixel) dev->SetPixelShaderConstantF(reg, regs.data(), n);
            else dev->SetVertexShaderConstantF(reg, regs.data(), n);
            break;
        case RS_INT4: {
            std::vector<int> iv(regs.size());
            for (size_t i = 0; i < regs.size(); ++i) iv[i] = int(regs[i]);
            if (t.pixel) dev->SetPixelShaderConstantI(reg, iv.data(), n);
            else dev->SetVertexShaderConstantI(reg, iv.data(), n);
            break;
        }
        case RS_BOOL: {
            std::vector<BOOL> bv(n);
            for (uint32_t i = 0; i < n; ++i) bv[i] = regs[i * 4] != 0.0f;
            if (t.pixel) dev->SetPixelShaderConstantB(reg, bv.data(), n);
            else dev->SetVertexShaderConstantB(reg, bv.data(), n);
            break;
        }
        default: break;
    }
}

// Packs `count` values of the constant's own shape from a flat float array
// (row-major per element), following D3DX's register layout rules.
std::vector<float> pack(const Constant& c, const float* data, uint32_t nfloats) {
    std::vector<float> regs;
    const uint32_t per = std::max<uint32_t>(1, c.rows) * std::max<uint32_t>(1, c.cols);
    const uint32_t elements = std::max<uint32_t>(1, nfloats / per);
    for (uint32_t e = 0; e < elements; ++e) {
        const float* m = data + e * per;
        if (c.cls == C_MATRIX_ROWS) {
            for (uint32_t r = 0; r < c.rows; ++r) {
                float v[4] = {};
                for (uint32_t k = 0; k < c.cols && k < 4; ++k) v[k] = m[r * c.cols + k];
                regs.insert(regs.end(), v, v + 4);
            }
        } else if (c.cls == C_MATRIX_COLUMNS) {
            for (uint32_t k = 0; k < c.cols; ++k) {
                float v[4] = {};
                for (uint32_t r = 0; r < c.rows && r < 4; ++r) v[r] = m[r * c.cols + k];
                regs.insert(regs.end(), v, v + 4);
            }
        } else {
            float v[4] = {};
            for (uint32_t k = 0; k < per && k < 4; ++k) v[k] = m[k];
            regs.insert(regs.end(), v, v + 4);
        }
    }
    return regs;
}

// Matrices arrive as D3DXMATRIX (4x4 row-major); reduce to rows x cols.
std::vector<float> matrices(const Constant& c, const float* m4, uint32_t count, bool transpose) {
    std::vector<float> flat;
    for (uint32_t e = 0; e < count; ++e) {
        const float* m = m4 + 16 * e;
        for (uint32_t r = 0; r < std::max<uint32_t>(1, c.rows); ++r)
            for (uint32_t k = 0; k < std::max<uint32_t>(1, c.cols); ++k)
                flat.push_back(transpose ? m[k * 4 + r] : m[r * 4 + k]);
    }
    return flat;
}

uint32_t set_floats(const Args& a, const std::vector<float>& flat) {
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t) return D3DERR_INVALIDCALL;
    uint32_t elem = 0;
    const Constant* c = resolve(*t, a[2], &elem);
    if (!c) return D3DERR_INVALIDCALL;
    const uint32_t offset = elem ? (elem - 1) * regs_per_element(*c) : 0;
    upload(*t, a[1], *c, offset, pack(*c, flat.data(), uint32_t(flat.size())));
    return D3D_OK;
}

const float* fptr(uint32_t g) { return reinterpret_cast<const float*>(rt::host_ptr(g)); }
float bits_to_float(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

uint32_t ct_buffer_pointer(X86*, const Args& a) {
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    return t ? t->ctab : 0;
}
uint32_t ct_buffer_size(X86*, const Args& a) {
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    return t ? t->ctab_size : 0;
}
uint32_t ct_get_desc(X86*, const Args& a) {  // (D3DXCONSTANTTABLE_DESC*)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t || !a[1]) return D3DERR_INVALIDCALL;
    wr32(a[1], t->creator);
    wr32(a[1] + 4, t->version);
    wr32(a[1] + 8, uint32_t(t->constants.size()));
    return D3D_OK;
}
uint32_t ct_get_constant_desc(X86*, const Args& a) {  // (handle, D3DXCONSTANT_DESC*, UINT* count)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t) return D3DERR_INVALIDCALL;
    uint32_t elem = 0;
    const Constant* c = resolve(*t, a[1], &elem);
    if (!c) return D3DERR_INVALIDCALL;
    if (a[2]) {
        const uint32_t d = a[2];
        wr32(d + 0, c->guest_name);
        wr32(d + 4, c->set);
        wr32(d + 8, c->reg + (elem ? (elem - 1) * regs_per_element(*c) : 0));
        wr32(d + 12, elem ? regs_per_element(*c) : c->count);
        wr32(d + 16, c->cls);
        wr32(d + 20, c->type);
        wr32(d + 24, c->rows);
        wr32(d + 28, c->cols);
        wr32(d + 32, elem ? 1 : c->elements);
        wr32(d + 36, c->members);
        wr32(d + 40, uint32_t(c->rows) * c->cols * 4 * (elem ? 1 : std::max<uint32_t>(1, c->elements)));
        wr32(d + 44, c->default_value);
    }
    if (a[3]) wr32(a[3], 1);
    return D3D_OK;
}
uint32_t ct_get_sampler_index(X86*, const Args& a) {
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    uint32_t elem = 0;
    const Constant* c = t ? resolve(*t, a[1], &elem) : nullptr;
    return c && c->set == RS_SAMPLER ? c->reg + (elem ? elem - 1 : 0) : 0xFFFFFFFF;
}
uint32_t ct_get_constant(X86*, const Args& a) {  // (parent, index)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t || a[1] || a[2] >= t->constants.size()) return 0;  // struct members are not supported
    return make_handle(a[2], 0);
}
uint32_t ct_get_constant_by_name(X86*, const Args& a) {  // (parent, name)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t || !a[2]) return 0;
    const char* name = rt::host_str(a[2]);
    for (uint32_t i = 0; i < t->constants.size(); ++i)
        if (t->constants[i].name == name || (name[0] == '$' && t->constants[i].name == name + 1))
            return make_handle(i, 0);
    return 0;
}
uint32_t ct_get_constant_element(X86*, const Args& a) {  // (handle, index)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t) return 0;
    uint32_t elem = 0;
    const Constant* c = resolve(*t, a[1], &elem);
    if (!c || a[2] >= std::max<uint32_t>(1, c->elements)) return 0;
    return make_handle(uint32_t(c - t->constants.data()), a[2] + 1);
}
uint32_t ct_set_defaults(X86*, const Args& a) {  // (device)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    if (!t) return D3DERR_INVALIDCALL;
    for (const Constant& c : t->constants) {
        if (!c.default_value || c.set == RS_SAMPLER) continue;
        std::vector<float> regs(size_t(c.count) * 4);
        std::memcpy(regs.data(), fptr(c.default_value), regs.size() * 4);
        upload(*t, a[1], c, 0, regs);
    }
    return D3D_OK;
}
uint32_t ct_set_value(X86*, const Args& a) {  // (device, handle, data, bytes)
    const float* d = fptr(a[3]);
    return set_floats(a, std::vector<float>(d, d + a[4] / 4));
}
uint32_t ct_set_bool(X86*, const Args& a) { return set_floats(a, {a[3] ? 1.0f : 0.0f}); }
uint32_t ct_set_int(X86*, const Args& a) { return set_floats(a, {float(int32_t(a[3]))}); }
uint32_t ct_set_float(X86*, const Args& a) { return set_floats(a, {bits_to_float(a[3])}); }
uint32_t ct_set_bool_array(X86*, const Args& a) {
    std::vector<float> v;
    for (uint32_t i = 0; i < a[4]; ++i) v.push_back(rd32(a[3] + 4 * i) ? 1.0f : 0.0f);
    return set_floats(a, v);
}
uint32_t ct_set_int_array(X86*, const Args& a) {
    std::vector<float> v;
    for (uint32_t i = 0; i < a[4]; ++i) v.push_back(float(int32_t(rd32(a[3] + 4 * i))));
    return set_floats(a, v);
}
uint32_t ct_set_float_array(X86*, const Args& a) {
    const float* d = fptr(a[3]);
    return set_floats(a, std::vector<float>(d, d + a[4]));
}
uint32_t ct_set_vector(X86*, const Args& a) {  // (device, handle, D3DXVECTOR4*)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    uint32_t elem = 0;
    const Constant* c = t ? resolve(*t, a[2], &elem) : nullptr;
    if (!c) return D3DERR_INVALIDCALL;
    const float* d = fptr(a[3]);
    std::vector<float> regs(d, d + 4);
    upload(*t, a[1], *c, elem ? elem - 1 : 0, regs);
    return D3D_OK;
}
uint32_t ct_set_vector_array(X86*, const Args& a) {  // (device, handle, D3DXVECTOR4*, count)
    std::lock_guard lk(g_mu);
    Table* t = table_of(a[0]);
    uint32_t elem = 0;
    const Constant* c = t ? resolve(*t, a[2], &elem) : nullptr;
    if (!c) return D3DERR_INVALIDCALL;
    const float* d = fptr(a[3]);
    upload(*t, a[1], *c, elem ? elem - 1 : 0, std::vector<float>(d, d + 4 * a[4]));
    return D3D_OK;
}
uint32_t set_matrices(const Args& a, uint32_t count, bool transpose, bool pointers) {
    Constant c;
    {
        std::lock_guard lk(g_mu);
        Table* t = table_of(a[0]);
        uint32_t elem = 0;
        const Constant* p = t ? resolve(*t, a[2], &elem) : nullptr;
        if (!p) return D3DERR_INVALIDCALL;
        c = *p;
    }
    std::vector<float> m4(16 * size_t(count));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t src = pointers ? rd32(a[3] + 4 * i) : a[3] + 64 * i;
        std::memcpy(&m4[16 * i], fptr(src), 64);
    }
    return set_floats(a, matrices(c, m4.data(), count, transpose));
}
uint32_t ct_set_matrix(X86*, const Args& a) { return set_matrices(a, 1, false, false); }
uint32_t ct_set_matrix_array(X86*, const Args& a) { return set_matrices(a, a[4], false, false); }
uint32_t ct_set_matrix_ptr_array(X86*, const Args& a) { return set_matrices(a, a[4], false, true); }
uint32_t ct_set_matrix_t(X86*, const Args& a) { return set_matrices(a, 1, true, false); }
uint32_t ct_set_matrix_t_array(X86*, const Args& a) { return set_matrices(a, a[4], true, false); }
uint32_t ct_set_matrix_t_ptr_array(X86*, const Args& a) { return set_matrices(a, a[4], true, true); }

uint32_t table_vtable() {
    static uint32_t vt = make_vtable({
        &COM_FN<2, com_qi_self>, &COM_FN<0, com_addref>, &COM_FN<0, com_release>,
        &COM_FN<0, ct_buffer_pointer>, &COM_FN<0, ct_buffer_size>,
        &COM_FN<1, ct_get_desc>, &COM_FN<3, ct_get_constant_desc>, &COM_FN<1, ct_get_sampler_index>,
        &COM_FN<2, ct_get_constant>, &COM_FN<2, ct_get_constant_by_name>, &COM_FN<2, ct_get_constant_element>,
        &COM_FN<1, ct_set_defaults>, &COM_FN<4, ct_set_value>, &COM_FN<3, ct_set_bool>, &COM_FN<4, ct_set_bool_array>,
        &COM_FN<3, ct_set_int>, &COM_FN<4, ct_set_int_array>, &COM_FN<3, ct_set_float>, &COM_FN<4, ct_set_float_array>,
        &COM_FN<3, ct_set_vector>, &COM_FN<4, ct_set_vector_array>, &COM_FN<3, ct_set_matrix>,
        &COM_FN<4, ct_set_matrix_array>, &COM_FN<4, ct_set_matrix_ptr_array>, &COM_FN<3, ct_set_matrix_t>,
        &COM_FN<4, ct_set_matrix_t_array>, &COM_FN<4, ct_set_matrix_t_ptr_array>,
    });
    return vt;
}

uint32_t new_constant_table(uint32_t function) {
    Table t;
    if (!parse_ctab(function, t)) return 0;
    const uint32_t o = new_object(table_vtable(), 0);
    std::lock_guard lk(g_mu);
    g_tables[o] = std::move(t);
    return o;
}

}  // namespace

// D3DXCompileShader(src, len, defines, include, function, profile, flags, **shader, **errors, **constants)
STDCALL(d3dx9_42, D3DXCompileShader, 10) {
    const char* fn = a.str(4);
    const char* profile = a.str(5);
    for (uint32_t i : {7u, 8u, 9u})
        if (a[i]) wr32(a[i], 0);
    for (const BuiltinShader& s : kBuiltinShaders) {
        if (!fn || !profile || std::strcmp(s.function, fn) || std::strcmp(s.profile, profile)) continue;
        const uint32_t buffer = new_buffer(s.words, s.count * 4);
        if (a[7]) wr32(a[7], buffer);
        if (a[9]) {
            uint32_t data;
            {
                std::lock_guard lk(g_mu);
                data = g_buffers[buffer].data;
            }
            wr32(a[9], new_constant_table(data));
        }
        return D3D_OK;
    }
    rt::log("D3DXCompileShader: no built-in shader for %s %s", profile ? profile : "?", fn ? fn : "?");
    if (a[8]) {
        const std::string msg = std::string("shader compilation is not available: ") + (fn ? fn : "?");
        wr32(a[8], new_buffer(msg.c_str(), uint32_t(msg.size() + 1)));
    }
    return E_FAIL;
}

STDCALL(d3dx9_42, D3DXGetShaderConstantTable, 2) {  // (const DWORD* function, ID3DXConstantTable**)
    if (!a[1]) return D3DERR_INVALIDCALL;
    const uint32_t t = a[0] ? new_constant_table(a[0]) : 0;
    wr32(a[1], t);
    return t ? D3D_OK : E_FAIL;
}

// D3DXGetShaderInputSemantics(function, D3DXSEMANTIC* out, UINT* count): vertex inputs from dcl tokens.
STDCALL(d3dx9_42, D3DXGetShaderInputSemantics, 3) {
    if (!a[0]) return D3DERR_INVALIDCALL;
    uint32_t n = 0;
    for (uint32_t p = a[0] + 4;;) {
        const uint32_t tok = rd32(p);
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) {
            p += 4 + (tok >> 16) * 4;
            continue;
        }
        const uint32_t len = (tok >> 24) & 0xF;
        if ((tok & 0xFFFF) == 0x1F) {  // dcl usage, register
            const uint32_t usage = rd32(p + 4), reg = rd32(p + 8);
            const uint32_t type = ((reg >> 28) & 7) | (((reg >> 11) & 3) << 3);
            if (type == 1) {  // input register
                if (a[1]) {
                    wr32(a[1] + 8 * n, usage & 0x1F);
                    wr32(a[1] + 8 * n + 4, (usage >> 16) & 0xF);
                }
                ++n;
            }
        }
        p += 4 + len * 4;
    }
    if (a[2]) wr32(a[2], n);
    return D3D_OK;
}

// D3DXFilterTexture(texture, palette, srcLevel, filter): box-filtered mips for 32-bit 2D textures.
STDCALL(d3dx9_42, D3DXFilterTexture, 4) {
    auto* base = static_cast<IDirect3DBaseTexture9*>(d3d9_host(a[0]));
    if (!base || base->GetType() != D3DRTYPE_TEXTURE) return D3D_OK;
    auto* tex = static_cast<IDirect3DTexture9*>(base);
    const uint32_t src_level = a[2] == 0xFFFFFFFF ? 0 : a[2];
    for (uint32_t level = src_level + 1; level < tex->GetLevelCount(); ++level) {
        D3DSURFACE_DESC sd{}, dd{};
        tex->GetLevelDesc(level - 1, &sd);
        tex->GetLevelDesc(level, &dd);
        if (sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8 && sd.Format != D3DFMT_A8B8G8R8) break;
        D3DLOCKED_RECT s{}, d{};
        if (FAILED(tex->LockRect(level - 1, &s, nullptr, D3DLOCK_READONLY))) break;
        if (FAILED(tex->LockRect(level, &d, nullptr, 0))) {
            tex->UnlockRect(level - 1);
            break;
        }
        for (uint32_t y = 0; y < dd.Height; ++y)
            for (uint32_t x = 0; x < dd.Width; ++x) {
                uint32_t acc[4] = {};
                for (uint32_t k = 0; k < 4; ++k) {
                    const uint32_t sx = std::min(sd.Width - 1, 2 * x + (k & 1)), sy = std::min(sd.Height - 1, 2 * y + (k >> 1));
                    const uint8_t* px = static_cast<const uint8_t*>(s.pBits) + sy * s.Pitch + sx * 4;
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += px[ch];
                }
                uint8_t* out = static_cast<uint8_t*>(d.pBits) + y * d.Pitch + x * 4;
                for (int ch = 0; ch < 4; ++ch) out[ch] = uint8_t((acc[ch] + 2) / 4);
            }
        tex->UnlockRect(level);
        tex->UnlockRect(level - 1);
    }
    return D3D_OK;
}

// D3DXFillTexture(texture, LPD3DXFILL2D fn, data): calls the guest function per texel (32-bit formats).
STDCALL(d3dx9_42, D3DXFillTexture, 3) {
    auto* tex = static_cast<IDirect3DTexture9*>(d3d9_host(a[0]));
    if (!tex) return D3DERR_INVALIDCALL;
    const uint32_t scratch = rt::heap_alloc(32);  // out vec4, texcoord vec2, texel size vec2
    for (uint32_t level = 0; level < tex->GetLevelCount(); ++level) {
        D3DSURFACE_DESC dd{};
        tex->GetLevelDesc(level, &dd);
        if (dd.Format != D3DFMT_A8R8G8B8 && dd.Format != D3DFMT_X8R8G8B8) continue;
        D3DLOCKED_RECT d{};
        if (FAILED(tex->LockRect(level, &d, nullptr, 0))) continue;
        const float tw = 1.0f / float(dd.Width), th = 1.0f / float(dd.Height);
        for (uint32_t y = 0; y < dd.Height; ++y)
            for (uint32_t x = 0; x < dd.Width; ++x) {
                const float uv[2] = {(float(x) + 0.5f) * tw, (float(y) + 0.5f) * th}, ts[2] = {tw, th};
                std::memcpy(rt::host_ptr(scratch + 16), uv, 8);
                std::memcpy(rt::host_ptr(scratch + 24), ts, 8);
                callback(c, a[1], {scratch, scratch + 16, scratch + 24, a[2]});
                float v[4];
                std::memcpy(v, rt::host_ptr(scratch), 16);
                auto q = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(d.pBits) + y * d.Pitch + x * 4) =
                    (q(v[3]) << 24) | (q(v[0]) << 16) | (q(v[1]) << 8) | q(v[2]);
            }
        tex->UnlockRect(level);
    }
    rt::heap_free(scratch);
    return D3D_OK;
}

STDCALL(d3dx9_42, D3DXSaveSurfaceToFileA, 5) {
    rt::log("D3DXSaveSurfaceToFileA(%s) ignored", a.str(0) ? a.str(0) : "");
    return D3D_OK;
}
