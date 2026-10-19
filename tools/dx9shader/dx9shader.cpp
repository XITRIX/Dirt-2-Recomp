// dx9shader: translates Direct3D 9 shader bytecode (d3dbc, SM1-3) to GLSL 4.60
// for deko3d's uam compiler, plus a JSON description of the shader's interface
// for the D3D9-on-deko3d device (docs/SWITCH.md).
//
//   d3dbc --MojoShader--> SPIR-V --locations patched--> SPIRV-Cross --> GLSL 4.60
//
// D3D9 links vertex outputs to pixel inputs by semantic (TEXCOORD3, COLOR0),
// which MojoShader resolves by patching each vertex/pixel pair at link time. Here
// every semantic has one fixed location instead (location_for), so each shader
// translates on its own, offline. Vertex inputs keep MojoShader's location: the
// input register number (v0..v15); the JSON lists each one's usage and index,
// which the device matches against the vertex declaration.
//
// Bindings: constants are one std140 uniform block per stage at binding 0 (float4
// array, then int4, then bool arrays, as MojoShader lays them out; offsets in the
// JSON); sampler sN is binding N in its stage.
//
// Usage: dx9shader --out DIR file.d3dbc...   writes DIR/<name>.{vert,frag}.glsl and
// DIR/<name>.json, or DIR/<name>.err with the reason it failed.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#define __MOJOSHADER_INTERNAL__ 1  // for the patch table layout
#include "mojoshader.h"
#include "mojoshader_internal.h"  // SpirvPatchTable, appended to MojoShader's SPIR-V output
}
#include "spirv_glsl.hpp"

