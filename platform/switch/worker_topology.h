#pragma once
#include <initializer_list>
#include <string>
#include <string_view>

namespace switch_platform {
// Use the engine's existing8-core job groups with a small worker pool. Solver
// and soft-body tasks must remain on disjoint workers (the shipped map's rule).
// Scheduling stays with Horizon; core attributes do not add host affinity.
inline std::string parallel_worker_map() {
    std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<workerMap>\n";
    auto thread = [&](const char* name, unsigned core, unsigned stack) {
        xml += " <workerThread name=\"" + std::string(name) + "\" priority=\"normal\" core=\"" +
               std::to_string(core) + "\" stacksize=\"" + std::to_string(stack) + "\" />\n";
    };
    thread("switch_update",0,131072);
    thread("switch_solver",1,131072);
    thread("switch_softbody",2,131072);
    thread("switch_gc_1",1,32768);
    thread("switch_gc_2",2,32768);
    auto group = [&](const char* name, std::initializer_list<const char*> workers) {
        xml += " <workerSet name=\"" + std::string(name) + "\" jobpoolsize=\"1024\">\n";
        for (const char* worker : workers) xml += "  <worker name=\"" + std::string(worker) + "\" />\n";
        xml += " </workerSet>\n";
    };
    for (const char* name : {"generic_workers","collision","integrator","cloth","handling","ai","pfx",
                            "visibility","ground_cover","anim","build_crowd","in_car_characters",
                            "view_manager","storage","osdUpdateSet","damageSkinning"})
        group(name,{"switch_solver","switch_softbody"});
    group("solver",{"switch_solver"});
    group("softbody",{"switch_softbody"});
    group("plugin_finalise",{"switch_solver"});
    group("pssg_storage",{"switch_solver"});
    group("audio",{"switch_softbody"});
    group("vehicleSingleThreaded",{"switch_update","switch_solver","switch_softbody"});
    group("ground_cover_workers",{"switch_gc_1","switch_gc_2"});
    xml += "</workerMap>\n";
    return xml;
}
}
