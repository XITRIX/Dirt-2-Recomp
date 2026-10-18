// d3dcompiler_42!D3DReflect: ID3D11ShaderReflection for the game's DX11 path.
//
// The reflector lives entirely in guest memory: D3DReflect parses the DXBC
// container once (RDEF: constant buffers, variables, types, resource
// bindings; ISGN/OSGN/PCSG: signatures; STAT: instruction counts; SHEX: the
// version) and lays every descriptor out in the 32-bit d3dcompiler_42 layout,
// with names as guest strings. The methods then copy descriptors or return
// guest pointers to the sub-objects (constant buffers, variables, types: not
// COM objects, plain vtables), so nothing here touches the host D3D11.
//
// Layouts are d3dcompiler_42's (IID 17F27486-A342-4D10-8842-AB0874E7F670, the
// August 2009 SDK): D3D11_SHADER_DESC has no barrier/interlocked/texture-store
// counts (35 dwords), the signature descriptor has no Stream, the variable
// descriptor no texture/sampler ranges and the type descriptor no Name.
#include <cstring>
#include <string>
#include <vector>

#include "../shim.h"

using namespace shim;

namespace {

constexpr uint32_t kSE_OK = 0, kE_FAIL = 0x80004005, kE_INVALIDARG = 0x80070057, kE_NOINTERFACE = 0x80004002;

struct Reader {
    const uint8_t* p;
    uint32_t n;
    uint32_t u32(uint32_t o) const { return o + 4 <= n ? uint32_t(p[o]) | p[o + 1] << 8 | p[o + 2] << 16 | uint32_t(p[o + 3]) << 24 : 0; }
    uint16_t u16(uint32_t o) const { return o + 2 <= n ? uint16_t(p[o] | p[o + 1] << 8) : 0; }
    std::string str(uint32_t o) const {
        std::string s;
        while (o < n && p[o]) s += char(p[o++]);
        return s;
    }
};

// Guest allocation helpers: everything a reflector hands out is freed with it.
struct Arena {
    std::vector<uint32_t> blocks;
    uint32_t alloc(uint32_t size) {
        const uint32_t g = rt::heap_alloc(size ? size : 4, true);
        blocks.push_back(g);
        return g;
    }
    uint32_t string(const std::string& s) {
        const uint32_t g = alloc(uint32_t(s.size() + 1));
        std::memcpy(rt::host_ptr(g), s.data(), s.size());
        return g;
    }
};

// Guest objects: [0] vtable, [4] data. A reflector's data is a Reflector*
// (host); sub-objects point at their guest descriptor block.
struct Reflector {
    Arena arena;
    uint32_t refs = 1;
    uint32_t desc = 0;            // D3D11_SHADER_DESC (35 dwords)
    std::vector<uint32_t> cbs;    // constant buffer objects
    std::vector<std::string> cb_names;
    std::vector<uint32_t> binds;  // D3D11_SHADER_INPUT_BIND_DESC (32 bytes each)
    std::vector<std::string> bind_names;
    std::vector<uint32_t> inputs, outputs, patch;  // signature descs (24 bytes)
    std::vector<std::pair<std::string, uint32_t>> vars;  // all variables by name
    uint32_t null_cb = 0, null_var = 0, null_type = 0;
    uint32_t mov = 0, movc = 0, conv = 0, bitwise = 0, gs_input = 0;
    bool sample_freq = false;
};

std::vector<Reflector*> g_live;  // index stored in the guest object

uint32_t vt_reflection = 0, vt_cb = 0, vt_var = 0, vt_type = 0;

Reflector* self_of(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    const uint32_t idx = obj ? rd32(obj + 4) : 0;
    return idx && idx <= g_live.size() ? g_live[idx - 1] : nullptr;
}

void ret(X86* c, uint32_t value, uint32_t nargs) {
    c->eax = value;
    c->esp += 4 * (2 + nargs);
}

uint32_t copy_desc(uint32_t dst, uint32_t src, uint32_t size) {
    if (!dst) return kE_INVALIDARG;
    if (!src) return kE_FAIL;
    std::memcpy(rt::host_ptr(dst), rt::host_ptr(src), size);
    return kSE_OK;
}

// ---- ID3D11ShaderReflectionType: [0] vtable, [4] desc (28 bytes), [8] members (n objs), [12] member names ----
void type_get_desc(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    ret(c, obj ? copy_desc(rd32(c->esp + 8), rd32(obj + 4), 28) : kE_FAIL, 1);
}
void type_member_by_index(X86* c) {
    const uint32_t obj = rd32(c->esp + 4), i = rd32(c->esp + 8);
    const uint32_t desc = obj ? rd32(obj + 4) : 0;
    const uint32_t n = desc ? rd32(desc + 20) : 0;  // Members
    ret(c, desc && i < n ? rd32(rd32(obj + 8) + 4 * i) : 0, 1);
}
void type_member_by_name(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    const std::string name = rt::host_str(rd32(c->esp + 8));
    const uint32_t desc = obj ? rd32(obj + 4) : 0;
    const uint32_t n = desc ? rd32(desc + 20) : 0;
    uint32_t r = 0;
    for (uint32_t i = 0; i < n && !r; ++i)
        if (name == rt::host_str(rd32(rd32(obj + 12) + 4 * i))) r = rd32(rd32(obj + 8) + 4 * i);
    ret(c, r, 1);
}
void type_member_name(X86* c) {
    const uint32_t obj = rd32(c->esp + 4), i = rd32(c->esp + 8);
    const uint32_t desc = obj ? rd32(obj + 4) : 0;
    ret(c, desc && i < rd32(desc + 20) ? rd32(rd32(obj + 12) + 4 * i) : 0, 1);
}
void type_is_equal(X86* c) { ret(c, rd32(c->esp + 4) == rd32(c->esp + 8) ? 0 : 1, 1); }  // S_OK / S_FALSE

// ---- ID3D11ShaderReflectionVariable: [0] vtable, [4] desc (20 bytes), [8] type obj, [12] buffer obj ----
void var_get_desc(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    ret(c, obj ? copy_desc(rd32(c->esp + 8), rd32(obj + 4), 20) : kE_FAIL, 1);
}
void var_get_type(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    ret(c, obj ? rd32(obj + 8) : 0, 0);
}
void var_get_buffer(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    ret(c, obj ? rd32(obj + 12) : 0, 0);
}

// ---- ID3D11ShaderReflectionConstantBuffer: [0] vtable, [4] desc (20 bytes), [8] vars, [12] var names ----
void cb_get_desc(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    ret(c, obj ? copy_desc(rd32(c->esp + 8), rd32(obj + 4), 20) : kE_FAIL, 1);
}
void cb_var_by_index(X86* c) {
    const uint32_t obj = rd32(c->esp + 4), i = rd32(c->esp + 8);
    const uint32_t desc = obj ? rd32(obj + 4) : 0;
    ret(c, desc && i < rd32(desc + 8) ? rd32(rd32(obj + 8) + 4 * i) : rd32(obj + 16), 1);
}
void cb_var_by_name(X86* c) {
    const uint32_t obj = rd32(c->esp + 4);
    const std::string name = rt::host_str(rd32(c->esp + 8));
    const uint32_t desc = obj ? rd32(obj + 4) : 0;
    const uint32_t n = desc ? rd32(desc + 8) : 0;
    uint32_t v = rd32(obj + 16);  // the null variable
    for (uint32_t i = 0; i < n; ++i)
        if (name == rt::host_str(rd32(rd32(obj + 12) + 4 * i))) {
            v = rd32(rd32(obj + 8) + 4 * i);
            break;
        }
    ret(c, v, 1);
}

// ---- ID3D11ShaderReflection (d3dcompiler_42) ----
void refl_qi(X86* c) {
    const uint32_t obj = rd32(c->esp + 4), out = rd32(c->esp + 12);
    if (Reflector* r = self_of(c)) ++r->refs;
    if (out) wr32(out, obj);
    ret(c, kSE_OK, 2);
}
void refl_addref(X86* c) {
    Reflector* r = self_of(c);
    ret(c, r ? ++r->refs : 0, 0);
}
void refl_release(X86* c) {
    Reflector* r = self_of(c);
    uint32_t n = 0;
    if (r && (n = --r->refs) == 0) {
        const uint32_t obj = rd32(c->esp + 4);
        for (uint32_t b : r->arena.blocks) rt::heap_free(b);
        g_live[rd32(obj + 4) - 1] = nullptr;
        delete r;
        rt::heap_free(obj);
    }
    ret(c, n, 0);
}
void refl_get_desc(X86* c) {
    Reflector* r = self_of(c);
    ret(c, r ? copy_desc(rd32(c->esp + 8), r->desc, 35 * 4) : kE_FAIL, 1);
}
void refl_cb_by_index(X86* c) {
    Reflector* r = self_of(c);
    const uint32_t i = rd32(c->esp + 8);
    ret(c, r ? (i < r->cbs.size() ? r->cbs[i] : r->null_cb) : 0, 1);
}
void refl_cb_by_name(X86* c) {
    Reflector* r = self_of(c);
    const std::string name = rt::host_str(rd32(c->esp + 8));
    uint32_t v = r ? r->null_cb : 0;
    if (r)
        for (size_t i = 0; i < r->cbs.size(); ++i)
            if (r->cb_names[i] == name) v = r->cbs[i];
    ret(c, v, 1);
}
void refl_binding(X86* c) {
    Reflector* r = self_of(c);
    const uint32_t i = rd32(c->esp + 8);
    ret(c, r && i < r->binds.size() ? copy_desc(rd32(c->esp + 12), r->binds[i], 32) : kE_INVALIDARG, 2);
}
uint32_t sig_desc(const std::vector<uint32_t>& v, uint32_t i, uint32_t dst) {
    return i < v.size() ? copy_desc(dst, v[i], 24) : kE_INVALIDARG;
}
void refl_input(X86* c) {
    Reflector* r = self_of(c);
    ret(c, r ? sig_desc(r->inputs, rd32(c->esp + 8), rd32(c->esp + 12)) : kE_FAIL, 2);
}
void refl_output(X86* c) {
    Reflector* r = self_of(c);
    ret(c, r ? sig_desc(r->outputs, rd32(c->esp + 8), rd32(c->esp + 12)) : kE_FAIL, 2);
}
void refl_patch(X86* c) {
    Reflector* r = self_of(c);
    ret(c, r ? sig_desc(r->patch, rd32(c->esp + 8), rd32(c->esp + 12)) : kE_FAIL, 2);
}
void refl_var_by_name(X86* c) {
    Reflector* r = self_of(c);
    const std::string name = rt::host_str(rd32(c->esp + 8));
    uint32_t v = r ? r->null_var : 0;
    if (r)
        for (auto& [n, obj] : r->vars)
            if (n == name) v = obj;
    ret(c, v, 1);
}
void refl_binding_by_name(X86* c) {
    Reflector* r = self_of(c);
    const std::string name = rt::host_str(rd32(c->esp + 8));
    uint32_t hr = kE_INVALIDARG;
    if (r)
        for (size_t i = 0; i < r->binds.size(); ++i)
            if (r->bind_names[i] == name) hr = copy_desc(rd32(c->esp + 12), r->binds[i], 32);
    ret(c, hr, 2);
}
void refl_mov(X86* c) { Reflector* r = self_of(c); ret(c, r ? r->mov : 0, 0); }
void refl_movc(X86* c) { Reflector* r = self_of(c); ret(c, r ? r->movc : 0, 0); }
void refl_conv(X86* c) { Reflector* r = self_of(c); ret(c, r ? r->conv : 0, 0); }
void refl_bitwise(X86* c) { Reflector* r = self_of(c); ret(c, r ? r->bitwise : 0, 0); }
void refl_gs_input(X86* c) { Reflector* r = self_of(c); ret(c, r ? r->gs_input : 0, 0); }
void refl_is_level9(X86* c) { ret(c, 0, 0); }
void refl_sample_freq(X86* c) { Reflector* r = self_of(c); ret(c, r && r->sample_freq, 0); }

uint32_t make_vtable(std::initializer_list<recomp_fn> fns) {
    const uint32_t vt = rt::heap_alloc(uint32_t(fns.size() * 4));
    uint32_t i = 0;
    for (recomp_fn f : fns) wr32(vt + 4 * i++, rt::alloc_thunk(f));
    return vt;
}

void init_vtables() {
    if (vt_reflection) return;
    // d3dcompiler_42 order. Slots after GetOutputParameterDesc are as in the
    // August 2009 header; the game has been seen to use only 3, 4, 6 and 7.
    vt_reflection = make_vtable({refl_qi, refl_addref, refl_release, refl_get_desc, refl_cb_by_index, refl_cb_by_name,
                                 refl_binding, refl_input, refl_output, refl_patch, refl_var_by_name,
                                 refl_binding_by_name, refl_mov, refl_movc, refl_conv, refl_bitwise, refl_gs_input,
                                 refl_is_level9, refl_sample_freq});
    vt_cb = make_vtable({cb_get_desc, cb_var_by_index, cb_var_by_name});
    vt_var = make_vtable({var_get_desc, var_get_type, var_get_buffer});
    vt_type = make_vtable({type_get_desc, type_member_by_index, type_member_by_name, type_member_name, type_is_equal});
}

// ---- DXBC parsing ----

struct Chunk {
    Reader r;
    bool ok = false;
};
Chunk find_chunk(const Reader& dxbc, const char* tag) {
    const uint32_t count = dxbc.u32(28);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = dxbc.u32(32 + 4 * i);
        if (off + 8 <= dxbc.n && std::memcmp(dxbc.p + off, tag, 4) == 0) {
            const uint32_t size = dxbc.u32(off + 4);
            return Chunk{Reader{dxbc.p + off + 8, std::min(size, dxbc.n - off - 8)}, true};
        }
    }
    return Chunk{};
}

