// Translates discovered functions to C against runtime/include/recomp/x86.h.
#pragma once

#include <cstddef>
#include <map>
#include <string>

#include "analysis/discovery.h"

namespace recomp {

struct LiftOptions {
    std::string out_dir = "generated";
    bool include_dead = false;     // lift unreferenced (gap-scanned) functions too
    size_t funcs_per_file = 300;
};

struct LiftStats {
    size_t functions = 0, instructions = 0, files = 0;
    size_t unimplemented = 0;      // instruction sites emitted as recomp_unimpl
    size_t live_in_flags = 0;      // functions reading flags they never set
    size_t resumable = 0;          // functions with SEH landing pads
    std::map<std::string, size_t> unimpl_by_mnemonic;
};

bool lift_program(const PeImage& img, const Disasm& dis, const Discovery& disc, const LiftOptions& opt,
                  LiftStats& stats, std::string& error);

}  // namespace recomp
