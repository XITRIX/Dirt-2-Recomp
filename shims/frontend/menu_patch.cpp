// Frontend data patches, served through vfs overrides so the game folder stays
// untouched (copies in <data>/overlay).
//
// MetalFX row: the graphics menu's DX11 ambient occlusion row (item_23, combo
// pc_gfx_dao) is hidden on the DX9 path the game runs. It becomes MetalFX
// On/Off:
//   system/states.bin                    item_23 shown, titled MetalFX, placed
//                                         under Multisampling
//   system/hardware_settings_options.xml dynamic_ambient_occ options off/on;
//                                         the detail presets no longer set it
//   system/hardware_settings_config.xml  (first-run defaults) quality 0, as
//                                         the options expect
// Its title: string ids missing from the language file show as "<id>", so the
// localisation lookup (0x01161960, tools/recomp/config/hooks.txt) answers the
// row's id itself.
//
// The DX9 renderer never reads dynamic_ambient_occ, so its enabled flag in
// hardware_settings_config.xml carries the MetalFX setting
// (platform/sdl/sdl_main.cpp reads it).
//
// states.bin is the compiled frontend XML; tools/scripts/states_bin.py
// documents the format and dumps it.
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../vfs.h"
#include "recomp/host.h"
#include "recomp/x86.h"

namespace shim {

namespace {

constexpr uint32_t kTag = 0x72522200;
// The row's title id; the game's localisation lookup (hooked below) answers it.
constexpr const char* kMetalFxTitle = "lng_metalfx_combo_title";
constexpr const char* kMetalFxText = "METALFX";  // the menu's titles are capitals
uint32_t g_metalfx_text = 0;  // guest copy of kMetalFxText once the row is installed

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    return bool(out);
}

uint32_t u32(const std::string& d, size_t o) {
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}

void put32(std::string& d, uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); }

struct States {
    std::vector<std::string> strings;
    std::vector<uint32_t> nodes;  // 6 per node: name, text, attr_count, first_attr, child_count, first_child
    std::vector<uint32_t> attrs;  // 2 per attribute: name, value

    bool parse(const std::string& d) {
        std::string blob, offs, node_bytes, attr_bytes;
        auto walk = [&](auto&& self, size_t o, size_t end) -> void {
            while (o + 8 <= end) {
                const uint32_t tag = u32(d, o) & 0xFF, size = u32(d, o + 4);
                if (o + 8 + size > end) return;
                const std::string body = d.substr(o + 8, size);
                if (tag == 0x1A || tag == 0x17) self(self, o + 8, o + 8 + size);
                else if (tag == 0x1D) blob = body;
                else if (tag == 0x1E) offs = body;
                else if (tag == 0x1B) node_bytes = body;
                else if (tag == 0x1C) attr_bytes = body;
                o += 8 + size;
            }
        };
        walk(walk, 0, d.size());
        if (blob.empty() || offs.empty() || node_bytes.empty()) return false;
        for (size_t i = 0; i + 4 <= offs.size(); i += 4) strings.emplace_back(blob.c_str() + u32(offs, i));
        for (size_t i = 0; i + 4 <= node_bytes.size(); i += 4) nodes.push_back(u32(node_bytes, i));
        for (size_t i = 0; i + 4 <= attr_bytes.size(); i += 4) attrs.push_back(u32(attr_bytes, i));
        return true;
    }

    std::string encode() const {
        std::string blob, offs, node_bytes, attr_bytes;
        for (const std::string& s : strings) {
            put32(offs, uint32_t(blob.size()));
            blob += s;
            blob += '\0';
        }
        while (blob.size() % 4) blob += '\0';
        for (uint32_t v : nodes) put32(node_bytes, v);
        for (uint32_t v : attrs) put32(attr_bytes, v);
        auto chunk = [](uint32_t tag, const std::string& body) {
            std::string c;
            put32(c, kTag | tag);
            put32(c, uint32_t(body.size()));
            return c + body;
        };
        return chunk(0x1A, chunk(0x17, chunk(0x1D, blob) + chunk(0x1E, offs)) + chunk(0x1B, node_bytes) +
                               chunk(0x1C, attr_bytes));
    }

