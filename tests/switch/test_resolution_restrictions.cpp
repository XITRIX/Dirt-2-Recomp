#include "platform/switch/resolution_restrictions.h"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream f(argv[1], std::ios::binary);
    assert(f);
    const std::string original((std::istreambuf_iterator<char>(f)), {});
    auto patched = original;
    assert(switch_platform::allow_half_resolution(patched));
    // Exactly three equal-length edits; no other settings or original asset change.
    auto restored = patched;
    auto replace_all = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = restored.find(from, pos)) != std::string::npos) { restored.replace(pos, from.size(), to); pos += to.size(); }
    };
    replace_all("default=\"360\"", "default=\"480\"");
    replace_all("value=\"359\"", "value=\"479\"");
    assert(restored == original);
    const auto before = patched;
    assert(!switch_platform::allow_half_resolution(patched));
    assert(patched == before);
    for (std::string broken : {std::string{}, original.substr(0, original.find("</resolution>")), std::string("<resolution></resolution>")}) {
        const auto saved = broken;
        assert(!switch_platform::allow_half_resolution(broken));
        assert(broken == saved);
    }
}