// A type and its members, recursively (RDEF type offset -> guest type object).
uint32_t build_type(Reflector& R, const Reader& rd, uint32_t off, bool sm5, int depth = 0) {
    const uint32_t obj = R.arena.alloc(16), desc = R.arena.alloc(28);
    wr32(obj, vt_type);
    wr32(obj + 4, desc);
    const uint32_t members = rd.u16(off + 10), member_off = rd.u32(off + 12);
    wr32(desc + 0, rd.u16(off + 0));  // Class
    wr32(desc + 4, rd.u16(off + 2));  // Type
    wr32(desc + 8, rd.u16(off + 4));  // Rows
    wr32(desc + 12, rd.u16(off + 6)); // Columns
    wr32(desc + 16, rd.u16(off + 8)); // Elements
    wr32(desc + 20, members);
    wr32(desc + 24, 0);               // Offset (set by the parent for members)
    if (members && depth < 8) {
        const uint32_t objs = R.arena.alloc(4 * members), names = R.arena.alloc(4 * members);
        wr32(obj + 8, objs);
        wr32(obj + 12, names);
        for (uint32_t m = 0; m < members; ++m) {
            const uint32_t e = member_off + 12 * m;
            const uint32_t t = build_type(R, rd, rd.u32(e + 4), sm5, depth + 1);
            wr32(rd32(t + 4) + 24, rd.u32(e + 8));
            wr32(objs + 4 * m, t);
            wr32(names + 4 * m, R.arena.string(rd.str(rd.u32(e))));
        }
    }
    return obj;
}

