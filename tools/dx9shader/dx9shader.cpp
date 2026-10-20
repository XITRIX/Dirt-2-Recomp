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
#include <set>
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
        case MOJOSHADER_USAGE_FOG: return index <= 1 ? 24 : -1;
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

// Make unwritten vertex-output components deterministic. This does not fix
// the depth-pass corruption: its vertex shader does write the depth varying;
// the verified workaround is in dx9_shader_census.py's fragment-output patch.
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

std::string vertex_clipping(std::string glsl) {
    const size_t block=glsl.find("out gl_PerVertex");
    if(block==std::string::npos)return glsl;
    const size_t end=glsl.find("};",block);
    glsl.insert(end,"    float gl_ClipDistance[6];\n");
    const size_t main=glsl.find("void main()");
    if(main==std::string::npos)return glsl;
    glsl.replace(main,11,"void d3d9_original_main()");
    glsl.insert(main,"layout(std140,binding=6) uniform D3D9VertexState { vec4 d3d9ClipPlanes[6]; };\n");
    glsl += "\nvoid main(){ d3d9_original_main();\n";
    for(unsigned i=0;i<6;++i)glsl += "    gl_ClipDistance["+std::to_string(i)+"] = dot(gl_Position, d3d9ClipPlanes["+std::to_string(i)+"]);\n";
    return glsl+"}\n";
}

// D3D9 alpha testing and pre-SM3 fog run after the programmable pixel shader.
// A wrapper is required: an early return from the original main still executes
// the fixed pipeline operations. Binding 5 is independent of shader constants.
std::string pixel_epilogue(std::string glsl, const std::string& output, unsigned major) {
    const size_t main = glsl.find("void main()");
    if (main == std::string::npos || output.empty()) return glsl;
    glsl.replace(main, 11, "void d3d9_original_main()");
    const size_t declarations = glsl.find("\n\n") + 2;
    std::string uniforms = R"(
layout(std140, binding = 5) uniform D3D9FragmentState {
    uvec4 d3d9Control; // compare operation, alpha reference, fog mode, fog enabled
    vec4 d3d9FogColor;
    vec4 d3d9FogParams; // start, end, density, packed legacy sampler dimensions
};
)";
    if (major < 3 && glsl.find("layout(location = 24) in ") == std::string::npos)
        uniforms += "layout(location = 24) in float d3d9VertexFog;\n";
    glsl.insert(declarations, uniforms);
    glsl += "\nvoid main() {\n    d3d9_original_main();\n";
    if (major == 1) glsl += "    " + output + " = clamp(" + output + ", 0.0, 1.0);\n";
    glsl += "    float a = roundEven("+output+".a * 65535.0);\n";
    glsl += R"(    float ref = float(d3d9Control.y * 257u);
    uint op = d3d9Control.x;
    bool pass = op == 8u || (op == 2u && a < ref) || (op == 3u && a == ref)
        || (op == 4u && a <= ref) || (op == 5u && a > ref)
        || (op == 6u && a != ref) || (op == 7u && a >= ref);
    if (!pass) discard;
)";
    if (major < 3) {
        // Use an existing fog input when the shader already declares one.
        std::string fog = "d3d9VertexFog";
        const size_t loc = glsl.find("layout(location = 24) in ");
        if (loc != std::string::npos) {
            const size_t type = loc + std::strlen("layout(location = 24) in ");
            const size_t name = glsl.find(' ', type) + 1;
            fog = glsl.substr(name, glsl.find(';', name)-name);
        }
        glsl += "    if ((d3d9Control.w & 1u) != 0u) {\n        float f = " + fog + ";\n";
        glsl += R"(        float z = (d3d9Control.w & 2u) != 0u ? gl_FragCoord.z : 1.0 / gl_FragCoord.w;
        if (d3d9Control.z == 1u) f = exp(-d3d9FogParams.z * z);
        if (d3d9Control.z == 2u) f = exp(-pow(d3d9FogParams.z * z, 2.0));
        if (d3d9Control.z == 3u) f = (d3d9FogParams.y-z)/(d3d9FogParams.y-d3d9FogParams.x);
)";
        glsl += "        " + output + ".rgb = mix(d3d9FogColor.rgb, " + output + ".rgb, clamp(f, 0.0, 1.0));\n    }\n";
    }
    glsl += "    if ((d3d9Control.w & 16u) != 0u) {\n";
    glsl += "        vec3 c = clamp(" + output + ".rgb, 0.0, 1.0);\n";
    glsl += "        " + output + ".rgb = mix(1.055 * pow(c, vec3(1.0/2.4)) - 0.055, 12.92 * c, lessThanEqual(c, vec3(0.0031308)));\n    }\n";
    return glsl + "}\n";
}

