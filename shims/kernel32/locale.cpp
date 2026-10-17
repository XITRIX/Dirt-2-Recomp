// kernel32: code pages, locale and string APIs. The process runs as
// English (United Kingdom), code page 1252; 1252 is treated as Latin-1.
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <ctime>

#include "../shim.h"

using namespace shim;

namespace {

constexpr uint32_t kLcid = 0x0809;  // en-GB
constexpr uint32_t CP_UTF8 = 65001;

// Decodes a guest multibyte string to UTF-16 code units.
std::u16string mb_decode(uint32_t cp, uint32_t src, int len) {
    std::string bytes;
    if (len < 0) bytes = rt::host_str(src);
    else bytes.assign(rt::host_str(src), size_t(len));
    if (cp == CP_UTF8) return to_utf16(bytes);
    std::u16string out;
    for (unsigned char b : bytes) out += char16_t(b);
    return out;
}

std::string mb_encode(uint32_t cp, const std::u16string& w) {
    if (cp == CP_UTF8) {
        std::string s;
        // Reuse the guest-memory converter through a temporary host buffer.
        for (char16_t ch : w) {
            if (ch < 0x80) s += char(ch);
            else if (ch < 0x800) {
                s += char(0xC0 | (ch >> 6));
                s += char(0x80 | (ch & 0x3F));
            } else {
                s += char(0xE0 | (ch >> 12));
                s += char(0x80 | ((ch >> 6) & 0x3F));
                s += char(0x80 | (ch & 0x3F));
            }
        }
        return s;
    }
    std::string s;
    for (char16_t ch : w) s += ch < 0x100 ? char(ch) : '?';
    return s;
}

std::u16string read_w(uint32_t p, int len) {
    std::u16string w;
    if (len < 0) len = int(wlen(p));
    for (int i = 0; i < len; ++i) w += char16_t(rd16(p + 2 * i));
    return w;
}

uint16_t ctype1(uint32_t ch) {
    uint16_t t = 0;
    if (ch < 0x80) {
        if (std::isupper(int(ch))) t |= 0x1;
        if (std::islower(int(ch))) t |= 0x2;
        if (std::isdigit(int(ch))) t |= 0x4;
        if (std::isspace(int(ch))) t |= 0x8;
        if (std::ispunct(int(ch))) t |= 0x10;
        if (std::iscntrl(int(ch))) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (std::isxdigit(int(ch))) t |= 0x80;
        if (std::isalpha(int(ch))) t |= 0x100;
    } else {
        if (std::iswupper(wint_t(ch))) t |= 0x1 | 0x100;
        if (std::iswlower(wint_t(ch))) t |= 0x2 | 0x100;
        if (std::iswalpha(wint_t(ch))) t |= 0x100;
        if (std::iswpunct(wint_t(ch))) t |= 0x10;
        if (ch == 0xA0) t |= 0x8 | 0x40;
    }
    return t;
}

char16_t to_lower16(char16_t ch) { return char16_t(std::towlower(wint_t(ch))); }
char16_t to_upper16(char16_t ch) { return char16_t(std::towupper(wint_t(ch))); }

int compare16(std::u16string a, std::u16string b, bool icase) {
    if (icase) {
        for (auto& ch : a) ch = to_lower16(ch);
        for (auto& ch : b) ch = to_lower16(ch);
    }
    const int r = a.compare(b);
    return r < 0 ? 1 : r == 0 ? 2 : 3;  // CSTR_LESS_THAN / EQUAL / GREATER_THAN
}

std::string locale_info(uint32_t type) {
    switch (type & 0xFFFF) {
        case 0x0001: return "0809";            // LOCALE_ILANGUAGE
        case 0x0002: return "English (United Kingdom)";
        case 0x0003: return "ENG";             // LOCALE_SABBREVLANGNAME
        case 0x0004: return "English";         // LOCALE_SNATIVELANGNAME
        case 0x0005: return "44";              // LOCALE_ICOUNTRY
        case 0x0006: return "United Kingdom";
        case 0x0007: return "GBR";
        case 0x000B: return "850";             // LOCALE_IDEFAULTCODEPAGE
        case 0x000C: return ",";               // LOCALE_SLIST
        case 0x000E: return ".";               // LOCALE_SDECIMAL
        case 0x000F: return ",";               // LOCALE_STHOUSAND
        case 0x0014: return "\xa3";            // LOCALE_SCURRENCY (pound sign, cp1252)
        case 0x0015: return "\xa3";
        case 0x001D: return "/";               // LOCALE_SDATE
        case 0x001E: return ":";               // LOCALE_STIME
        case 0x001F: return "dd/MM/yyyy";      // LOCALE_SSHORTDATE
        case 0x0020: return "dd MMMM yyyy";    // LOCALE_SLONGDATE
        case 0x1001: return "English";         // LOCALE_SENGLANGUAGE
        case 0x1002: return "United Kingdom";  // LOCALE_SENGCOUNTRY
        case 0x1004: return "1252";            // LOCALE_IDEFAULTANSICODEPAGE
        case 0x1003: return "HH:mm:ss";        // LOCALE_STIMEFORMAT
        case 0x0059: return "en";              // LOCALE_SISO639LANGNAME
        case 0x005A: return "GB";              // LOCALE_SISO3166CTRYNAME
        default: return "";
    }
}

}  // namespace

