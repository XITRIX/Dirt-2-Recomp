#pragma once

#include <string>

#include "analysis/discovery.h"

namespace recomp {

// Writes a Markdown analysis report and a functions.csv next to it.
bool write_report(const PeImage& img, const Disasm& dis, const Discovery& disc,
                  const std::string& out_dir, std::string& error);

}  // namespace recomp
