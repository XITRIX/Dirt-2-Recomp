// printf-style formatting with arguments read from the guest stack (wsprintfA
// and friends). Supports the conversions MSVC-era code uses: d i u x X o c s S
// p f e g %, flags, width, precision, h/l/ll/I64 size prefixes.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "recomp/x86.h"

namespace shim {

inline std::string guest_format(const char* fmt, uint32_t args) {
    std::string out;
    uint32_t ap = args;
    auto next32 = [&] { const uint32_t v = rd32(ap); ap += 4; return v; };
    auto next64 = [&] { const uint64_t v = rd64(ap); ap += 8; return v; };
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        std::string spec = "%";
        ++p;
        while (*p && std::strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') { spec += std::to_string(int32_t(next32())); ++p; }
        while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') { spec += std::to_string(int32_t(next32())); ++p; }
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        int size = 32;
        if (*p == 'h') { ++p; size = 16; }
        else if (*p == 'l') { ++p; if (*p == 'l') { ++p; size = 64; } }
        else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { p += 3; size = 64; }
        else if (*p == 'I') { ++p; }
        char buf[512];
        const char conv = *p;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                spec += size == 64 ? "ll" : "";
                spec += conv;
                if (size == 64) std::snprintf(buf, sizeof buf, spec.c_str(), (long long)next64());
                else if (conv == 'd' || conv == 'i') std::snprintf(buf, sizeof buf, spec.c_str(), int32_t(next32()));
                else std::snprintf(buf, sizeof buf, spec.c_str(), next32());
                out += buf;
                break;
            }
            case 'c':
                out += char(next32());
                break;
            case 's': case 'S': {
                const uint32_t s = next32();
                std::string v = s ? reinterpret_cast<const char*>(RECOMP_MEM(s)) : "(null)";
                if (conv == 'S' && s) {  // wide string in narrow formatter
                    v.clear();
                    for (uint32_t i = 0; rd16(s + 2 * i); ++i) v += char(rd16(s + 2 * i));
                }
                spec += 's';
                std::snprintf(buf, sizeof buf, spec.c_str(), v.c_str());
                out += buf;
                break;
            }
            case 'p':
                std::snprintf(buf, sizeof buf, "%08X", next32());
                out += buf;
                break;
            case 'f': case 'e': case 'g': case 'E': case 'G': {
                double d;
                std::memcpy(&d, RECOMP_MEM(ap), 8);
                ap += 8;
                spec += conv;
                std::snprintf(buf, sizeof buf, spec.c_str(), d);
                out += buf;
                break;
            }
            case '%': out += '%'; break;
            case 0: return out;
            default: out += conv; break;
        }
    }
    return out;
}

}  // namespace shim