STDCALL(kernel32, GetACP, 0) { return 1252; }
STDCALL(kernel32, GetOEMCP, 0) { return 437; }
STDCALL(kernel32, IsValidCodePage, 1) { return a[0] == 1252 || a[0] == 437 || a[0] == CP_UTF8 || a[0] == 850; }
STDCALL(kernel32, GetCPInfo, 2) {
    const uint32_t d = a[1];
    std::memset(rt::host_ptr(d), 0, 20);
    wr32(d, a[0] == CP_UTF8 ? 4 : 1);  // MaxCharSize
    wr8(d + 4, '?');                    // DefaultChar
    return 1;
}
STDCALL(kernel32, GetUserDefaultLCID, 0) { return kLcid; }
STDCALL(kernel32, GetUserDefaultLangID, 0) { return kLcid; }
STDCALL(kernel32, GetThreadLocale, 0) { return kLcid; }
STDCALL(kernel32, IsValidLocale, 2) { return 1; }
STDCALL(kernel32, EnumSystemLocalesA, 2) {
    const uint32_t s = rt::guest_strdup("00000809");
    callback(c, a[0], {s});
    rt::heap_free(s);
    return 1;
}
STDCALL(kernel32, GetLocaleInfoA, 4) {
    const std::string v = locale_info(a[1]);
    if (!a[3]) return uint32_t(v.size() + 1);
    if (a[3] <= v.size()) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return copy_out(a[2], a[3], v) + 1;
}
STDCALL(kernel32, GetLocaleInfoW, 4) {
    const std::string v = locale_info(a[1]);
    std::u16string w;
    for (unsigned char b : v) w += char16_t(b);
    if (!a[3]) return uint32_t(w.size() + 1);
    if (a[3] <= w.size()) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (size_t i = 0; i < w.size(); ++i) wr16(a[2] + 2 * uint32_t(i), w[i]);
    wr16(a[2] + 2 * uint32_t(w.size()), 0);
    return uint32_t(w.size() + 1);
}

STDCALL(kernel32, MultiByteToWideChar, 6) {
    const int len = int32_t(a[3]);
    std::u16string w = mb_decode(a[0], a[2], len);
    if (len < 0) w += u'\0';
    if (!a[5]) return uint32_t(w.size());
    if (a[5] < w.size()) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (size_t i = 0; i < w.size(); ++i) wr16(a[4] + 2 * uint32_t(i), w[i]);
    return uint32_t(w.size());
}
STDCALL(kernel32, WideCharToMultiByte, 8) {
    const int len = int32_t(a[3]);
    std::string s = mb_encode(a[0], read_w(a[2], len));
    if (len < 0) s += '\0';
    if (!a[5]) return uint32_t(s.size());
    if (a[5] < s.size()) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(rt::host_ptr(a[4]), s.data(), s.size());
    return uint32_t(s.size());
}

