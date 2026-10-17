// recomp: static recompiler for dirt2_game.exe.
//
//   recomp analyze <exe> [--out <dir>]
//   recomp lift <exe> [--out <dir>] [--include-dead] [--entries <file>]
//   recomp disasm <exe> <hex addr> [count]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "analysis/discovery.h"
#include "analysis/report.h"
#include "codegen/lifter.h"

using namespace recomp;

static int usage() {
    std::fprintf(stderr, "usage: recomp analyze|lift <exe> [--out <dir>] [--include-dead]\n       recomp disasm <exe> <hex addr> [count]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1], exe = argv[2];
    std::string out = "generated";
    bool include_dead = false;
    std::vector<std::string> entry_files;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--entries") && i + 1 < argc) entry_files.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--include-dead")) include_dead = true;
    }

    if (cmd != "analyze" && cmd != "lift" && cmd != "disasm") return usage();

    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    auto img = PeImage::load(exe, err);
    if (!img) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    if (cmd == "disasm") {
        // recomp disasm <exe> <addr> [count]: decoder and flow-classifier debugging.
        if (argc < 4) return usage();
        uint32_t a = uint32_t(std::stoul(argv[3], nullptr, 16));
        const int count = argc > 4 ? std::atoi(argv[4]) : 20;
        Disasm dis(*img);
        static const char* flows[] = {"", "call", "jump", "cjump", "ret", "stop"};
        for (int i = 0; i < count; ++i) {
            Insn in;
            if (!dis.decode(a, in)) {
                std::printf("%08X  <invalid>\n", a);
                break;
            }
            char buf[256];
            dis.format(in, buf, sizeof buf);
            std::printf("%08X  %-48s %s\n", a, buf, flows[int(classify(in))]);
            a = in.next();
        }
        return 0;
    }
    std::printf("loaded %s: %zu sections, %zu imports, %zu relocations\n", exe.c_str(),
                img->sections().size(), img->imports().size(), img->relocations().size());

    Disasm dis(*img);
    // Extra entry points: one hex address per line (from recomp_discovered.txt).
    DiscoveryConfig dcfg;
    for (const std::string& path : entry_files) {
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            char buf[64];
            while (std::fgets(buf, sizeof buf, f))
                if (buf[0] != '#' && std::strlen(buf) > 1) dcfg.extra_entries.push_back(uint32_t(std::strtoul(buf, nullptr, 16)));
            std::fclose(f);
        }
    }
    if (!dcfg.extra_entries.empty()) std::printf("%zu extra entry points\n", dcfg.extra_entries.size());
    Discovery disc(*img, dis, dcfg);
    disc.run();
    const auto cov = disc.coverage();
    std::printf("functions: %zu, jump tables: %zu, code coverage %.2f%% (unclaimed %llu bytes)\n",
                disc.functions().size(), disc.jump_tables().size(),
                100.0 * double(cov.code + cov.table_data + cov.padding) / double(cov.total),
                (unsigned long long)cov.unknown);

    // RECOMP_PROBE=addr,addr: show which blocks end at or contain each address.
    if (const char* probe = std::getenv("RECOMP_PROBE")) {
        std::string s = probe;
        size_t pos = 0;
        while (pos < s.size()) {
            const size_t comma = s.find(',', pos);
            const uint32_t p = uint32_t(std::stoul(s.substr(pos, comma - pos), nullptr, 16));
            pos = comma == std::string::npos ? s.size() : comma + 1;
            for (auto& [fa, f] : disc.functions())
                for (auto& [ba, b] : f.blocks)
                    if ((p >= b.start && p < b.end) || b.end == p)
                        std::printf("probe %08X: fn %08X block %08X-%08X kind %d succ0 %08X\n", p, fa, b.start,
                                    b.end, int(b.kind), b.succ.empty() ? 0 : b.succ[0]);
        }
    }

    if (!write_report(*img, dis, disc, out, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("report written to %s/analysis.md\n", out.c_str());

    if (cmd == "lift") {
        LiftOptions lo;
        lo.out_dir = out;
        lo.include_dead = include_dead;
        LiftStats ls;
        if (!lift_program(*img, dis, disc, lo, ls, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("lifted %zu functions, %zu instructions into %zu files\n", ls.functions, ls.instructions, ls.files);
        std::printf("unimplemented sites: %zu, functions with live-in flags: %zu, SEH landing pads: %zu\n", ls.unimplemented, ls.live_in_flags, ls.resumable);
        std::vector<std::pair<std::string, size_t>> v(ls.unimpl_by_mnemonic.begin(), ls.unimpl_by_mnemonic.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 40; ++i) std::printf("  %-12s %zu\n", v[i].first.c_str(), v[i].second);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("done in %.1fs\n", secs);
    return 0;
}