void parse_rdef(Reflector& R, const Reader& rd, uint32_t desc) {
    const uint32_t cb_count = rd.u32(0), cb_off = rd.u32(4), bind_count = rd.u32(8), bind_off = rd.u32(12);
    const bool sm5 = rd.n > 17 && rd.p[17] >= 5;  // RDEF target version: minor, major, program type
    wr32(desc + 4, R.arena.string(rd.str(rd.u32(24))));  // Creator
    wr32(desc + 8, rd.u32(20));                          // Flags
    wr32(desc + 12, cb_count);
    wr32(desc + 16, bind_count);
    for (uint32_t i = 0; i < bind_count; ++i) {
        const uint32_t e = bind_off + 32 * i, g = R.arena.alloc(32);
        const std::string name = rd.str(rd.u32(e));
        wr32(g + 0, R.arena.string(name));
        wr32(g + 4, rd.u32(e + 4));    // Type
        wr32(g + 8, rd.u32(e + 20));   // BindPoint
        wr32(g + 12, rd.u32(e + 24));  // BindCount
        wr32(g + 16, rd.u32(e + 28));  // uFlags
        wr32(g + 20, rd.u32(e + 8));   // ReturnType
        wr32(g + 24, rd.u32(e + 12));  // Dimension
        wr32(g + 28, rd.u32(e + 16));  // NumSamples
        R.binds.push_back(g);
        R.bind_names.push_back(name);
    }
    const uint32_t var_size = sm5 ? 40 : 24;
    for (uint32_t i = 0; i < cb_count; ++i) {
        const uint32_t e = cb_off + 24 * i;
        const uint32_t nvars = rd.u32(e + 4), voff = rd.u32(e + 8);
        const std::string name = rd.str(rd.u32(e));
        const uint32_t obj = R.arena.alloc(20), d = R.arena.alloc(20);
        wr32(obj, vt_cb);
        wr32(obj + 4, d);
        wr32(obj + 16, R.null_var);
        wr32(d + 0, R.arena.string(name));
        wr32(d + 4, rd.u32(e + 20));  // Type
        wr32(d + 8, nvars);           // Variables
        wr32(d + 12, rd.u32(e + 12)); // Size
        wr32(d + 16, rd.u32(e + 16)); // uFlags
        const uint32_t objs = R.arena.alloc(4 * nvars), names = R.arena.alloc(4 * nvars);
        wr32(obj + 8, objs);
        wr32(obj + 12, names);
        for (uint32_t v = 0; v < nvars; ++v) {
            const uint32_t ve = voff + var_size * v;
            const std::string vname = rd.str(rd.u32(ve));
            const uint32_t vobj = R.arena.alloc(16), vd = R.arena.alloc(20);
            wr32(vobj, vt_var);
            wr32(vobj + 4, vd);
            wr32(vobj + 8, build_type(R, rd, rd.u32(ve + 16), sm5));
            wr32(vobj + 12, obj);
            const uint32_t gname = R.arena.string(vname);
            wr32(vd + 0, gname);
            wr32(vd + 4, rd.u32(ve + 4));   // StartOffset
            wr32(vd + 8, rd.u32(ve + 8));   // Size
            wr32(vd + 12, rd.u32(ve + 12)); // uFlags
            const uint32_t def = rd.u32(ve + 20), size = rd.u32(ve + 8);
            if (def && def + size <= rd.n) {
                const uint32_t g = R.arena.alloc(size);
                std::memcpy(rt::host_ptr(g), rd.p + def, size);
                wr32(vd + 16, g);           // DefaultValue
            }
            wr32(objs + 4 * v, vobj);
            wr32(names + 4 * v, gname);
            R.vars.emplace_back(vname, vobj);
        }
        R.cbs.push_back(obj);
        R.cb_names.push_back(name);
    }
}

