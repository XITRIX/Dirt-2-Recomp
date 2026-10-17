#include "vfs.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "recomp/host.h"

namespace vfs {

namespace {

std::mutex g_mu;
std::string g_data;
std::string g_cwd = kInstallDir;
std::unordered_map<std::string, std::vector<std::string>> g_dirs;  // host dir -> entries

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : p) {
        if (ch == '\\' || ch == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const std::vector<std::string>& listing(const std::string& dir) {
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end()) return it->second;
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) names.emplace_back(e->d_name);
        closedir(d);
    }
    return g_dirs[dir] = std::move(names);
}

std::string default_data_dir() {
    if (const char* e = std::getenv("DIRT2_DATA_DIR")) return e;
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/Library/Application Support/Dirt2Recomp";
    return "./dirt2_data";
}

}  // namespace

void set_data_dir(const std::string& d) {
    std::lock_guard lk(g_mu);
    g_data = d;
}

std::string data_dir() {
    std::lock_guard lk(g_mu);
    if (g_data.empty()) g_data = default_data_dir();
    return g_data;
}

std::string full_path(const std::string& in) {
    std::string p = in;
    if (p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
    std::string base;
    std::string rest;
    if (p.size() >= 2 && p[1] == ':') {
        base = std::string(1, char(std::toupper(static_cast<unsigned char>(p[0])))) + ":";
        rest = p.substr(2);
        if (rest.empty() || (rest[0] != '\\' && rest[0] != '/')) rest = cwd().substr(2) + "\\" + rest;  // drive-relative
    } else if (!p.empty() && (p[0] == '\\' || p[0] == '/')) {
        base = cwd().substr(0, 2);
        rest = p;
    } else {
        base = cwd().substr(0, 2);
        rest = cwd().substr(2) + "\\" + p;
    }
    std::vector<std::string> parts;
    for (const std::string& s : split(rest)) {
        if (s == ".") continue;
        if (s == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(s);
    }
    std::string out = base;
    for (const std::string& s : parts) out += "\\" + s;
    if (parts.empty()) out += "\\";
    return out;
}

std::string host_path(const std::string& win) {
    const std::string full = full_path(win);
    std::vector<std::string> parts = split(full.substr(2));
    std::string root;
    const std::vector<std::string> install = split(std::string(kInstallDir).substr(2));
    size_t skip = 0;
    if (full[0] == 'C' && parts.size() >= install.size() &&
        std::equal(install.begin(), install.end(), parts.begin(), iequals)) {
        root = rt::game_dir();
        skip = install.size();
    } else {
        root = data_dir() + "/" + full.substr(0, 1);
    }
    std::lock_guard lk(g_mu);
    std::string cur = root;
    bool exists = true;
    for (size_t i = skip; i < parts.size(); ++i) {
        std::string name = parts[i];
        if (exists) {
            const auto& entries = listing(cur);
            auto it = std::find(entries.begin(), entries.end(), name);
            if (it == entries.end())
                it = std::find_if(entries.begin(), entries.end(), [&](const std::string& e) { return iequals(e, name); });
            if (it != entries.end()) name = *it;
            else exists = false;
        }
        cur += "/" + name;
    }
    return cur;
}

void invalidate(const std::string& host_dir) {
    std::lock_guard lk(g_mu);
    g_dirs.erase(host_dir);
}

std::string cwd() {
    std::lock_guard lk(g_mu);
    return g_cwd;
}

bool set_cwd(const std::string& win) {
    const std::string f = full_path(win);
    struct stat st;
    if (stat(host_path(f).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    std::lock_guard lk(g_mu);
    g_cwd = f;
    if (g_cwd.size() > 3 && g_cwd.back() == '\\') g_cwd.pop_back();
    return true;
}

}  // namespace vfs
