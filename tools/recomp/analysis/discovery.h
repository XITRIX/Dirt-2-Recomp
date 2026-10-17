// Function discovery and control-flow recovery over the whole executable.
//
// Seeds: entry point, TLS callbacks, config entry points, direct call targets,
// relocation targets landing in code (vtables, callbacks, function pointers).
// Bodies are recovered by recursive descent; MSVC jump tables are bounded by
// the relocation table, which marks every entry as a fixup.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "analysis/disasm.h"

namespace recomp {

enum class BlockEnd : uint8_t {
    Fallthrough,   // split because the next address starts another block
    Jump,          // direct jmp inside the function
    CondJump,
    TailCall,      // direct jmp to another function's entry
    JumpTable,
    IndirectJump,  // unresolved jmp reg/[mem]
    ImportJump,    // jmp [iat]
    Ret,
    Stop,          // int3 / hlt / ud2
    NoReturnCall,  // call that never returns
    Invalid,       // undecodable bytes reached
};

struct Block {
    uint32_t start = 0, end = 0;  // [start, end)
    BlockEnd kind = BlockEnd::Fallthrough;
    std::vector<uint32_t> succ;
};

struct JumpTable {
    uint32_t jump_addr = 0;   // the jmp instruction
    uint32_t table = 0;       // address of the first entry
    bool displaced = false;   // first entry is not at the instruction's displacement
    std::vector<uint32_t> targets;
};

struct Function {
    uint32_t start = 0;
    std::map<uint32_t, Block> blocks;
    std::set<uint32_t> callees;
    uint32_t insn_count = 0;
    uint32_t ret_pop = 0xFFFFFFFF;  // bytes popped by ret imm16, if consistent
    bool returns = false;
    bool has_indirect_jump = false;
    bool has_invalid = false;
    bool dead = false;  // only reachable from gap-scanned (unreferenced) code
};

struct DiscoveryConfig {
    std::vector<uint32_t> extra_entries;
    std::set<std::string> noreturn_imports = {
        "ExitProcess", "ExitThread", "TerminateProcess", "FatalAppExitA", "FatalExit",
        "abort", "exit", "_exit"};
};

class Discovery {
public:
    Discovery(const PeImage& img, const Disasm& dis, DiscoveryConfig cfg);
    void run();

    const std::map<uint32_t, Function>& functions() const { return funcs_; }
    const std::map<uint32_t, JumpTable>& jump_tables() const { return tables_; }
    bool is_noreturn(uint32_t f) const { return noreturn_funcs_.count(f) != 0; }
    // Bytes of executable sections covered by code, jump-table data, or padding.
    struct Coverage { uint64_t total = 0, code = 0, table_data = 0, padding = 0, unknown = 0; };
    Coverage coverage() const;
    // Unclaimed byte ranges in code sections, for the report.
    std::vector<std::pair<uint32_t, uint32_t>> gaps(size_t min_len) const;

    uint32_t seeds_from_calls() const { return seeds_calls_; }
    uint32_t seeds_from_relocs() const { return seeds_relocs_; }
    uint32_t seeds_from_gaps() const { return seeds_gaps_; }

private:
    Function build(uint32_t start);
    bool call_is_noreturn(const Insn& in) const;
    bool resolve_jump_table(const Insn& in, JumpTable& jt) const;
    void compute_noreturn();
    void mark_coverage();
    uint32_t scan_gaps_for_functions();

    const PeImage& img_;
    const Disasm& dis_;
    DiscoveryConfig cfg_;
    std::map<uint32_t, Function> funcs_;
    std::map<uint32_t, JumpTable> tables_;
    std::set<uint32_t> noreturn_funcs_;
    std::set<uint32_t> known_starts_;
    // Per-byte ownership over executable sections: 0 unknown, 1 code, 2 table, 3 padding.
    std::vector<uint8_t> cover_;
    uint32_t text_lo_ = 0, text_hi_ = 0;
    bool gap_phase_ = false;
    uint32_t seeds_calls_ = 0, seeds_relocs_ = 0, seeds_gaps_ = 0;
};

}  // namespace recomp
