#pragma once
#include <string>
#include <string_view>

namespace switch_platform {
// Modify only the known Steam v1.1 minimum-resolution rules. The private
// override leaves the installed game data and other platforms unchanged.
inline bool allow_half_resolution(std::string& xml) {
    const auto first = xml.find("<resolution>");
    const auto last = xml.find("</resolution>", first);
    if (first == std::string::npos || last == std::string::npos) return false;
    std::string rules = xml.substr(first, last - first);
    const std::string_view height = "<restrict_set name=\"height\" default=\"480\" />";
    const std::string_view limit = "<restrict_if name=\"height\" op=\"int_le\" value=\"479\">";
    auto count = [&](std::string_view token) {
        unsigned n = 0; size_t pos = 0;
        while ((pos = rules.find(token, pos)) != std::string::npos) { ++n; pos += token.size(); }
        return n;
    };
    if (count(height) != 2 || count(limit) != 1) return false;
    for (unsigned i = 0; i < 2; ++i)
        rules.replace(rules.find(height), height.size(), "<restrict_set name=\"height\" default=\"360\" />");
    rules.replace(rules.find(limit), limit.size(), "<restrict_if name=\"height\" op=\"int_le\" value=\"359\">");
    xml.replace(first, last - first, rules);
    return true;
}
}
