#include "analysis/discovery.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_map>

namespace recomp {

namespace {

enum : uint8_t { kUnknown = 0, kCode = 1, kTable = 2, kPadding = 3 };

struct Node {
    uint8_t len = 0;
    bool term = false;
    BlockEnd kind = BlockEnd::Fallthrough;
    std::vector<uint32_t> succ;
};

bool is_ja_like(ZydisMnemonic m) { return m == ZYDIS_MNEMONIC_JNBE || m == ZYDIS_MNEMONIC_JNB; }

}  // namespace

Discovery::Discovery(const PeImage& img, const Disasm& dis, DiscoveryConfig cfg)
    : img_(img), dis_(dis), cfg_(std::move(cfg)) {
    text_lo_ = 0xFFFFFFFF;
    for (const Section& s : img_.sections())
        if (s.executable()) {
            text_lo_ = std::min(text_lo_, s.va);
            text_hi_ = std::max(text_hi_, s.va + s.vsize);
        }
    if (text_lo_ > text_hi_) text_lo_ = text_hi_ = 0;
    cover_.assign(text_hi_ - text_lo_, kUnknown);
}

bool Discovery::call_is_noreturn(const Insn& in) const {
    if (auto t = in.branch_target()) return noreturn_funcs_.count(*t) != 0;
    if (auto m = in.abs_mem(0))
        if (const Import* imp = img_.import_at(*m)) return cfg_.noreturn_imports.count(imp->name) != 0;
    return false;
}

bool Discovery::resolve_jump_table(const Insn& in, JumpTable& jt) const {
    if (in.nops() < 1) return false;
    const ZydisDecodedOperand& o = in.op(0);
    if (o.type != ZYDIS_OPERAND_TYPE_MEMORY || o.mem.base != ZYDIS_REGISTER_NONE ||
        o.mem.index == ZYDIS_REGISTER_NONE || o.mem.scale != 4)
        return false;
    const uint32_t table = uint32_t(o.mem.disp.value);
    if (!img_.in_image(table, 4)) return false;
    jt.jump_addr = in.addr;
    jt.table = table;
    for (uint32_t k = 0; k < 4096; ++k) {
        const uint32_t slot = table + k * 4;
        if (!img_.has_reloc(slot)) break;
        const uint32_t t = img_.read32(slot);
        if (!img_.is_code(t)) break;
        jt.targets.push_back(t);
    }
    return !jt.targets.empty();
}

Function Discovery::build(uint32_t start) {
    Function f;
    f.start = start;
    f.dead = gap_phase_;
    if (auto it = funcs_.find(start); it != funcs_.end()) f.dead = it->second.dead;
    std::unordered_map<uint32_t, Node> nodes;
    std::set<uint32_t> leaders{start};
    std::deque<uint32_t> work{start};

    while (!work.empty()) {
        uint32_t a = work.front();
        work.pop_front();
        // Recent instructions in this linear run, for jump-table bounds.
        Insn hist[6];
        int nhist = 0;
        for (;;) {
            if (nodes.count(a)) {
                leaders.insert(a);
                break;
            }
            Node n;
            if (a != start && known_starts_.count(a)) {
                // Fell through into another function: model as a tail call.
                leaders.insert(a);
                break;
            }
            Insn in;
            if (!img_.is_code(a) || !dis_.decode(a, in)) {
                n.len = 1;
                n.term = true;
                n.kind = BlockEnd::Invalid;
                f.has_invalid = true;
                nodes[a] = n;
                break;
            }
            n.len = in.ins.length;
            ++f.insn_count;
            const uint32_t next = in.next();
            switch (classify(in)) {
                case Flow::Normal: break;
                case Flow::Call:
                    if (auto t = in.branch_target(); t && img_.is_code(*t)) f.callees.insert(*t);
                    if (call_is_noreturn(in)) {
                        n.term = true;
                        n.kind = BlockEnd::NoReturnCall;
                    }
                    break;
                case Flow::Ret:
                    n.term = true;
                    n.kind = BlockEnd::Ret;
                    f.returns = true;
                    {
                        const uint32_t pop = in.nops() ? uint32_t(in.op(0).imm.value.u) : 0;
                        if (f.ret_pop == 0xFFFFFFFF) f.ret_pop = pop;
                        else if (f.ret_pop != pop) f.ret_pop = 0xFFFFFFFE;  // inconsistent
                    }
                    break;
                case Flow::Stop:
                    n.term = true;
                    n.kind = BlockEnd::Stop;
                    break;
                case Flow::CondJump:
                    if (auto t = in.branch_target()) {
                        n.succ.push_back(*t);
                        if (img_.is_code(*t) && !(*t != start && known_starts_.count(*t))) {
                            leaders.insert(*t);
                            work.push_back(*t);
                        }
                    }
                    n.term = true;
                    n.kind = BlockEnd::CondJump;
                    n.succ.push_back(next);
                    leaders.insert(next);
                    work.push_front(next);
                    break;
                case Flow::Jump:
                    n.term = true;
                    if (auto t = in.branch_target()) {
                        n.succ.push_back(*t);
                        if (*t != start && known_starts_.count(*t)) {
                            n.kind = BlockEnd::TailCall;
                        } else {
                            n.kind = BlockEnd::Jump;
                            leaders.insert(*t);
                            work.push_back(*t);
                        }
                    } else if (auto m = in.abs_mem(0); m && img_.import_at(*m)) {
                        n.kind = BlockEnd::ImportJump;
                    } else {
                        JumpTable jt;
                        if (resolve_jump_table(in, jt)) {
                            // Bound the table by a preceding "cmp r, imm; ja/jae".
                            uint32_t bound = 0;
                            uint32_t byte_table = 0;
                            for (int h = nhist - 1; h >= 0; --h) {
                                const Insn& p = hist[h];
                                if (p.mnem() == ZYDIS_MNEMONIC_MOVZX && p.nops() == 2 &&
                                    p.op(1).type == ZYDIS_OPERAND_TYPE_MEMORY && p.op(1).size == 8 &&
                                    p.op(1).mem.base != ZYDIS_REGISTER_NONE &&
                                    p.op(1).mem.index == ZYDIS_REGISTER_NONE)
                                    byte_table = uint32_t(p.op(1).mem.disp.value);
                                if (p.mnem() == ZYDIS_MNEMONIC_CMP && p.nops() == 2 &&
                                    p.op(1).type == ZYDIS_OPERAND_TYPE_IMMEDIATE && h + 1 < nhist &&
                                    is_ja_like(hist[h + 1].mnem())) {
                                    const uint32_t imm = uint32_t(p.op(1).imm.value.u);
                                    bound = hist[h + 1].mnem() == ZYDIS_MNEMONIC_JNBE ? imm + 1 : imm;
                                    break;
                                }
                            }
                            if (bound && byte_table && img_.in_image(byte_table, bound)) {
                                uint32_t maxidx = 0;
                                for (uint32_t k = 0; k < bound; ++k)
                                    maxidx = std::max<uint32_t>(maxidx, img_.read8(byte_table + k));
                                for (uint32_t k = 0; k < bound; ++k) {
                                    const uint32_t ba = byte_table + k;
                                    if (img_.is_code(ba)) cover_[ba - text_lo_] = kTable;
                                }
                                bound = maxidx + 1;
                            }
                            if (bound && bound < jt.targets.size()) jt.targets.resize(bound);
                            for (uint32_t k = 0; k < jt.targets.size(); ++k) {
                                const uint32_t slot = jt.table + k * 4;
                                if (img_.is_code(slot))
                                    for (int b = 0; b < 4; ++b) cover_[slot + b - text_lo_] = kTable;
                            }
                            n.kind = BlockEnd::JumpTable;
                            for (uint32_t t : jt.targets) {
                                n.succ.push_back(t);
                                leaders.insert(t);
                                work.push_back(t);
                            }
                            tables_[in.addr] = std::move(jt);
                        } else {
                            n.kind = BlockEnd::IndirectJump;
                            f.has_indirect_jump = true;
                        }
                    }
                    break;
            }
            nodes[a] = std::move(n);
            if (nodes[a].term) break;
            if (nhist == 6) {
                std::move(hist + 1, hist + 6, hist);
                nhist = 5;
            }
            hist[nhist++] = in;
            a = next;
        }
    }

    // Form blocks from the explored instructions.
    std::vector<uint32_t> addrs;
    addrs.reserve(nodes.size());
    for (auto& [a, n] : nodes) addrs.push_back(a);
    std::sort(addrs.begin(), addrs.end());
    Block cur;
    bool open = false;
    for (size_t i = 0; i < addrs.size(); ++i) {
        const uint32_t a = addrs[i];
        const Node& n = nodes[a];
        if (open && (leaders.count(a) || cur.end != a)) {
            // Close the previous block by fallthrough into this leader or a gap.
            cur.kind = BlockEnd::Fallthrough;
            cur.succ = {cur.end};
            f.blocks[cur.start] = cur;
            open = false;
        }
        if (!open) {
            cur = Block{};
            cur.start = a;
            open = true;
        }
        cur.end = a + n.len;
        if (n.term) {
            cur.kind = n.kind;
            cur.succ = n.succ;
            f.blocks[cur.start] = cur;
            open = false;
        }
    }
    if (open) {
        cur.kind = BlockEnd::Fallthrough;
        cur.succ = {cur.end};
        f.blocks[cur.start] = cur;
    }
    // A fallthrough into another function's entry is a tail call.
    for (auto& [s, b] : f.blocks)
        if (b.kind == BlockEnd::Fallthrough && !f.blocks.count(b.end)) b.kind = BlockEnd::TailCall;
    return f;
}

void Discovery::compute_noreturn() {
    std::set<uint32_t> nr;
    for (auto& [a, f] : funcs_) nr.insert(a);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = nr.begin(); it != nr.end();) {
            const Function& f = funcs_.at(*it);
            bool ret = f.returns || f.has_indirect_jump || f.has_invalid;
            for (auto& [s, b] : f.blocks) {
                if (ret) break;
                if (b.kind == BlockEnd::ImportJump) ret = true;
                // Any edge leaving the function is a tail call into another entry.
                for (uint32_t t : b.succ)
                    if (!f.blocks.count(t) && !nr.count(t)) ret = true;
                if (b.kind == BlockEnd::TailCall && b.succ.empty()) ret = true;
            }
            if (ret) {
                it = nr.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    noreturn_funcs_ = std::move(nr);
}

void Discovery::mark_coverage() {
    for (uint8_t& c : cover_)
        if (c == kCode || c == kPadding) c = kUnknown;
    for (auto& [a, f] : funcs_)
        for (auto& [s, b] : f.blocks)
            for (uint32_t x = b.start; x < b.end && x < text_hi_; ++x)
                if (x >= text_lo_ && cover_[x - text_lo_] != kTable) cover_[x - text_lo_] = kCode;
    for (uint32_t x = text_lo_; x < text_hi_; ++x) {
        uint8_t& c = cover_[x - text_lo_];
        if (c != kUnknown) continue;
        const uint8_t v = img_.read8(x);
        if (v == 0xCC || v == 0x90) c = kPadding;
    }
}

uint32_t Discovery::scan_gaps_for_functions() {
    // Unreferenced (dead) functions: MSVC lays functions out on 16-byte
    // boundaries separated by int3 padding, so a gap that starts aligned or
    // right after int3 padding is taken as a function entry if it decodes
    // cleanly. New callees are drained normally, and rounds repeat until no
    // more functions are found.
    uint32_t added = 0;
    for (int round = 0; round < 1; ++round) {
        uint32_t round_added = 0;
        std::deque<uint32_t> queue;
        for (auto [lo, hi] : gaps(1)) {
            // Gap boundaries: aligned, after int3 padding, or directly after a
            // terminated block (hand-written CRT code is packed without padding).
            const bool after_int3 = lo > text_lo_ && img_.read8(lo - 1) == 0xCC;
            const bool after_code = lo > text_lo_ && cover_[lo - 1 - text_lo_] == kCode;
            if ((lo & 0xF) && !after_int3 && !after_code) continue;
            known_starts_.insert(lo);
            Function f = build(lo);
            if (f.has_invalid || f.insn_count < 2) {
                known_starts_.erase(lo);
                continue;
            }
            for (uint32_t cal : f.callees)
                if (known_starts_.insert(cal).second) queue.push_back(cal);
            funcs_[lo] = std::move(f);
            ++round_added;
        }
        while (!queue.empty()) {
            const uint32_t a = queue.front();
            queue.pop_front();
            Function f = build(a);
            for (uint32_t cal : f.callees)
                if (known_starts_.insert(cal).second) queue.push_back(cal);
            funcs_[a] = std::move(f);
            ++round_added;
        }
        mark_coverage();
        added += round_added;
        if (!round_added) break;
    }
    return added;
}

void Discovery::run() {
    std::deque<uint32_t> queue;
    auto seed = [&](uint32_t a) {
        if (img_.is_code(a) && known_starts_.insert(a).second) queue.push_back(a);
    };
    seed(img_.entry_point());
    for (uint32_t a : cfg_.extra_entries) seed(a);
    if (const auto& tls = img_.tls(); tls && tls->callbacks)
        for (uint32_t p = tls->callbacks; img_.in_image(p, 4) && img_.read32(p); p += 4) seed(img_.read32(p));

    auto drain = [&]() {
        while (!queue.empty()) {
            const uint32_t a = queue.front();
            queue.pop_front();
            Function f = build(a);
            for (uint32_t c : f.callees)
                if (known_starts_.insert(c).second) {
                    queue.push_back(c);
                    ++seeds_calls_;
                }
            funcs_[a] = std::move(f);
        }
    };

    // Relocations are processed once each. Those inside code are only used
    // once the containing instruction has been decoded, so jump-table bases
    // and case labels in not-yet-discovered code are never mistaken for entries.
    std::vector<bool> reloc_done(img_.relocations().size(), false);
    auto reloc_pass = [&]() {
        const auto& relocs = img_.relocations();
        for (size_t i = 0; i < relocs.size(); ++i) {
            if (reloc_done[i]) continue;
            const uint32_t r = relocs[i];
            if (img_.is_code(r)) {
                const uint8_t c = cover_[r - text_lo_];
                if (c == kTable) {
                    reloc_done[i] = true;
                    continue;
                }
                if (c != kCode) continue;
            }
            reloc_done[i] = true;
            const uint32_t v = img_.read32(r);
            if (!img_.is_code(v) || cover_[v - text_lo_] == kTable) continue;
            if (known_starts_.insert(v).second) {
                queue.push_back(v);
                ++seeds_relocs_;
            }
        }
    };
    auto rebuild_all = [&]() {
        compute_noreturn();
        for (auto& [a, f] : funcs_) f = build(a);
        for (auto& [a, f] : funcs_)
            for (uint32_t c : f.callees)
                if (known_starts_.insert(c).second) queue.push_back(c);
        drain();
        mark_coverage();
    };

    const bool gap_scan = !std::getenv("RECOMP_NO_GAPSCAN");
    for (int iter = 0; iter < 32; ++iter) {
        const size_t before = known_starts_.size();
        drain();
        mark_coverage();
        reloc_pass();
        drain();
        rebuild_all();
        if (known_starts_.size() != before) continue;
        if (!gap_scan) break;
        gap_phase_ = true;
        const uint32_t g = scan_gaps_for_functions();
        seeds_gaps_ += g;
        if (!g) break;
    }
    compute_noreturn();
    mark_coverage();
}

Discovery::Coverage Discovery::coverage() const {
    Coverage c;
    c.total = cover_.size();
    for (uint8_t v : cover_) {
        if (v == kCode) ++c.code;
        else if (v == kTable) ++c.table_data;
        else if (v == kPadding) ++c.padding;
        else ++c.unknown;
    }
    return c;
}

std::vector<std::pair<uint32_t, uint32_t>> Discovery::gaps(size_t min_len) const {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    uint32_t i = 0;
    const uint32_t n = uint32_t(cover_.size());
    while (i < n) {
        if (cover_[i] != kUnknown) {
            ++i;
            continue;
        }
        uint32_t j = i;
        while (j < n && cover_[j] == kUnknown) ++j;
        if (j - i >= min_len) out.emplace_back(text_lo_ + i, text_lo_ + j);
        i = j;
    }
    return out;
}

}  // namespace recomp