namespace {

// Location of a vertex output / pixel input semantic: one fixed table for every
// shader. Maxwell has 32 generic varyings. -1: not representable.
int location_for(int usage, int index) {
    switch (usage) {
        case MOJOSHADER_USAGE_TEXCOORD: return index < 16 ? index : -1;          // 0..15
        case MOJOSHADER_USAGE_COLOR: return index < 4 ? 16 + index : -1;         // 16..19
        case MOJOSHADER_USAGE_NORMAL: return index < 2 ? 20 + index : -1;        // 20..21
        case MOJOSHADER_USAGE_TANGENT: return index == 0 ? 22 : -1;
        case MOJOSHADER_USAGE_BINORMAL: return index == 0 ? 23 : -1;
        case MOJOSHADER_USAGE_FOG: return index == 0 ? 24 : -1;
        case MOJOSHADER_USAGE_BLENDWEIGHT: return index == 0 ? 25 : -1;
        case MOJOSHADER_USAGE_BLENDINDICES: return index == 0 ? 26 : -1;
        case MOJOSHADER_USAGE_POSITION: return index >= 1 && index <= 4 ? 26 + index : -1;  // 27..30
        case MOJOSHADER_USAGE_SAMPLE: return index == 0 ? 31 : -1;
        default: return -1;
    }
}

const char* usage_name(int u) {
    static const char* names[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                                  "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
    return u >= 0 && u < int(sizeof names / sizeof *names) ? names[u] : "UNKNOWN";
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// Vertex shader outputs start at zero: D3D9 hardware (and the translators
// that run these games) read unwritten output components as 0, while an
// uninitialised GLSL output carries leftover register contents on Maxwell:
// the game's depth pass wrote a TEXCOORD's z that its vertex shader never set.
std::string zero_outputs(const std::string& glsl) {
    std::string init;
    size_t pos = 0;
    while ((pos = glsl.find(") out vec4 ", pos)) != std::string::npos) {
        const size_t name = pos + 11, end = glsl.find(';', name);
        init += "    " + glsl.substr(name, end - name) + " = vec4(0.0);\n";
        pos = end;
    }
    const size_t body = glsl.find("void main()\n{\n");
    if (init.empty() || body == std::string::npos) return glsl;
    std::string out = glsl;
    out.insert(body + 14, init);
    return out;
}

struct Result {
    std::string glsl, json, error;
    bool vertex = false;
};

Result translate(const std::vector<unsigned char>& code) {
    Result r;
    const MOJOSHADER_parseData* pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_SPIRV, "main", code.data(),
                                                      unsigned(code.size()), nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
    if (pd->error_count > 0) {
        r.error = std::string("mojoshader: ") + (pd->errors[0].error ? pd->errors[0].error : "?");
        MOJOSHADER_freeParseData(pd);
        return r;
    }
    r.vertex = pd->shader_type == MOJOSHADER_TYPE_VERTEX;
    const size_t body = size_t(pd->output_len) - sizeof(SpirvPatchTable);
    SpirvPatchTable table;
    std::memcpy(&table, pd->output + body, sizeof table);
    std::vector<uint32_t> words(body / 4);
    std::memcpy(words.data(), pd->output, body);

    // Vertex outputs (vertex shader) / inputs (pixel shader): the fixed table.
    // MojoShader marks a PSIZE output it made a builtin with offset 1.
    for (int u = 0; u < MOJOSHADER_USAGE_TOTAL && r.error.empty(); ++u)
        for (int i = 0; i < 16; ++i) {
            const uint32_t off = table.attrib_offsets[u][i];
            if (off <= 1) continue;
            const int loc = location_for(u, i);
            if (loc < 0) {
                r.error = std::string("no location for ") + usage_name(u) + std::to_string(i);
                break;
            }
            words[off] = uint32_t(loc);
        }
    // MojoShader's link step also resolves the pixel shader's TEXCOORD0, which can
    // become gl_PointCoord; without point sprites it stays a vec4 input.
    if (!r.vertex && r.error.empty() && table.attrib_offsets[MOJOSHADER_USAGE_TEXCOORD][0] > 1) {
        const uint32_t t0 = table.attrib_offsets[MOJOSHADER_USAGE_TEXCOORD][0];
        if (table.pointcoord_var_offset) words[table.pointcoord_var_offset + 1] = table.tid_pvec4i;
        if (table.pointcoord_load_offset) words[table.pointcoord_load_offset + 1] = table.tid_vec4;
        words[t0 - 1] = spv::DecorationLocation;
        words[t0] = uint32_t(location_for(MOJOSHADER_USAGE_TEXCOORD, 0));
    }
    if (!r.error.empty()) {
        MOJOSHADER_freeParseData(pd);
        return r;
    }

    std::ostringstream js;
    try {
        spirv_cross::CompilerGLSL glsl(std::move(words));
        spirv_cross::CompilerGLSL::Options opt;
        opt.version = 460;
        opt.es = false;
        opt.vulkan_semantics = false;
        opt.separate_shader_objects = true;
        opt.enable_420pack_extension = true;
        glsl.set_common_options(opt);
        const spirv_cross::ShaderResources res = glsl.get_shader_resources();
        // deko3d has no descriptor sets: bindings are per stage.
        for (const auto& b : res.uniform_buffers) {
            glsl.unset_decoration(b.id, spv::DecorationDescriptorSet);
            glsl.set_decoration(b.id, spv::DecorationBinding, 0);
        }
        for (const auto& s : res.sampled_images) glsl.unset_decoration(s.id, spv::DecorationDescriptorSet);
        r.glsl = glsl.compile();
        if (r.vertex) r.glsl = zero_outputs(r.glsl);

        js << "{\n  \"stage\": \"" << (r.vertex ? "vertex" : "pixel") << "\", \"version\": \"" << pd->major_ver << "_"
           << pd->minor_ver << "\", \"instructions\": " << pd->instruction_count << ",\n";
        js << "  \"inputs\": [";
        for (int i = 0; i < pd->attribute_count; ++i) {
            const MOJOSHADER_attribute& a = pd->attributes[i];
            js << (i ? ", " : "") << "{\"usage\": \"" << usage_name(a.usage) << "\", \"index\": " << a.index
               << ", \"name\": " << json_str(a.name ? a.name : "") << "}";
        }
        js << "],\n  \"outputs\": [";
        for (int i = 0; i < pd->output_count; ++i) {
            const MOJOSHADER_attribute& a = pd->outputs[i];
            js << (i ? ", " : "") << "{\"usage\": \"" << usage_name(a.usage) << "\", \"index\": " << a.index << "}";
        }
        js << "],\n  \"samplers\": [";
        for (int i = 0; i < pd->sampler_count; ++i) {
            static const char* types[] = {"2d", "cube", "volume"};
            const MOJOSHADER_sampler& s = pd->samplers[i];
            js << (i ? ", " : "") << "{\"register\": " << s.index << ", \"type\": \""
               << (s.type >= 0 && s.type < 3 ? types[s.type] : "?") << "\"}";
        }
        js << "],\n  \"uniform_block\": [";
        bool first = true;
        for (const auto& b : res.uniform_buffers) {
            const auto& type = glsl.get_type(b.base_type_id);
            for (uint32_t m = 0; m < type.member_types.size(); ++m) {
                const auto& mt = glsl.get_type(type.member_types[m]);
                js << (first ? "" : ", ") << "{\"member\": " << json_str(glsl.get_member_name(b.base_type_id, m))
                   << ", \"offset\": " << glsl.type_struct_member_offset(type, m)
                   << ", \"count\": " << (mt.array.empty() ? 1 : mt.array[0]) << "}";
                first = false;
            }
        }
        js << "],\n  \"uniforms\": [";
        for (int i = 0; i < pd->uniform_count; ++i) {
            static const char* types[] = {"float4", "int4", "bool"};
            const MOJOSHADER_uniform& u = pd->uniforms[i];
            js << (i ? ", " : "") << "{\"type\": \"" << (u.type >= 0 && u.type < 3 ? types[u.type] : "?")
               << "\", \"register\": " << u.index << ", \"count\": " << u.array_count
               << ", \"constant\": " << (u.constant ? "true" : "false") << "}";
        }
        js << "],\n  \"constants\": " << pd->constant_count << "\n}\n";
        r.json = js.str();
    } catch (const std::exception& e) {
        r.error = std::string("spirv-cross: ") + e.what();
    }
    MOJOSHADER_freeParseData(pd);
    return r;
}

bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

void write_file(const std::string& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

std::string stem(const std::string& path) {
    const size_t s = path.find_last_of('/');
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    const size_t d = n.find_last_of('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = ".";
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s --out DIR file.d3dbc...\n", argv[0]);
        return 2;
    }
    int ok = 0, failed = 0;
    for (const std::string& f : files) {
        std::vector<unsigned char> code;
        const std::string base = out + "/" + stem(f);
        if (!read_file(f, code)) {
            write_file(base + ".err", "cannot read " + f + "\n");
            ++failed;
            continue;
        }
        const Result r = translate(code);
        if (!r.error.empty()) {
            write_file(base + ".err", r.error + "\n");
            ++failed;
            continue;
        }
        write_file(base + (r.vertex ? ".vert.glsl" : ".frag.glsl"), r.glsl);
        write_file(base + ".json", r.json);
        ++ok;
    }
    std::printf("dx9shader: %d translated, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}
