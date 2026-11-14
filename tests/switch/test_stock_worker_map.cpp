#include "platform/switch/stock_worker_map.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    // A pinned startup thread does not limit a process allowed six cores.
    constexpr uint64_t startup_thread_mask = 1, process_mask = 0x3F;
    assert(switch_platform::stock_parallel_process_allowed(0, process_mask));
    assert(!switch_platform::stock_parallel_process_allowed(0, startup_thread_mask));
    assert(!switch_platform::stock_parallel_process_allowed(0, 0x1F));
    assert(switch_platform::stock_parallel_process_allowed(0, 0xFC));
    assert(!switch_platform::stock_parallel_process_allowed(1, 0xFF));
    assert(!switch_platform::stock_parallel_process_allowed(0, 0));
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    assert(input);
    std::string xml((std::istreambuf_iterator<char>(input)), {});
    assert(switch_platform::known_stock_parallel_map(xml));
    std::string changed = xml;
    // A same-size edit must fail, including accidental worker reassignment.
    const auto solver = changed.find("<workerSet name=\"solver\"");
    assert(solver != std::string::npos);
    const auto worker = changed.find("name=\"core_1\"", solver);
    assert(worker != std::string::npos);
    changed[worker + 11] = '5';
    assert(!switch_platform::known_stock_parallel_map(changed));
    assert(!switch_platform::known_stock_parallel_map(xml.substr(0, xml.size()-1)));
    assert(!switch_platform::known_stock_parallel_map(""));
    std::puts("PASS: exact stock graph accepted; reassignment, truncation and missing input rejected");
}
