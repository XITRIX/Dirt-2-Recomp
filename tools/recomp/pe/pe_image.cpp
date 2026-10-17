#include "pe/pe_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "recomp/sha256.h"

namespace recomp {

namespace {

template <typename T>
bool read_at(const std::vector<uint8_t>& file, size_t off, T& out) {
    if (off + sizeof(T) > file.size()) return false;
    std::memcpy(&out, file.data() + off, sizeof(T));
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

std::optional<PeImage> PeImage::load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return std::nullopt;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    uint16_t mz = 0;
    uint32_t pe_off = 0, sig = 0;
    if (!read_at(file, 0, mz) || mz != 0x5A4D || !read_at(file, 0x3C, pe_off) ||
        !read_at(file, pe_off, sig) || sig != 0x00004550) {
        error = "not a PE file";
        return std::nullopt;
    }
    uint16_t machine = 0, nsections = 0, opt_size = 0, magic = 0;
    read_at(file, pe_off + 4, machine);
    read_at(file, pe_off + 6, nsections);
    read_at(file, pe_off + 20, opt_size);
    const size_t opt = pe_off + 24;
    read_at(file, opt, magic);
    if (machine != 0x14C || magic != 0x10B) {
        error = "only PE32 i386 images are supported";
        return std::nullopt;
    }

    PeImage img;
    uint32_t entry_rva = 0, size_of_headers = 0, ndirs = 0;
    read_at(file, opt + 16, entry_rva);
    read_at(file, opt + 28, img.image_base_);
    read_at(file, opt + 56, img.image_size_);
    read_at(file, opt + 60, size_of_headers);
    read_at(file, opt + 92, ndirs);
    img.entry_point_ = img.image_base_ + entry_rva;

    struct Dir { uint32_t rva = 0, size = 0; };
    std::vector<Dir> dirs(std::min<uint32_t>(ndirs, 16));
    for (size_t i = 0; i < dirs.size(); ++i) {
        read_at(file, opt + 96 + i * 8, dirs[i].rva);
        read_at(file, opt + 100 + i * 8, dirs[i].size);
    }

    img.mapped_.assign(img.image_size_, 0);
    std::memcpy(img.mapped_.data(), file.data(), std::min<size_t>(size_of_headers, file.size()));

    const size_t sec_off = opt + opt_size;
    for (uint16_t i = 0; i < nsections; ++i) {
        const size_t s = sec_off + i * 40;
        char name[9] = {};
        if (s + 40 > file.size()) break;
        std::memcpy(name, file.data() + s, 8);
        Section sec;
        sec.name = name;
        uint32_t rva = 0, raw_ptr = 0;
        read_at(file, s + 8, sec.vsize);
        read_at(file, s + 12, rva);
        read_at(file, s + 16, sec.raw_size);
        read_at(file, s + 20, raw_ptr);
        read_at(file, s + 36, sec.characteristics);
        sec.va = img.image_base_ + rva;
        const uint32_t copy = std::min(sec.raw_size, sec.vsize ? sec.vsize : sec.raw_size);
        if (raw_ptr + uint64_t(copy) <= file.size() && rva + uint64_t(copy) <= img.image_size_)
            std::memcpy(img.mapped_.data() + rva, file.data() + raw_ptr, copy);
        img.sections_.push_back(sec);
    }

    auto rva_ptr = [&](uint32_t rva) -> const uint8_t* {
        return rva < img.image_size_ ? img.mapped_.data() + rva : nullptr;
    };
    auto rva_u32 = [&](uint32_t rva) -> uint32_t {
        uint32_t v = 0;
        if (rva + 4 <= img.image_size_) std::memcpy(&v, img.mapped_.data() + rva, 4);
        return v;
    };
    auto rva_str = [&](uint32_t rva) -> std::string {
        const uint8_t* p = rva_ptr(rva);
        if (!p) return {};
        return std::string(reinterpret_cast<const char*>(p),
                           strnlen(reinterpret_cast<const char*>(p), img.image_size_ - rva));
    };

    // Imports (directory 1).
    if (dirs.size() > 1 && dirs[1].rva) {
        for (uint32_t d = dirs[1].rva;; d += 20) {
            const uint32_t ilt = rva_u32(d), name_rva = rva_u32(d + 12), iat = rva_u32(d + 16);
            if (!name_rva && !iat) break;
            const std::string dll = lower(rva_str(name_rva));
            const uint32_t lookup = ilt ? ilt : iat;
            for (uint32_t k = 0;; ++k) {
                const uint32_t entry = rva_u32(lookup + k * 4);
                if (!entry) break;
                Import imp;
                imp.dll = dll;
                imp.iat_va = img.image_base_ + iat + k * 4;
                if (entry & 0x80000000)
                    imp.ordinal = uint16_t(entry & 0xFFFF);
                else {
                    uint16_t hint = 0;
                    if (const uint8_t* p = rva_ptr(entry)) std::memcpy(&hint, p, 2);
                    imp.ordinal = hint;
                    imp.name = rva_str(entry + 2);
                }
                img.imports_.push_back(std::move(imp));
            }
        }
    }

    // Base relocations (directory 5), taken from the file since .reloc is mapped.
    if (dirs.size() > 5 && dirs[5].rva) {
        uint32_t off = dirs[5].rva;
        const uint32_t end = dirs[5].rva + dirs[5].size;
        while (off + 8 <= end) {
            const uint32_t page = rva_u32(off), block = rva_u32(off + 4);
            if (block < 8) break;
            for (uint32_t e = 8; e < block; e += 2) {
                uint16_t v = 0;
                std::memcpy(&v, img.mapped_.data() + off + e, 2);
                if ((v >> 12) == 3) img.relocs_.push_back(img.image_base_ + page + (v & 0xFFF));
            }
            off += block;
        }
        std::sort(img.relocs_.begin(), img.relocs_.end());
        img.reloc_set_.insert(img.relocs_.begin(), img.relocs_.end());
    }

    // TLS (directory 9). Fields are absolute VAs in PE32.
    if (dirs.size() > 9 && dirs[9].rva) {
        TlsInfo t;
        t.raw_start = rva_u32(dirs[9].rva);
        t.raw_end = rva_u32(dirs[9].rva + 4);
        t.index_addr = rva_u32(dirs[9].rva + 8);
        t.callbacks = rva_u32(dirs[9].rva + 12);
        t.zero_fill = rva_u32(dirs[9].rva + 16);
        img.tls_ = t;
    }

    for (size_t i = 0; i < img.imports_.size(); ++i) img.import_index_[img.imports_[i].iat_va] = i;
    img.sha256_ = sha256_hex(file.data(), file.size());
    return img;
}

const Section* PeImage::section_at(uint32_t addr) const {
    for (const Section& s : sections_)
        if (s.contains(addr)) return &s;
    return nullptr;
}

uint32_t PeImage::read32(uint32_t addr) const {
    uint32_t v = 0;
    if (in_image(addr, 4)) std::memcpy(&v, ptr(addr), 4);
    return v;
}

uint32_t PeImage::bytes_left(uint32_t addr) const {
    const Section* s = section_at(addr);
    return s ? s->va + s->vsize - addr : 0;
}

const Import* PeImage::import_at(uint32_t iat_va) const {
    auto it = import_index_.find(iat_va);
    return it == import_index_.end() ? nullptr : &imports_[it->second];
}

}  // namespace recomp