    uint32_t sid(const std::string& s) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return uint32_t(i);
        strings.push_back(s);
        return uint32_t(strings.size() - 1);
    }
    size_t count() const { return nodes.size() / 6; }
    const std::string& name(size_t n) const { return strings[nodes[6 * n]]; }
    // Index into attrs of the value of attribute key on node n, or -1.
    long attr(size_t n, const char* key) const {
        for (uint32_t k = 0; k < nodes[6 * n + 2]; ++k) {
            const size_t a = 2 * (nodes[6 * n + 3] + k);
            if (strings[attrs[a]] == key) return long(a + 1);
        }
        return -1;
    }
    long child(size_t n, const char* node_name) const {
        for (uint32_t k = 0; k < nodes[6 * n + 4]; ++k)
            if (name(nodes[6 * n + 5] + k) == node_name) return long(nodes[6 * n + 5] + k);
        return -1;
    }
};

// rv_graphics_options' item_23 (ambient occlusion): shown, titled MetalFX and
// moved under Multisampling. A row's place is its slot number (glyph
// "N.root.library", IBScreenGenericData index, IBGlyphLink "N.root.library.pill")
// and itemflow is the pad order, so MetalFX takes slot 4 and rows 4..22 move
// down one; item ids stay as they are.
constexpr int kMetalFxItem = 23, kMetalFxSlot = 4;

bool patch_states(const std::string& in, const std::string& out) {
    States s;
    if (!s.parse(read_file(in))) return false;
    auto value = [&](long a) -> const std::string& { return s.strings[s.attrs[a]]; };
    for (size_t n = 0; n < s.count(); ++n) {
        if (s.name(n) != "ScreenGarageDecorator") continue;
        const long id = s.attr(n, "id");
        if (id < 0 || value(id) != "rv_graphics_options") continue;
        const long generic = s.child(n, "ScreenGeneric"), flow = generic < 0 ? -1 : s.child(generic, "itemflow");
        if (flow < 0) return false;
        bool found = false;
        for (uint32_t k = 0; k < s.nodes[6 * generic + 4]; ++k) {
            const size_t item = s.nodes[6 * generic + 5] + k;
            const long item_id = s.attr(item, "id"), glyph = s.attr(item, "glyph");
            const long data = s.child(item, "IBScreenGenericData"), link = s.child(item, "IBGlyphLink");
            if (item_id < 0 || glyph < 0 || data < 0 || link < 0 || value(item_id).rfind("item_", 0) != 0) continue;
            const int num = std::atoi(value(item_id).c_str() + 5);
            if (num == kMetalFxItem) {
                const long vis = s.child(item, "IBDX11OnlyVisibility"), title = s.child(item, "IBTextStatic"),
                           combo = s.child(item, "IBHWConfigBasedCombo");
                const long only = vis < 0 ? -1 : s.attr(vis, "dx_11_only"), text = title < 0 ? -1 : s.attr(title, "string"),
                           cdata = combo < 0 ? -1 : s.attr(combo, "data");
                if (only < 0 || text < 0 || cdata < 0 || value(cdata) != "pc_gfx_dao") return false;
                s.attrs[only] = s.sid("false");
                s.attrs[text] = s.sid(kMetalFxTitle);
                found = true;
            }
            const int slot = num == kMetalFxItem ? kMetalFxSlot : num >= kMetalFxSlot && num < kMetalFxItem ? num + 1 : num;
            const long index = s.attr(data, "index"), linked = s.attr(link, "linked_glyph");
            if (index < 0 || linked < 0) return false;
            s.attrs[glyph] = s.sid(std::to_string(slot) + ".root.library");
            s.attrs[index] = s.sid(std::to_string(slot));
            s.attrs[linked] = s.sid(std::to_string(slot) + ".root.library.pill");
        }
        // Pad order: MetalFX's line moves to just after item_3 (Multisampling).
        // The list is one id per line; the game splits it on line breaks.
        std::string order = s.strings[s.nodes[6 * flow + 1]];
        auto line = [&](int num, size_t& at, size_t& len) {
            const std::string id = "item_" + std::to_string(num);
            for (size_t p = order.find(id); p != std::string::npos; p = order.find(id, p + 1)) {
                const size_t end = p + id.size();
                if (end < order.size() && !std::isspace(static_cast<unsigned char>(order[end]))) continue;
                at = order.rfind('\n', p);  // the line starts after its line break
                if (at == std::string::npos) return false;
                at += 1;
                len = end - at;
                return true;
            }
            return false;
        };
        size_t mat, mlen, pat, plen;
        if (!line(kMetalFxItem, mat, mlen) || !line(kMetalFxSlot - 1, pat, plen)) return false;
        const std::string moved = order.substr(mat, mlen);
        order.erase(mat - 2, mlen + 2);  // with the "\r\n" before it
        if (!line(kMetalFxSlot - 1, pat, plen)) return false;
        order.insert(pat + plen, "\r\n" + moved);
        s.nodes[6 * flow + 1] = s.sid(order);
        return found && write_file(out, s.encode());
    }
    return false;
}

