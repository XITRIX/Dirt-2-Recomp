// Minimal PE32 loader for the recompiler. Maps the image as the Windows loader
// would at its preferred base (no relocation applied) and exposes the tables
// the analysis needs: sections, imports, base relocations and TLS.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace recomp {

struct Section {
    std::string name;
    uint32_t va = 0;        // absolute virtual address (image base + RVA)
    uint32_t vsize = 0;
    uint32_t raw_size = 0;
    uint32_t characteristics = 0;

    bool executable() const { return characteristics & 0x20000000; }
    bool writable() const { return characteristics & 0x80000000; }
    bool contains(uint32_t addr) const { return addr >= va && addr < va + vsize; }
};

struct Import {
    std::string dll;        // lower-cased, e.g. "kernel32.dll"
    std::string name;       // empty when imported by ordinal
    uint16_t ordinal = 0;
    uint32_t iat_va = 0;    // absolute address of the IAT slot
};

struct TlsInfo {
    uint32_t raw_start = 0, raw_end = 0, index_addr = 0, callbacks = 0;
    uint32_t zero_fill = 0;
};

class PeImage {
public:
    static std::optional<PeImage> load(const std::string& path, std::string& error);
    // In-memory image with one executable section at code_va, for tests.
    static PeImage from_code(uint32_t image_base, uint32_t code_va, const std::vector<uint8_t>& code);

    uint32_t image_base() const { return image_base_; }
    uint32_t image_size() const { return image_size_; }
    uint32_t entry_point() const { return entry_point_; }
    const std::vector<Section>& sections() const { return sections_; }
    const std::vector<Import>& imports() const { return imports_; }
    const std::optional<TlsInfo>& tls() const { return tls_; }
    const std::string& sha256() const { return sha256_; }

    // Absolute addresses of every 32-bit HIGHLOW fixup.
    const std::vector<uint32_t>& relocations() const { return relocs_; }
    bool has_reloc(uint32_t addr) const { return reloc_set_.count(addr) != 0; }

    bool in_image(uint32_t addr, uint32_t len = 1) const {
        return addr >= image_base_ && uint64_t(addr) + len <= uint64_t(image_base_) + image_size_;
    }
    const Section* section_at(uint32_t addr) const;
    bool is_code(uint32_t addr) const {
        const Section* s = section_at(addr);
        return s && s->executable();
    }
    const uint8_t* ptr(uint32_t addr) const { return mapped_.data() + (addr - image_base_); }
    uint32_t read32(uint32_t addr) const;
    uint8_t read8(uint32_t addr) const { return *ptr(addr); }
    // Bytes available from addr to the end of its section's virtual extent.
    uint32_t bytes_left(uint32_t addr) const;

    // Import whose IAT slot is at addr, if any.
    const Import* import_at(uint32_t iat_va) const;

private:
    uint32_t image_base_ = 0, image_size_ = 0, entry_point_ = 0;
    std::vector<uint8_t> mapped_;
    std::vector<Section> sections_;
    std::vector<Import> imports_;
    std::unordered_map<uint32_t, size_t> import_index_;
    std::vector<uint32_t> relocs_;
    std::unordered_set<uint32_t> reloc_set_;
    std::optional<TlsInfo> tls_;
    std::string sha256_;
};

}  // namespace recomp
