#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

#include "shim.h"

namespace shim {

bool g_trace = std::getenv("RECOMP_TRACE") != nullptr;

void trace_call(const char* sym, uint32_t ret_addr, const Args& a, uint32_t result) {
    rt::log("t%u %s(%08X, %08X, %08X, %08X) = %08X  [from %08X]", rt::current_thread_id(), sym + 4, a[0], a[1], a[2], a[3], result, ret_addr);
}

namespace {

struct Export {
    ShimFn fn;
    uint32_t thunk = 0;
};

std::mutex& reg_mu() {
    static std::mutex m;
    return m;
}
// Keyed by "dll!name" or "dll!#ordinal", lower-case dll.
std::unordered_map<std::string, Export>& registry() {
    static std::unordered_map<std::string, Export> r;
    return r;
}
uint32_t g_next_thunk = rt::kThunkBase + 0x80000;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
}

std::mutex g_handle_mu;
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x1000;

}  // namespace

void register_shim(const char* dll, const char* name, uint16_t ordinal, ShimFn fn) {
    std::lock_guard lk(reg_mu());
    const std::string d = lower(dll);
    if (*name) registry()[d + "!" + name] = Export{fn};
    if (ordinal) registry()[d + "!#" + std::to_string(ordinal)] = Export{fn};
}

uint32_t resolve_export(const std::string& dll, const char* name, uint16_t ordinal) {
    std::lock_guard lk(reg_mu());
    std::string d = lower(dll);
    if (d.find('.') == std::string::npos) d += ".dll";
    const std::string key = name ? d + "!" + name : d + "!#" + std::to_string(ordinal);
    auto it = registry().find(key);
    if (it == registry().end()) return 0;
    if (!it->second.thunk) {
        it->second.thunk = g_next_thunk;
        g_next_thunk += 16;
        rt::register_thunk(it->second.thunk, it->second.fn);
    }
    return it->second.thunk;
}

// ---- handles ----

uint32_t handle_new(std::shared_ptr<KObject> obj) {
    std::lock_guard lk(g_handle_mu);
    const uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
    std::lock_guard lk(g_handle_mu);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

bool handle_close(uint32_t h) {
    std::lock_guard lk(g_handle_mu);
    return g_handles.erase(h) != 0;
}

uint32_t handle_dup(uint32_t h) {
    auto obj = handle_get(h);
    return obj ? handle_new(obj) : 0;
}

// ---- strings ----

std::string from_utf16(uint32_t w, int len) {
    std::string out;
    if (!w) return out;
    for (int i = 0; len < 0 || i < len; ++i) {
        uint32_t ch = rd16(w + 2 * i);
        if (len < 0 && !ch) break;
        if (ch >= 0xD800 && ch < 0xDC00 && (len < 0 || i + 1 < len)) {
            const uint32_t lo = rd16(w + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (ch < 0x80) out += char(ch);
        else if (ch < 0x800) {
            out += char(0xC0 | (ch >> 6));
            out += char(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            out += char(0xE0 | (ch >> 12));
            out += char(0x80 | ((ch >> 6) & 0x3F));
            out += char(0x80 | (ch & 0x3F));
        } else {
            out += char(0xF0 | (ch >> 18));
            out += char(0x80 | ((ch >> 12) & 0x3F));
            out += char(0x80 | ((ch >> 6) & 0x3F));
            out += char(0x80 | (ch & 0x3F));
        }
    }
    return out;
}

std::u16string to_utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = s[i];
        uint32_t ch;
        int n;
        if (b < 0x80) { ch = b; n = 1; }
        else if ((b >> 5) == 6 && i + 1 < s.size()) { ch = b & 0x1F; n = 2; }
        else if ((b >> 4) == 14 && i + 2 < s.size()) { ch = b & 0x0F; n = 3; }
        else if ((b >> 3) == 30 && i + 3 < s.size()) { ch = b & 0x07; n = 4; }
        else { ch = b; n = 1; }  // invalid UTF-8: treat as Latin-1
        for (int k = 1; k < n; ++k) ch = (ch << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (ch >= 0x10000) {
            ch -= 0x10000;
            out += char16_t(0xD800 + (ch >> 10));
            out += char16_t(0xDC00 + (ch & 0x3FF));
        } else {
            out += char16_t(ch);
        }
    }
    return out;
}

uint32_t copy_out(uint32_t dst, uint32_t cap, const std::string& s) {
    if (!dst || !cap) return 0;
    const uint32_t n = std::min<uint32_t>(uint32_t(s.size()), cap - 1);
    std::memcpy(rt::host_ptr(dst), s.data(), n);
    wr8(dst + n, 0);
    return n;
}

uint32_t copy_out_w(uint32_t dst, uint32_t cap, const std::string& utf8) {
    if (!dst || !cap) return 0;
    const std::u16string w = to_utf16(utf8);
    const uint32_t n = std::min<uint32_t>(uint32_t(w.size()), cap - 1);
    for (uint32_t i = 0; i < n; ++i) wr16(dst + 2 * i, w[i]);
    wr16(dst + 2 * n, 0);
    return n;
}

}  // namespace shim