// D3D9 switches ordinary 2D sampling to depth comparison according to the
// bound texture format. Keep the full bytecode coordinate (including z/q),
// which the normal GLSL texture expression discards for a color sampler.
class D3D9CompilerGLSL : public spirv_cross::CompilerGLSL {
public:
    using CompilerGLSL::CompilerGLSL;
    std::set<unsigned> shadow_samplers, legacy_samplers;
    bool legacy_pixel = false;
protected:
    std::string to_texture_op(const spirv_cross::Instruction& instruction, bool sparse, bool* forward,
                              spirv_cross::SmallVector<uint32_t>& inherited) override {
        auto normal = CompilerGLSL::to_texture_op(instruction, sparse, forward, inherited);
        const auto op = static_cast<spv::Op>(instruction.op);
        if (sparse || get_execution_model() != spv::ExecutionModelFragment ||
            (op != spv::OpImageSampleImplicitLod && op != spv::OpImageSampleProjImplicitLod)) return normal;
        const auto* args = stream(instruction);
        const auto& image_type = expression_type(args[2]);
        const auto& coordinate_type = expression_type(args[3]);
        if (image_type.image.dim != spv::Dim2D || image_type.image.arrayed || image_type.image.depth || coordinate_type.vecsize < 3)
            return normal;
        const auto* variable = maybe_get_backing_variable(args[2]);
        if (!variable || !has_decoration(variable->self, spv::DecorationBinding)) return normal;
        const unsigned binding = get_decoration(variable->self, spv::DecorationBinding);
        if (binding >= 16) return normal;
        shadow_samplers.insert(binding);
        if (legacy_pixel && binding < 6) legacy_samplers.insert(binding);
        const auto coord = to_expression(args[3]);
        std::string xyz = "(" + coord + ").xyz";
        if (op == spv::OpImageSampleProjImplicitLod) xyz = "(" + xyz + " / (" + coord + ").w)";
        // ATI/NVIDIA depth formats compare the reference clamped to [0,1].
        std::string shadow = "texture(d3d9Shadow" + std::to_string(binding) + ", vec3((" + xyz + ").xy, clamp((" + xyz + ").z, 0.0, 1.0))";
        if (instruction.length >= 6 && (args[4] & spv::ImageOperandsBiasMask)) shadow += ", " + to_expression(args[5]);
        shadow += ")";
        if (legacy_samplers.count(binding)) {
            const std::string mode = "((floatBitsToUint(d3d9FogParams.w) >> " + std::to_string(2 * binding) + "u) & 3u)";
            normal = "(" + mode + " == 2u ? texture(d3d9Volume" + std::to_string(binding) + ", " + xyz + ") : " + mode + " == 1u ? texture(d3d9Cube" + std::to_string(binding) + ", " + xyz + ") : " + normal + ")";
        }
        return "(((d3d9Control.w >> " + std::to_string(16 + binding) + "u) & 1u) != 0u ? vec4(" + shadow + ") : " + normal + ")";
    }
};

