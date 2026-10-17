// Maps dirt2_game.exe into guest memory at its preferred base, checks it is
// the exact build the code was generated from, and points every IAT slot at
// a dispatchable thunk address for its shim.
#include <cstring>
#include <fstream>
#include <vector>

#include "recomp/host.h"
#include "recomp/sha256.h"

namespace rt {

uint32_t thunk_for_import(uint32_t index) { return kThunkBase + index * 16; }

bool load_image(const std::string& exe_path, std::string& error) {
    std::ifstream in(exe_path, std::ios::binary);
    if (!in) {
        error = "cannot open " + exe_path;
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string sha = recomp::sha256_hex(file.data(), file.size());
    if (sha != recomp_image_info.sha256) {
        error = "executable does not match the recompiled build (sha256 " + sha + ")";
        return false;
    }

    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, file.data() + o, 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, file.data() + o, 4); return v; };
    const uint32_t pe = u32(0x3C);
    const uint16_t nsec = u16(pe + 6), opt_size = u16(pe + 20);
    const size_t opt = pe + 24;
    const uint32_t base = u32(opt + 28), size = u32(opt + 56), hdr_size = u32(opt + 60);
    if (base != recomp_image_info.image_base || size != recomp_image_info.image_size) {
        error = "unexpected image layout";
        return false;
    }
    if (vm_alloc(base, size, true) != base) {
        error = "image range is not available";
        return false;
    }
    std::memcpy(host_ptr(base), file.data(), hdr_size);
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t s = opt + opt_size + i * 40;
        const uint32_t vsize = u32(s + 8), rva = u32(s + 12), raw = u32(s + 16), ptr = u32(s + 20);
        const uint32_t n = std::min(raw, vsize ? vsize : raw);
        if (ptr + uint64_t(n) <= file.size()) std::memcpy(host_ptr(base + rva), file.data() + ptr, n);
    }

    for (uint32_t i = 0; i < recomp_import_count; ++i) {
        const RecompImport& imp = recomp_import_table[i];
        const uint32_t thunk = thunk_for_import(i);
        wr32(imp.iat, thunk);
        register_thunk(thunk, imp.fn);
    }
    log("loaded %s (%u imports)", exe_path.c_str(), recomp_import_count);
    return true;
}

}  // namespace rt
