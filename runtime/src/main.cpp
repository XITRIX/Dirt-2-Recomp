// Host entry point: maps the game, then runs the guest CRT entry on a thread
// with a large host stack (guest call depth nests host frames).
#include <pthread.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "recomp/host.h"

namespace rt {
namespace {
std::string g_game_dir;
}
std::string game_dir() { return g_game_dir; }
void set_game_dir(const std::string& d) { g_game_dir = d; }
}  // namespace rt

namespace {

void* game_main(void*) {
    rt::install_crash_handler();  // per-thread alternate signal stack
    X86* c = rt::create_main_thread();
    rt::log("entering guest at 0x%08X", recomp_image_info.entry);
    const uint32_t r = rt::invoke(c, recomp_image_info.entry, {}, true);
    rt::log("guest entry returned %u", r);
    std::exit(int(r));
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir;
    if (const char* e = std::getenv("DIRT2_GAME_DIR")) dir = e;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--game")) dir = argv[++i];
    if (dir.empty()) {
        std::fprintf(stderr, "usage: %s --game <folder with dirt2_game.exe>\n", argv[0]);
        return 2;
    }
    rt::set_game_dir(dir);
    rt::install_crash_handler();
    if (!rt::mem_init()) return 1;
    std::string err;
    if (!rt::load_image(dir + "/dirt2_game.exe", err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (chdir(dir.c_str()) != 0) rt::log("warning: cannot chdir to %s", dir.c_str());

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_t th;
    pthread_create(&th, &attr, game_main, nullptr);
    pthread_join(th, nullptr);
    return 0;
}