struct Result {
    std::string glsl, json, error;
    std::vector<uint32_t> spirv;
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
        r.spirv=words;
        D3D9CompilerGLSL glsl(std::move(words));
        glsl.legacy_pixel = !r.vertex && pd->major_ver == 1;
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
        if (r.vertex) {
            r.glsl = zero_outputs(r.glsl);
            if (r.glsl.find("layout(location = 24) out ") == std::string::npos) {
                const size_t main = r.glsl.find("void main()\n{\n");
                if (main != std::string::npos) {
                    r.glsl.insert(main + 14, "    d3d9DefaultFog = 1.0;\n");
                    r.glsl.insert(main, "layout(location = 24) out float d3d9DefaultFog;\n");
                }
            }
            // A vertex shader which never declares/writes diffuse supplies white.
            // Declared color outputs are initialized separately and stay black
            // when their instructions do not write them.
            if (r.glsl.find("layout(location = 16) out ") == std::string::npos) {
                const size_t main = r.glsl.find("void main()\n{\n");
                if (main != std::string::npos) {
                    r.glsl.insert(main + 14, "    d3d9DefaultDiffuse = vec4(1.0);\n");
                    r.glsl.insert(main, "layout(location = 16) out vec4 d3d9DefaultDiffuse;\n");
                }
            }
            r.glsl = vertex_clipping(r.glsl);
        }
        else {
            // D3D9 VPOS has integer pixel centers. deko's gl_FragCoord has
            // half-integer centers; the viewport's position offset does not
            // change this built-in. Match DXVK's SM3 MiscTypePosition lowering.
            const std::string from = "= gl_FragCoord;";
            const std::string to = "= gl_FragCoord - vec4(0.5, 0.5, 0.0, 0.0);";
            size_t pos = 0;
            while ((pos = r.glsl.find(from, pos)) != std::string::npos) {
                r.glsl.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        if (!r.vertex) {
            std::string output;
            for (const auto& o : res.stage_outputs)
                if (glsl.has_decoration(o.id, spv::DecorationLocation) && glsl.get_decoration(o.id, spv::DecorationLocation) == 0)
                    output = glsl.get_name(o.id);
            r.glsl = pixel_epilogue(r.glsl, output, pd->major_ver);
            for (unsigned sampler : glsl.shadow_samplers) {
                const size_t declarations = r.glsl.find("\n\n") + 2;
                if (declarations != std::string::npos)
                    r.glsl.insert(declarations, "layout(binding = " + std::to_string((glsl.legacy_samplers.count(sampler) ? 24 : 16) + sampler) + ") uniform sampler2DShadow d3d9Shadow" + std::to_string(sampler) + ";\n");
                if (glsl.legacy_samplers.count(sampler)) {
                    r.glsl.insert(declarations, "layout(binding = " + std::to_string(8 + sampler) + ") uniform sampler3D d3d9Volume" + std::to_string(sampler) + ";\n");
                    r.glsl.insert(declarations, "layout(binding = " + std::to_string(16 + sampler) + ") uniform samplerCube d3d9Cube" + std::to_string(sampler) + ";\n");
                }
            }
        }

        js << "{\n  \"stage\": \"" << (r.vertex ? "vertex" : "pixel") << "\", \"version\": \"" << pd->major_ver << "_"
           << pd->minor_ver << "\", \"instructions\": " << pd->instruction_count << ",\n";
        js << "  \"inputs\": [";
        for (int i = 0; i < pd->attribute_count; ++i) {
            const MOJOSHADER_attribute& a = pd->attributes[i];
            js << (i ? ", " : "") << "{\"usage\": \"" << usage_name(a.usage) << "\", \"index\": " << a.index
               << ", \"name\": " << json_str(a.name ? a.name : "") << "}";
        }
        js << "],\n  \"outputs\": [";
        bool implicit_color = !r.vertex && pd->major_ver == 1;
        for (int i = 0; i < pd->output_count; ++i)
            if (pd->outputs[i].usage == MOJOSHADER_USAGE_COLOR && pd->outputs[i].index == 0) implicit_color = false;
        if (implicit_color)
            js << "{\"usage\": \"COLOR\", \"index\": 0}";
        for (int i = 0; i < pd->output_count; ++i) {
            const MOJOSHADER_attribute& a = pd->outputs[i];
            js << (i || implicit_color ? ", " : "") << "{\"usage\": \"" << usage_name(a.usage) << "\", \"index\": " << a.index << "}";
        }
        js << "],\n  \"samplers\": [";
        for (int i = 0; i < pd->sampler_count; ++i) {
            static const char* types[] = {"2d", "cube", "volume"};
            const MOJOSHADER_sampler& s = pd->samplers[i];
            js << (i ? ", " : "") << "{\"register\": " << s.index << ", \"type\": \""
               << (glsl.legacy_samplers.count(s.index) ? "legacy" : s.type >= 0 && s.type < 3 ? types[s.type] : "?") << "\"}";
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
        std::ofstream spv(base + ".spv", std::ios::binary);
        spv.write(reinterpret_cast<const char*>(r.spirv.data()), r.spirv.size()*sizeof(uint32_t));
        ++ok;
    }
    std::printf("dx9shader: %d translated, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}