// ISGN/OSGN/PCSG (24-byte entries) and the SM5 ISG1/OSG5/OSG1 variants (a
// leading stream dword, 28 or 32 bytes); the guest gets the 42 layout.
void parse_signature(Reflector& R, const Reader& rd, std::vector<uint32_t>& out, uint32_t entry_size, uint32_t lead) {
    const uint32_t count = rd.u32(0), base = rd.u32(4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = base + entry_size * i + lead, g = R.arena.alloc(24);
        wr32(g + 0, R.arena.string(rd.str(rd.u32(e))));  // SemanticName
        wr32(g + 4, rd.u32(e + 4));                       // SemanticIndex
        wr32(g + 8, rd.u32(e + 16));                      // Register
        wr32(g + 12, rd.u32(e + 8));                      // SystemValueType
        wr32(g + 16, rd.u32(e + 12));                     // ComponentType
        const uint32_t masks = rd.u32(e + 20);
        wr8(g + 20, uint8_t(masks));                      // Mask
        wr8(g + 21, uint8_t(masks >> 8));                 // ReadWriteMask
        out.push_back(g);
    }
}

}  // namespace

// D3DReflect(code, size, riid, **reflector)
STDCALL(d3dcompiler_42, D3DReflect, 4) {
    init_vtables();
    if (a[3]) wr32(a[3], 0);
    if (!a[0] || a[1] < 32 || !a[3]) return kE_INVALIDARG;
    const Reader dxbc{rt::host_ptr(a[0]), a[1]};
    if (std::memcmp(dxbc.p, "DXBC", 4) != 0) return kE_INVALIDARG;
    auto* R = new Reflector;
    R->null_var = R->arena.alloc(16);
    wr32(R->null_var, vt_var);  // desc 0: GetDesc fails, as on Windows
    R->null_cb = R->arena.alloc(20);
    wr32(R->null_cb, vt_cb);
    wr32(R->null_cb + 16, R->null_var);
    R->desc = R->arena.alloc(35 * 4);
    if (Chunk shex = find_chunk(dxbc, "SHEX"); shex.ok) {
        const uint32_t v = shex.r.u32(0);  // [minor:4][major:4]...[type:16]
        wr32(R->desc, (v >> 16) << 16 | (v & 0xFF));
    } else if (Chunk shdr = find_chunk(dxbc, "SHDR"); shdr.ok) {
        const uint32_t v = shdr.r.u32(0);
        wr32(R->desc, (v >> 16) << 16 | (v & 0xFF));
    }
    if (Chunk rdef = find_chunk(dxbc, "RDEF"); rdef.ok) parse_rdef(*R, rdef.r, R->desc);
    if (Chunk c = find_chunk(dxbc, "ISGN"); c.ok) parse_signature(*R, c.r, R->inputs, 24, 0);
    else if (Chunk c1 = find_chunk(dxbc, "ISG1"); c1.ok) parse_signature(*R, c1.r, R->inputs, 32, 4);
    if (Chunk c = find_chunk(dxbc, "OSGN"); c.ok) parse_signature(*R, c.r, R->outputs, 24, 0);
    else if (Chunk c5 = find_chunk(dxbc, "OSG5"); c5.ok) parse_signature(*R, c5.r, R->outputs, 28, 4);
    else if (Chunk c1 = find_chunk(dxbc, "OSG1"); c1.ok) parse_signature(*R, c1.r, R->outputs, 32, 4);
    if (Chunk c = find_chunk(dxbc, "PCSG"); c.ok) parse_signature(*R, c.r, R->patch, 24, 0);
    wr32(R->desc + 20, uint32_t(R->inputs.size()));
    wr32(R->desc + 24, uint32_t(R->outputs.size()));
    wr32(R->desc + 29 * 4, uint32_t(R->patch.size()));  // PatchConstantParameters
    if (Chunk stat = find_chunk(dxbc, "STAT"); stat.ok) {
        // STAT dword order (as Wine's d3dcompiler reads it) -> D3D11_SHADER_DESC fields.
        const Reader& s = stat.r;
        const int to_desc[] = {7, 8, 10, 11, 17, 18, 19, 20, 21, 22, 9, 23, 24, 25, 12, 13, 14, 15, 16};
        for (int i = 0; i < 19; ++i) wr32(R->desc + 4 * to_desc[i], s.u32(4 * i));
        R->mov = s.u32(4 * 19);
        R->conv = s.u32(4 * 21);
        R->gs_input = s.u32(4 * 23);
        wr32(R->desc + 28 * 4, R->gs_input);       // InputPrimitive
        wr32(R->desc + 26 * 4, s.u32(4 * 24));     // GSOutputTopology
        wr32(R->desc + 27 * 4, s.u32(4 * 25));     // GSMaxOutputVertexCount
    }
    g_live.push_back(R);
    const uint32_t obj = rt::heap_alloc(8, true);
    wr32(obj, vt_reflection);
    wr32(obj + 4, uint32_t(g_live.size()));
    wr32(a[3], obj);
    return kSE_OK;
}