STDCALL(kernel32, GetStringTypeW, 4) {
    if (a[0] != 1) return 0;  // only CT_CTYPE1
    const std::u16string w = read_w(a[1], int32_t(a[2]));
    for (size_t i = 0; i < w.size(); ++i) wr16(a[3] + 2 * uint32_t(i), ctype1(w[i]));
    return 1;
}
STDCALL(kernel32, GetStringTypeExW, 5) {
    if (a[1] != 1) return 0;
    const std::u16string w = read_w(a[2], int32_t(a[3]));
    for (size_t i = 0; i < w.size(); ++i) wr16(a[4] + 2 * uint32_t(i), ctype1(w[i]));
    return 1;
}
STDCALL(kernel32, GetStringTypeA, 5) {
    if (a[1] != 1) return 0;
    const std::u16string w = mb_decode(1252, a[2], int32_t(a[3]));
    for (size_t i = 0; i < w.size(); ++i) wr16(a[4] + 2 * uint32_t(i), ctype1(w[i]));
    return 1;
}
STDCALL(kernel32, GetStringTypeExA, 5) {
    if (a[1] != 1) return 0;
    const std::u16string w = mb_decode(1252, a[2], int32_t(a[3]));
    for (size_t i = 0; i < w.size(); ++i) wr16(a[4] + 2 * uint32_t(i), ctype1(w[i]));
    return 1;
}

STDCALL(kernel32, LCMapStringW, 6) {
    std::u16string w = read_w(a[2], int32_t(a[3]));
    const uint32_t flags = a[1];
    for (auto& ch : w) {
        if (flags & 0x100) ch = to_lower16(ch);
        if (flags & 0x200) ch = to_upper16(ch);
    }
    if (int32_t(a[3]) < 0) w += u'\0';
    if (!a[5]) return uint32_t(w.size());
    if (a[5] < w.size()) return 0;
    for (size_t i = 0; i < w.size(); ++i) wr16(a[4] + 2 * uint32_t(i), w[i]);
    return uint32_t(w.size());
}
STDCALL(kernel32, LCMapStringA, 6) {
    std::u16string w = mb_decode(1252, a[2], int32_t(a[3]));
    const uint32_t flags = a[1];
    for (auto& ch : w) {
        if (flags & 0x100) ch = to_lower16(ch);
        if (flags & 0x200) ch = to_upper16(ch);
    }
    std::string s = mb_encode(1252, w);
    if (int32_t(a[3]) < 0) s += '\0';
    if (!a[5]) return uint32_t(s.size());
    if (a[5] < s.size()) return 0;
    std::memcpy(rt::host_ptr(a[4]), s.data(), s.size());
    return uint32_t(s.size());
}
STDCALL(kernel32, CompareStringW, 6) {
    return compare16(read_w(a[2], int32_t(a[3])), read_w(a[4], int32_t(a[5])), (a[1] & 1) != 0);
}
STDCALL(kernel32, CompareStringA, 6) {
    return compare16(mb_decode(1252, a[2], int32_t(a[3])), mb_decode(1252, a[4], int32_t(a[5])), (a[1] & 1) != 0);
}
STDCALL(kernel32, lstrlenA, 1) { return a[0] ? uint32_t(std::strlen(a.str(0))) : 0; }
STDCALL(kernel32, lstrlenW, 1) { return a[0] ? wlen(a[0]) : 0; }
STDCALL(kernel32, lstrcmpiA, 2) {
    const int r = strcasecmp(a.str(0), a.str(1));
    return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}
STDCALL(kernel32, lstrcmpiW, 2) {
    const int r = compare16(read_w(a[0], -1), read_w(a[1], -1), true) - 2;
    return uint32_t(r);
}

STDCALL(kernel32, GetDateFormatA, 6) {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%d/%m/%Y", &tm);
    if (!a[5]) return uint32_t(std::strlen(buf) + 1);
    return copy_out(a[4], a[5], buf) + 1;
}
STDCALL(kernel32, GetTimeFormatA, 6) {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    if (!a[5]) return uint32_t(std::strlen(buf) + 1);
    return copy_out(a[4], a[5], buf) + 1;
}
