#include <cassert>
#include <fstream>
#include <vector>
#include <iterator>
#include "../../platform/switch/preflight_pixels.h"
int main(int argc, char** argv) {
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        assert(bytes.size() == 16*16*4);
        for (unsigned i = 0; i < bytes.size(); i += 4)
            assert(switch_platform::preflight_rgba8_matches(bytes.data()+i));
    }
    uint8_t p[] = {64,127,191,255};
    assert(switch_platform::preflight_rgba8_matches(p));
    p[1]=128; assert(switch_platform::preflight_rgba8_matches(p));
    p[0]=63; assert(!switch_platform::preflight_rgba8_matches(p));
    p[0]=64; p[2]=192; assert(!switch_platform::preflight_rgba8_matches(p));
    p[2]=191; p[1]=126; assert(!switch_platform::preflight_rgba8_matches(p));
    p[1]=127; p[3]=254; assert(!switch_platform::preflight_rgba8_matches(p));
}
