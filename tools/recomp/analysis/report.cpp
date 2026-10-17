#include "analysis/report.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace recomp {

namespace {

const char* end_name(BlockEnd k) {
    switch (k) {
        case BlockEnd::Fallthrough: return "fallthrough";
        case BlockEnd::Jump: return "jump";
        case BlockEnd::CondJump: return "condjump";
        case BlockEnd::TailCall: return "tailcall";
        case BlockEnd::JumpTable: return "jumptable";
        case BlockEnd::IndirectJump: return "indirect_jump";
        case BlockEnd::ImportJump: return "import_jump";
        case BlockEnd::Ret: return "ret";
        case BlockEnd::Stop: return "stop";
        case BlockEnd::NoReturnCall: return "noreturn_call";
        case BlockEnd::Invalid: return "invalid";
    }
    return "?";
}

template <typename K>
std::vector<std::pair<K, uint64_t>> sorted_desc(const std::map<K, uint64_t>& m) {
    std::vector<std::pair<K, uint64_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    return v;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string pct(uint64_t a, uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f%%", b ? 100.0 * double(a) / double(b) : 0.0);
    return buf;
}

}  // namespace

bool write_report(const PeImage& img, const Disasm& dis, const Discovery& disc,
                  const std::string& out_dir, std::string& error) {
    namespace fs = std::filesystem;
    fs::create_directories(out_dir);

    // Walk every unique instruction once.
    std::map<std::string, uint64_t> mnem, isa_ext, isa_set;
    std::map<BlockEnd, uint64_t> ends;
    std::map<std::string, uint64_t> import_sites;
    uint64_t unique_insns = 0, blocks = 0, indirect_calls = 0, vcalls = 0, fs_access = 0;
    std::vector<bool> seen(img.image_size(), false);
    for (auto& [fa, f] : disc.functions()) {
        for (auto& [s, b] : f.blocks) {
            ++blocks;
            ++ends[b.kind];
            for (uint32_t a = b.start; a < b.end;) {
                Insn in;
                if (!dis.decode(a, in)) break;
                const uint32_t idx = a - img.image_base();
                if (!seen[idx]) {
                    seen[idx] = true;
                    ++unique_insns;
                    ++mnem[ZydisMnemonicGetString(in.mnem())];
                    ++isa_ext[ZydisISAExtGetString(in.ins.meta.isa_ext)];
                    ++isa_set[ZydisISASetGetString(in.ins.meta.isa_set)];
                    for (int i = 0; i < in.nops(); ++i)
                        if (in.op(i).type == ZYDIS_OPERAND_TYPE_MEMORY && in.op(i).mem.segment == ZYDIS_REGISTER_FS)
                            ++fs_access;
                    const Flow fl = classify(in);
                    if (fl == Flow::Call || fl == Flow::Jump) {
                        if (auto m = in.abs_mem(0)) {
                            if (const Import* imp = img.import_at(*m)) {
                                std::string key = imp->dll + "!" +
                                    (imp->name.empty() ? "#" + std::to_string(imp->ordinal) : imp->name);
                                ++import_sites[key];
                            }
                        } else if (fl == Flow::Call && !in.branch_target()) {
                            ++indirect_calls;
                            if (in.op(0).type == ZYDIS_OPERAND_TYPE_MEMORY && in.op(0).mem.base != ZYDIS_REGISTER_NONE)
                                ++vcalls;
                        }
                    }
                }
                a = in.next();
            }
        }
    }

    uint64_t dead = 0;
    uint64_t no_ret = 0, indirect_jump_funcs = 0, invalid_funcs = 0, stdcall = 0;
    for (auto& [a, f] : disc.functions()) {
        if (disc.is_noreturn(a)) ++no_ret;
        if (f.dead) ++dead;
        if (f.has_indirect_jump) ++indirect_jump_funcs;
        if (f.has_invalid) ++invalid_funcs;
        if (f.ret_pop != 0xFFFFFFFF && f.ret_pop != 0xFFFFFFFE && f.ret_pop) ++stdcall;
    }
    uint64_t jt_entries = 0;
    for (auto& [a, jt] : disc.jump_tables()) jt_entries += jt.targets.size();
    const auto cov = disc.coverage();

    std::ofstream md(out_dir + "/analysis.md");
    if (!md) {
        error = "cannot write report in " + out_dir;
        return false;
    }
    md << "# dirt2_game.exe analysis\n\n";
    md << "| | |\n|---|---|\n";
    md << "| SHA-256 | `" << img.sha256() << "` |\n";
    md << "| Image base | " << hex(img.image_base()) << " |\n";
    md << "| Entry point | " << hex(img.entry_point()) << " |\n";
    md << "| Imports | " << img.imports().size() << " |\n";
    md << "| Relocations | " << img.relocations().size() << " |\n";
    md << "| Functions | " << disc.functions().size() << " |\n";
    md << "| &nbsp; seeded by calls / relocs / gap scan | " << disc.seeds_from_calls() << " / "
       << disc.seeds_from_relocs() << " / " << disc.seeds_from_gaps() << " |\n";
    md << "| &nbsp; unreferenced (dead code) | " << dead << " |\n";
    md << "| &nbsp; never return | " << no_ret << " |\n";
    md << "| &nbsp; callee-pops (stdcall/thiscall ret N) | " << stdcall << " |\n";
    md << "| &nbsp; with unresolved indirect jumps | " << indirect_jump_funcs << " |\n";
    md << "| &nbsp; reaching undecodable bytes | " << invalid_funcs << " |\n";
    md << "| Basic blocks | " << blocks << " |\n";
    md << "| Unique instructions | " << unique_insns << " |\n";
    md << "| Jump tables / entries | " << disc.jump_tables().size() << " / " << jt_entries << " |\n";
    md << "| Indirect calls (of which [reg+disp]) | " << indirect_calls << " (" << vcalls << ") |\n";
    md << "| FS-segment accesses | " << fs_access << " |\n\n";

    md << "## Code-section coverage\n\n| Kind | Bytes | Share |\n|---|---|---|\n";
    md << "| Code | " << cov.code << " | " << pct(cov.code, cov.total) << " |\n";
    md << "| Jump-table data | " << cov.table_data << " | " << pct(cov.table_data, cov.total) << " |\n";
    md << "| Padding | " << cov.padding << " | " << pct(cov.padding, cov.total) << " |\n";
    md << "| Unclaimed | " << cov.unknown << " | " << pct(cov.unknown, cov.total) << " |\n\n";

    auto gaps = disc.gaps(16);
    std::sort(gaps.begin(), gaps.end(), [](auto& a, auto& b) { return a.second - a.first > b.second - b.first; });
    md << "Largest unclaimed ranges (" << gaps.size() << " of 16+ bytes):\n\n";
    for (size_t i = 0; i < gaps.size() && i < 20; ++i)
        md << "- " << hex(gaps[i].first) << " - " << hex(gaps[i].second) << " (" << gaps[i].second - gaps[i].first << " bytes)\n";

    md << "\n## Block terminators\n\n| Kind | Count |\n|---|---|\n";
    for (auto& [k, n] : ends) md << "| " << end_name(k) << " | " << n << " |\n";

    md << "\n## ISA extensions\n\n| Extension | Instructions |\n|---|---|\n";
    for (auto& [k, n] : sorted_desc(isa_ext)) md << "| " << k << " | " << n << " |\n";

    md << "\n## ISA sets\n\n| Set | Instructions |\n|---|---|\n";
    for (auto& [k, n] : sorted_desc(isa_set)) md << "| " << k << " | " << n << " |\n";

    md << "\n## Mnemonics (" << mnem.size() << " distinct)\n\n| Mnemonic | Count |\n|---|---|\n";
    for (auto& [k, n] : sorted_desc(mnem)) md << "| " << k << " | " << n << " |\n";

    md << "\n## Import call sites\n\n| Import | Sites |\n|---|---|\n";
    for (auto& [k, n] : sorted_desc(import_sites)) md << "| " << k << " | " << n << " |\n";

    std::ofstream csv(out_dir + "/functions.csv");
    csv << "start,dead,blocks,insns,noreturn,ret_pop,indirect_jump,invalid,callees\n";
    for (auto& [a, f] : disc.functions()) {
        csv << hex(a) << ',' << f.dead << ',' << f.blocks.size() << ',' << f.insn_count << ',' << disc.is_noreturn(a) << ','
            << (f.ret_pop >= 0xFFFFFFFE ? -1 : int(f.ret_pop)) << ',' << f.has_indirect_jump << ','
            << f.has_invalid << ',' << f.callees.size() << '\n';
    }
    return true;
}

}  // namespace recomp