// dynamic_ambient_occ becomes off/on (the options' names give the Off/On
// labels, as for skidmarks) and leaves the detail presets.
bool patch_options(const std::string& in, const std::string& out) {
    std::string x = read_file(in);
    const size_t a = x.find("<category name=\"dynamic_ambient_occ\"");
    const size_t b = a == std::string::npos ? a : x.find("</category>", a);
    if (b == std::string::npos) return false;
    x.replace(a, b - a,
              "<category name=\"dynamic_ambient_occ\" >\r\n"
              "      <option name=\"off\" >\r\n        <dynamic_ambient_occ enabled=\"false\" quality=\"0\" />\r\n"
              "      </option>\r\n"
              "      <option name=\"on\" >\r\n        <dynamic_ambient_occ enabled=\"true\" quality=\"0\" />\r\n"
              "      </option>\r\n    ");
    for (size_t r; (r = x.find("<ref category=\"dynamic_ambient_occ\"")) != std::string::npos;) {
        size_t start = x.rfind('\n', r);
        start = start == std::string::npos ? r : start;
        x.erase(start, x.find("/>", r) + 2 - start);
    }
    return write_file(out, x);
}

// The element the row writes, with quality forced to 0 so the off/on options
// match what is saved (the shipped defaults have quality 1).
bool fix_dao_quality(std::string& x) {
    const size_t tag = x.find("<dynamic_ambient_occ ");
    if (tag == std::string::npos) return false;
    const size_t end = x.find('>', tag), q = x.find("quality=\"", tag);
    if (q == std::string::npos || q > end) return false;
    const size_t v = q + 9, ve = x.find('"', v);
    if (x.compare(v, ve - v, "0") == 0) return false;
    x.replace(v, ve - v, "0");
    return true;
}

bool patch_defaults(const std::string& in, const std::string& out) {
    std::string x = read_file(in);
    if (x.find("<dynamic_ambient_occ ") == std::string::npos) return false;
    fix_dao_quality(x);
    return write_file(out, x);
}

}  // namespace

int read_metalfx_setting(const std::string& path) {
    std::string x = read_file(path);
    const size_t tag = x.find("<dynamic_ambient_occ ");
    const size_t at = tag == std::string::npos ? tag : x.find("enabled=\"", tag);
    if (at == std::string::npos || at > x.find('>', tag)) return -1;
    if (fix_dao_quality(x)) write_file(path, x);
    return x.compare(at + 9, 5, "false") == 0 ? 0 : 1;
}

void install_menu_patches(bool metalfx_row) {
    if (!metalfx_row) return;  // the row stays hidden, as the game ships it
    const std::string dir = vfs::data_dir() + "/overlay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    struct Patch {
        const char* rel;
        bool (*fn)(const std::string&, const std::string&);
    };
    const Patch patches[] = {{"system/states.bin", patch_states},
                             {"system/hardware_settings_options.xml", patch_options},
                             {"system/hardware_settings_config.xml", patch_defaults}};
    for (const Patch& p : patches) {  // all or nothing: half the patch shows ambient occlusion's options
        const std::string out = dir + "/" + std::filesystem::path(p.rel).filename().string();
        if (!p.fn(vfs::host_path(std::string(vfs::kInstallDir) + "\\" + p.rel), out)) {
            rt::log("menu: cannot patch %s, MetalFX row not added", p.rel);
            return;
        }
    }
    g_metalfx_text = rt::heap_alloc(uint32_t(std::strlen(kMetalFxText) + 1), true);
    std::memcpy(rt::host_ptr(g_metalfx_text), kMetalFxText, std::strlen(kMetalFxText));
    for (const Patch& p : patches)
        vfs::set_override(p.rel, dir + "/" + std::filesystem::path(p.rel).filename().string());
    rt::log("menu: MetalFX row added to the graphics options");
}

// Localisation lookup, thiscall (table, const char* id) -> text or 0, ret 4.
extern "C" void f_01161960_orig(X86* c);
extern "C" void f_01161960(X86* c) {
    const uint32_t id = rd32(c->esp + 4);
    if (g_metalfx_text && id && std::strcmp(reinterpret_cast<const char*>(rt::host_ptr(id)), kMetalFxTitle) == 0) {
        c->eax = g_metalfx_text;
        c->esp += 8;
        return;
    }
    f_01161960_orig(c);
}

}  // namespace shim
