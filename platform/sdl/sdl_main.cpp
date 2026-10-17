// Host entry point (SDL3). The main thread owns SDL: it pumps events and runs
// window operations; the guest runs on its own thread with a large host stack,
// since guest call depth nests host frames.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "../platform.h"
#include "recomp/host.h"

namespace shim {
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
}

namespace rt {
namespace {
std::string g_game_dir;
}
std::string game_dir() { return g_game_dir; }
void set_game_dir(const std::string& d) { g_game_dir = d; }
}  // namespace rt

namespace platform {

namespace {

template <typename F>
void on_main(F&& f) {
    if (SDL_IsMainThread()) {
        f();
        return;
    }
    auto call = [](void* p) { (*static_cast<F*>(p))(); };
    SDL_RunOnMainThread(call, &f, true);
}

}  // namespace

Window create_window(int width, int height, const char* title) {
    SDL_Window* w = nullptr;
    on_main([&] {
        w = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        if (!w) rt::log("SDL_CreateWindow failed: %s", SDL_GetError());
    });
    return w;
}

void destroy_window(Window w) {
    on_main([&] { SDL_DestroyWindow(static_cast<SDL_Window*>(w)); });
}

void set_window_size(Window w, int width, int height) {
    on_main([&] { SDL_SetWindowSize(static_cast<SDL_Window*>(w), width, height); });
}

void show_window(Window w, bool show) {
    on_main([&] {
        if (show) SDL_ShowWindow(static_cast<SDL_Window*>(w));
        else SDL_HideWindow(static_cast<SDL_Window*>(w));
    });
}

void set_fullscreen(Window w, bool fullscreen) {
    on_main([&] {
        SDL_SetWindowFullscreenMode(static_cast<SDL_Window*>(w), nullptr);
        SDL_SetWindowFullscreen(static_cast<SDL_Window*>(w), fullscreen);
    });
}

void drawable_size(Window w, int* width, int* height) {
    on_main([&] { SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(w), width, height); });
}

void display_size(int* width, int* height) {
    on_main([&] {
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        *width = m ? m->w : 1920;
        *height = m ? m->h : 1080;
    });
}

}  // namespace platform

namespace {

std::atomic<bool> g_guest_done{false};

void* game_main(void*) {
    rt::install_crash_handler();
    X86* c = rt::create_main_thread();
    rt::log("entering guest at 0x%08X", recomp_image_info.entry);
    const uint32_t r = rt::invoke(c, recomp_image_info.entry, {}, true);
    rt::log("guest entry returned %u", r);
    g_guest_done = true;
    std::exit(int(r));
}

constexpr uint32_t WM_CLOSE = 0x10, WM_ACTIVATEAPP = 0x1C, WM_ACTIVATE = 0x06;

void translate(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
            shim::post_message(0, WM_CLOSE, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            shim::post_message(0, WM_ACTIVATEAPP, 1, 0);
            shim::post_message(0, WM_ACTIVATE, 1, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            shim::post_message(0, WM_ACTIVATE, 0, 0);
            shim::post_message(0, WM_ACTIVATEAPP, 0, 0);
            break;
        default:
            break;
    }
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

    // DXVK native needs to know its window system. Its log and caches, and our
    // discovery log, go to the data directory: the process chdirs into the game
    // folder, which must stay untouched.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    {
        std::string data;
        if (const char* e = std::getenv("DIRT2_DATA_DIR")) data = e;
        else if (const char* h = std::getenv("HOME")) data = std::string(h) + "/Library/Application Support/Dirt2Recomp";
        else data = ".";
        std::error_code ec;
        std::filesystem::create_directories(data, ec);
        setenv("DXVK_LOG_PATH", data.c_str(), 0);
        setenv("DXVK_STATE_CACHE_PATH", data.c_str(), 0);
        rt::set_discovery_log(data + "/recomp_discovered.txt");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

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

    // Main thread: pump events (this also runs SDL_RunOnMainThread callbacks).
    while (!g_guest_done) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 10)) {
            translate(e);
            while (SDL_PollEvent(&e)) translate(e);
        }
    }
    return 0;
}
