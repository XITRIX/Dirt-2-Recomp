// Host entry point (SDL3). The main thread owns SDL: it pumps events and runs
// window operations; the guest runs on its own thread with a large host stack,
// since guest call depth nests host frames.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>
#if defined(DIRT2_SWITCH_DXVK)
#include <switch.h>
#endif
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <thread>

#include "../platform.h"
#include "recomp/host.h"
#include "../../shims/vfs.h"

namespace shim {
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void install_resolution_labels();
void install_menu_patches(bool metalfx_row);
int read_metalfx_setting(const std::string& path);
}

#if defined(SDL_PLATFORM_SWITCH)
namespace switch_platform {
bool check_launch_mode();
void install_resolution_restrictions();
void report_missing_game(const std::string& dir);
}  // namespace switch_platform
#endif

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

#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS) || defined(SDL_PLATFORM_VISIONOS) || \
    defined(SDL_PLATFORM_SWITCH)
// One window per screen: every top-level game window (the loading splash, then
// the main window) shares it; it goes away with the last one.
constexpr bool kSingleWindow = true;
#else
constexpr bool kSingleWindow = false;
#endif
static SDL_Window* g_shared_window = nullptr;
static int g_shared_refs = 0;

// The game sizes windows in pixels; SDL sizes them in points (HiDPI).
float display_density() {
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return m && m->pixel_density > 0 ? m->pixel_density : 1.0f;
}

Window create_window(int width, int height, const char* title) {
    SDL_Window* w = nullptr;
    on_main([&] {
        if (kSingleWindow && g_shared_window) {
            ++g_shared_refs;
            w = g_shared_window;
            return;
        }
        const float d = display_density();
#if defined(SDL_PLATFORM_SWITCH)
#if defined(DIRT2_SWITCH_DXVK)
        const SDL_WindowFlags api = SDL_WINDOW_VULKAN;
#else
        // deko3d presents to the native window itself (SDL_SWITCH_NATIVE_WINDOW, set in main).
        const SDL_WindowFlags api = 0;
#endif
#else
        const SDL_WindowFlags api = renderer() == Renderer::DX11 ? SDL_WINDOW_METAL : SDL_WINDOW_VULKAN;
#endif
#if defined(DIRT2_SWITCH_DXVK)
        // The loading splash is only 256x256. Every guest HWND shares this
        // window, and Vulkan's surface extent follows its initial NWindow size.
        // Keep physical output at 720p; guest render scales use swapchain buffers.
        constexpr int output_width = 1280, output_height = 720;
        rt::log("Switch Vulkan output: %dx%d (guest window request %dx%d)", output_width, output_height, width, height);
        w = SDL_CreateWindow(title, int(output_width / d), int(output_height / d),
#else
        w = SDL_CreateWindow(title, int(width / d), int(height / d),
#endif
                             api | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        if (!w) rt::log("SDL_CreateWindow failed: %s", SDL_GetError());
        if (kSingleWindow && w) {
            g_shared_window = w;
            g_shared_refs = 1;
        }
    });
    return w;
}

Renderer renderer() {
    static const Renderer r = [] {
        const char* e = std::getenv("DIRT2_RENDERER");
#ifdef DIRT2_DEFAULT_DX11
        return e && std::strcmp(e, "dx9") == 0 ? Renderer::DX9 : Renderer::DX11;
#else
        return e && std::strcmp(e, "dx11") == 0 ? Renderer::DX11 : Renderer::DX9;
#endif
    }();
    return r;
}

void* metal_layer(Window w) {
    static std::mutex mu;
    static std::unordered_map<Window, SDL_MetalView> views;
    std::lock_guard lk(mu);
    SDL_MetalView& v = views[w];
    void* layer = nullptr;
    on_main([&] {
        if (!v) v = SDL_Metal_CreateView(static_cast<SDL_Window*>(w));
        if (!v) rt::log("SDL_Metal_CreateView failed: %s", SDL_GetError());
        else layer = SDL_Metal_GetLayer(v);
    });
    return layer;
}

void destroy_window(Window w) {
    on_main([&] {
        if (kSingleWindow && w == g_shared_window) {
            if (--g_shared_refs > 0) return;
            g_shared_window = nullptr;
        }
        SDL_DestroyWindow(static_cast<SDL_Window*>(w));
    });
}

void set_window_size(Window w, int width, int height) {
    on_main([&] {
        const float d = display_density();
#if defined(DIRT2_SWITCH_DXVK)
        // Helper/splash HWND sizes must not resize the shared physical output.
        SDL_SetWindowSize(static_cast<SDL_Window*>(w), int(1280 / d), int(720 / d));
#else
        SDL_SetWindowSize(static_cast<SDL_Window*>(w), int(width / d), int(height / d));
#endif
    });
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

namespace {
std::mutex g_bg_mu;
std::condition_variable g_bg_cv;
bool g_background = false, g_parked = false;
bool g_paused = false;  // set_game_paused: a dialog the game must wait for
}  // namespace

void gpu_checkpoint() {
    std::unique_lock lk(g_bg_mu);
    if (!g_background && !g_paused) return;
    rt::log("platform: render thread paused (%s)", g_background ? "background" : "dialog");
    g_parked = true;
    g_bg_cv.notify_all();
    g_bg_cv.wait(lk, [] { return !g_background && !g_paused; });
    g_parked = false;
    rt::log("platform: render thread resumed");
}

void set_game_paused(bool paused) {
    {
        std::lock_guard lk(g_bg_mu);
        g_paused = paused;
    }
    g_bg_cv.notify_all();
}

const std::vector<DisplayMode>& display_modes() {
    static const std::vector<DisplayMode> modes = [] {
        uint32_t w = 1920, h = 1080, hz = 60;
#if defined(SDL_PLATFORM_SWITCH)
        // Switch render scales use the handheld framebuffer as a stable base.
        // SDL's desktop mode can retain 1080p after docking; it must not turn
        // the same 720p render size into a different scale after undocking.
        w = 1280;
        h = 720;
#else
        on_main([&] {
            if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
                // SDL reports points; the GPU renders pixels.
                const float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
                w = uint32_t(m->w * density + 0.5f);
                h = uint32_t(m->h * density + 0.5f);
                if (m->refresh_rate > 0) hz = uint32_t(m->refresh_rate + 0.5f);
            }
        });
#endif
        if (h > w) std::swap(w, h);  // the game runs landscape
        std::vector<DisplayMode> out;
        for (const float s : {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
            // Multiples of 8, so the game's half/quarter/eighth-size buffers stay whole.
            out.push_back({(uint32_t(w * s) + 7) & ~7u, (uint32_t(h * s) + 7) & ~7u, hz, s});
        rt::log("platform: render modes %ux%u x0.5 .. x2 at %u Hz", w, h, hz);
        return out;
    }();
    return modes;
}

void display_size(int* width, int* height) {
#if defined(SDL_PLATFORM_SWITCH)
    *width = 1280;
    *height = 720;
#else
    on_main([&] {
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        *width = m ? m->w : 1920;
        *height = m ? m->h : 1080;
    });
#endif
}

}  // namespace platform

namespace {

std::atomic<bool> g_guest_done{false};

std::string g_saves_dir;
std::string g_hw_config;  // the game's hardware_settings_config.xml

// The game falls back to its largest mode (x2, supersampling) when the saved
// resolution is not one we offer, e.g. its first-run 1024x768. Start such a
// config at the native (x1) mode instead; after that the game's menu decides.
void normalise_resolution(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string xml;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) xml.append(buf, n);
    std::fclose(f);
    const size_t tag = xml.find("<resolution ");
    if (tag == std::string::npos) return;
    auto attr = [&](const char* name, size_t& pos, size_t& len) {
        const std::string key = std::string(" ") + name + "=\"";
        pos = xml.find(key, tag);
        if (pos == std::string::npos || pos > xml.find('>', tag)) return false;
        pos += key.size();
        len = xml.find('"', pos) - pos;
        return true;
    };
    size_t wp, wl, hp, hl;
    if (!attr("width", wp, wl) || !attr("height", hp, hl)) return;
    const uint32_t w = uint32_t(std::stoul(xml.substr(wp, wl))), h = uint32_t(std::stoul(xml.substr(hp, hl)));
    const platform::DisplayMode* native = nullptr;
    for (const auto& m : platform::display_modes()) {
        if (m.width == w && m.height == h) return;  // already one of ours
        if (m.scale == 1.0f) native = &m;
    }
    if (!native) return;
    // Height first: replacing width would move the height attribute's offset.
    xml.replace(hp, hl, std::to_string(native->height));
    xml.replace(wp, wl, std::to_string(native->width));
    if (std::FILE* o = std::fopen(path.c_str(), "wb")) {
        std::fwrite(xml.data(), 1, xml.size(), o);
        std::fclose(o);
        rt::log("platform: saved resolution %ux%u is not offered, starting at native %ux%u", w, h, native->width,
                native->height);
    }
}

// MetalFX on/off is the graphics menu's METALFX row (the aspect ratio row),
// saved in the game's hardware settings (shims/frontend/menu_patch.cpp).
void read_metalfx_setting(const std::string& path) {
    if (const int on = shim::read_metalfx_setting(path); on >= 0) platform::set_metalfx_enabled(on != 0);
}

// The game writes its settings when they are applied; checked once a second.
void watch_metalfx_setting(std::string path) {
    std::thread([path] {
        std::filesystem::file_time_type seen{};
        for (;;) {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(path, ec);
            if (!ec && t != seen) {
                seen = t;
                read_metalfx_setting(path);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}

void* game_main(void*) {
#if defined(__APPLE__)
    pthread_setname_np("game main");
#elif defined(__SWITCH__)
#else
    pthread_setname_np(pthread_self(), "game main");
#endif
    rt::install_crash_handler();
    // Saves sync before the game reads them (C:\Users\Player\Documents maps into
    // the data folder). On this thread so the main thread can show a dialog.
    platform::cloud_saves_start(g_saves_dir);
    normalise_resolution(g_hw_config);
    if (platform::metalfx_supported()) {
        read_metalfx_setting(g_hw_config);
        rt::log("metalfx: %s", platform::metalfx_enabled() ? "on" : "off");
        watch_metalfx_setting(g_hw_config);
    }
    X86* c = rt::create_main_thread();
    rt::log("entering guest at 0x%08X", recomp_image_info.entry);
    const uint32_t r = rt::invoke(c, recomp_image_info.entry, {}, true);
    rt::log("guest entry returned %u", r);
    g_guest_done = true;
#if defined(DIRT2_SWITCH_DXVK)
    // Other guest threads can still own graphics objects. End the process so
    // Horizon releases GPU pages, rather than chainloading over live GPU memory.
    svcExitProcess();
#else
    std::exit(int(r));
#endif
}

// Copies everything written to stderr (our log, fatal errors with guest
// registers, DXVK) into <data>/dirt2.log as well: on iOS the console is gone
// unless a debugger is attached.
void tee_stderr(const std::string& path) {
#if defined(SDL_PLATFORM_SWITCH)
    // No console and no pipes on Horizon: stderr goes straight to the file
    // (line buffered, so a crash keeps everything before it).
    std::rename(path.c_str(), (path.substr(0, path.size() - 4) + ".prev.log").c_str());
    // Allow capture snapshots through this same handle: HOS refuses a second
    // read handle while the log is open for writing.
    if (std::freopen(path.c_str(), "w+", stderr)) std::setvbuf(stderr, nullptr, _IOLBF, 0);
    return;
#endif
    int fds[2];
    const int console = dup(STDERR_FILENO);
    // Keep the previous run's log: a crash is often only looked at after a relaunch.
    std::rename(path.c_str(), (path.substr(0, path.size() - 4) + ".prev.log").c_str());
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file || console < 0 || pipe(fds) != 0) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([in = fds[0], console, file] {
        char buf[4096];
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0) {
            (void)!write(console, buf, size_t(n));
            std::fwrite(buf, 1, size_t(n), file);
            std::fflush(file);
        }
    }).detach();
}

constexpr uint32_t WM_CLOSE = 0x10, WM_ACTIVATEAPP = 0x1C, WM_ACTIVATE = 0x06;

// App lifecycle on iOS. SDL calls event watches synchronously, so the render
// thread is parked at its next Present before the app finishes going to the
// background (submitting GPU work there loses the Metal device for good).
bool SDLCALL lifecycle_watch(void*, SDL_Event* e) {
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        platform::cloud_saves_flush();
        shim::post_message(0, WM_ACTIVATE, 0, 0);
        shim::post_message(0, WM_ACTIVATEAPP, 0, 0);
        std::unique_lock lk(platform::g_bg_mu);
        platform::g_background = true;
        // A render thread busy elsewhere (loading) parks at its next Present.
        platform::g_bg_cv.wait_for(lk, std::chrono::seconds(2), [] { return platform::g_parked; });
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        {
            std::lock_guard lk(platform::g_bg_mu);
            platform::g_background = false;
        }
        platform::g_bg_cv.notify_all();
        shim::post_message(0, WM_ACTIVATEAPP, 1, 0);
        shim::post_message(0, WM_ACTIVATE, 1, 0);
    }
    return true;
}

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

// Pipeline warm-up (third_party/patches/dxvk/0003-pipeline-warmup.patch).
// Without pipeline libraries on MoltenVK, DXVK compiles each new pipeline at
// its first draw: a stutter. DXVK records every pipeline the game uses and
// compiles the recorded ones on its worker threads at the next start. The
// app ships a shader cache and pipeline list (next to the executable, or in
// the iOS bundle); the shader cache is copied into the data directory because
// DXVK opens it read-write. This device's own recording adds to the shipped one.
void setup_pipeline_warmup(const std::string& data) {
    namespace fs = std::filesystem;
    setenv("DXVK_SHADER_CACHE_NAME", "dirt2", 0);
#if !defined(SDL_PLATFORM_IOS) && !defined(SDL_PLATFORM_TVOS) && !defined(SDL_PLATFORM_VISIONOS)
    setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
#endif
    const std::string recorded = data + "/dirt2.pipelines";
    std::string warmup = recorded;
    if (const char* b = SDL_GetBasePath()) {
        const std::string base = b;
        std::error_code ec;
        if (fs::exists(base + "dirt2.pipelines", ec)) warmup = base + "dirt2.pipelines:" + warmup;
        // Copy the shipped shader cache once per app build: the stamp is the
        // shipped file's size and time, so an update brings its new cache.
        const std::string lut = base + "dirt2.dxvk.lut", bin = base + "dirt2.dxvk.bin";
        if (fs::exists(lut, ec) && fs::exists(bin, ec)) {
            const std::string stamp = std::to_string(fs::file_size(lut, ec)) + " " +
                std::to_string((long long)fs::last_write_time(lut, ec).time_since_epoch().count());
            const std::string stamp_path = data + "/dirt2.dxvk.stamp";
            std::string old;
            if (FILE* f = std::fopen(stamp_path.c_str(), "r")) {
                char buf[64] = {};
                if (std::fgets(buf, sizeof buf, f)) old = buf;
                std::fclose(f);
            }
            if (old != stamp) {
                bool ok = true;
                for (const std::string* src : {&bin, &lut}) {
                    const std::string dst = data + "/" + fs::path(*src).filename().string();
                    ok = ok && fs::copy_file(*src, dst + ".tmp", fs::copy_options::overwrite_existing, ec);
                    if (ok) fs::rename(dst + ".tmp", dst, ec);
                    ok = ok && !ec;
                }
                if (FILE* f = ok ? std::fopen(stamp_path.c_str(), "w") : nullptr) {
                    std::fputs(stamp.c_str(), f);
                    std::fclose(f);
                }
                std::fprintf(stderr, "pipeline warm-up: %s the shipped shader cache\n",
                             ok ? "installed" : "failed to install");
            }
        }
    }
    setenv("DXVK_PIPELINE_WARMUP", warmup.c_str(), 0);
    // DXVK_PIPELINE_RECORD= (empty) stops recording.
    setenv("DXVK_PIPELINE_RECORD", recorded.c_str(), 0);

    // DX11 path (DXMT, dxmt/0004-pipeline-warmup.patch): the same scheme. Its
    // converted-shader cache lives in the data folder (iOS may purge
    // Library/Caches); pipelines are recorded there and warmed up from the
    // shipped list plus this device's own recordings.
    const std::string dxmt_cache = data + "/dxmt";
    std::error_code ec;
    fs::create_directories(dxmt_cache, ec);
    setenv("DXMT_SHADER_CACHE_PATH", dxmt_cache.c_str(), 0);
    const std::string dxmt_recorded = data + "/dirt2.dxmt-pipelines";
    std::string dxmt_warmup = dxmt_recorded;
    if (const char* b = SDL_GetBasePath()) {
        const std::string base = b;
        if (fs::exists(base + "dirt2.dxmt-pipelines", ec)) dxmt_warmup = base + "dirt2.dxmt-pipelines:" + dxmt_warmup;
        // The shipped converted-shader cache, installed once per app build like DXVK's.
        const std::string shipped = base + "dxmt";
        if (fs::is_directory(shipped, ec))
            for (const auto& e : fs::directory_iterator(shipped, ec)) {
                const fs::path dst = fs::path(dxmt_cache) / e.path().filename();
                const auto size = fs::file_size(e.path(), ec);
                const std::string stamp_path = dst.string() + ".stamp";
                const std::string stamp = std::to_string(size) + " " +
                    std::to_string((long long)fs::last_write_time(e.path(), ec).time_since_epoch().count());
                std::string old;
                if (FILE* f = std::fopen(stamp_path.c_str(), "r")) {
                    char buf[64] = {};
                    if (std::fgets(buf, sizeof buf, f)) old = buf;
                    std::fclose(f);
                }
                if (old == stamp) continue;
                if (fs::copy_file(e.path(), dst.string() + ".tmp", fs::copy_options::overwrite_existing, ec) && !ec) {
                    fs::rename(dst.string() + ".tmp", dst, ec);
                    if (FILE* f = !ec ? std::fopen(stamp_path.c_str(), "w") : nullptr) {
                        std::fputs(stamp.c_str(), f);
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "pipeline warm-up: installed the shipped DXMT cache %s\n",
                                 dst.filename().c_str());
                }
            }
    }
    setenv("DXMT_PIPELINE_WARMUP", dxmt_warmup.c_str(), 0);
    // MetalFX on the DX11 path: DXMT's own scaler, to the window's drawable
    // size (dxmt/0005; the bridge sets the target and the on/off setting).
    if (platform::metalfx_supported()) setenv("DXMT_METALFX_SPATIAL_NATIVE", "1", 0);
    setenv("DXMT_PIPELINE_RECORD", dxmt_recorded.c_str(), 0);
}

}  // namespace

#if defined(SDL_PLATFORM_SWITCH)
// Everything lives on the SD card: the game in sdmc:/switch/dirt2/Dirt 2, our
// data (log, saves, settings) in sdmc:/switch/dirt2.
#if defined(DIRT2_SWITCH_DXVK)
constexpr const char* kSwitchDataDir = "sdmc:/switch/dirt2-dx11";
#else
constexpr const char* kSwitchDataDir = "sdmc:/switch/dirt2";
#endif
#endif

#if defined(DIRT2_SWITCH_DXVK)
namespace switch_platform { bool check_c11_abis(); int run_dx11_preflight(); void start_dx11_startup_watchdog(); }
#endif

int main(int argc, char** argv) {
    std::string dir;
    if (const char* e = std::getenv("DIRT2_GAME_DIR")) dir = e;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--game")) dir = argv[++i];
#if defined(SDL_PLATFORM_SWITCH)
    if (!switch_platform::check_launch_mode()) return 1;
    setenv("DIRT2_DATA_DIR", kSwitchDataDir, 0);
    // No environment on Horizon: <data>/env.txt holds KEY=VALUE lines (the debug
    // switches: DIRT2_COUNT_CALLS=1, DIRT2_LOG_LOCKS=1, ...).
    if (std::FILE* f = std::fopen((std::string(kSwitchDataDir) + "/env.txt").c_str(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
            const size_t eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
            setenv(l.substr(0, eq).c_str(), l.substr(eq + 1).c_str(), 1);
        }
        std::fclose(f);
    }
    if (dir.empty()) {
#if defined(DIRT2_SWITCH_DXVK)
        dir = "sdmc:/switch/dirt2/Dirt 2";
#else
        dir = std::string(kSwitchDataDir) + "/Dirt 2";
#endif
        if (!std::filesystem::exists(dir + "/dirt2_game.exe")) {
            switch_platform::report_missing_game(dir);
            return 2;
        }
    }
#elif defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS) || defined(SDL_PLATFORM_VISIONOS)
    // The game data is copied into the app's Documents folder (Finder or the
    // Files app): either the "Dirt 2" folder itself or its contents.
    if (dir.empty())
        if (const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
            for (std::string d : {std::string(docs) + "Dirt 2", std::string(docs)})
                if (std::filesystem::exists(d + "/dirt2_game.exe")) {
                    dir = d;
                    break;
                }
            if (dir.empty()) {
                // The Files app lists an app's folder only once Documents holds
                // something: leave the instructions there.
                if (FILE* f = std::fopen((std::string(docs) + "put your game here.txt").c_str(), "w")) {
                    std::fputs("Copy the \"Dirt 2\" game folder (the one with dirt2_game.exe) into this folder, "
                               "then start DiRT 2 again. Saves sync through iCloud.\n", f);
                    std::fclose(f);
                }
                const std::string msg = "Copy the \"Dirt 2\" game folder (the one with dirt2_game.exe) into this app's "
                                        "Documents with Finder or the Files app, then start the app again.\n\n" +
                                        std::string(docs);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "DiRT 2: game data missing", msg.c_str(), nullptr);
                return 2;
            }
        }
#else
    // macOS without --game (the .app): "Dirt 2" next to the app, the folder chosen
    // last time, or a folder picker; the choice is remembered.
    if (dir.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string data_dir = std::getenv("DIRT2_DATA_DIR") ? std::getenv("DIRT2_DATA_DIR")
                                     : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                                           "/Library/Application Support/Dirt2Recomp";
        const std::string saved = data_dir + "/game_dir.txt";
        auto valid = [&](const std::string& d) { return !d.empty() && fs::exists(d + "/dirt2_game.exe", ec); };
        if (const char* b = SDL_GetBasePath()) {
            // Contents/Resources/ inside the .app; the folder that holds the .app.
            for (const fs::path& p : {fs::path(b) / "../../../Dirt 2", fs::path(b) / "Dirt 2"})
                if (valid(p.lexically_normal().string())) {
                    dir = p.lexically_normal().string();
                    break;
                }
        }
        if (dir.empty())
            if (FILE* f = std::fopen(saved.c_str(), "r")) {
                char buf[4096] = {};
                if (std::fgets(buf, sizeof buf, f)) {
                    std::string d = buf;
                    while (!d.empty() && (d.back() == '\n' || d.back() == '\r')) d.pop_back();
                    if (valid(d)) dir = d;
                }
                std::fclose(f);
            }
        while (dir.empty()) {
            std::string picked;
            if (!platform::choose_folder("Select the DiRT 2 game folder (the one with dirt2_game.exe)", picked)) {
                std::fprintf(stderr, "usage: %s --game <folder with dirt2_game.exe>\n", argv[0]);
                return 2;
            }
            if (valid(picked)) {
                dir = picked;
                fs::create_directories(data_dir, ec);
                if (FILE* f = std::fopen(saved.c_str(), "w")) {
                    std::fputs(dir.c_str(), f);
                    std::fclose(f);
                }
            }
        }
    }
#endif
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
    std::string data;
    {
        if (const char* e = std::getenv("DIRT2_DATA_DIR")) data = e;
        else if (const char* h = std::getenv("HOME")) data = std::string(h) + "/Library/Application Support/Dirt2Recomp";
        else data = ".";
        std::error_code ec;
        std::filesystem::create_directories(data, ec);
        setenv("DXVK_LOG_PATH", data.c_str(), 0);
        // Defaults to $HOME/.cache, which is not writable on iOS (HOME is the container root).
        setenv("DXVK_SHADER_CACHE_PATH", data.c_str(), 0);
#if defined(DIRT2_SWITCH_DXVK)
        const std::string mesa_cache = data + "/mesa-cache";
        std::filesystem::create_directories(mesa_cache, ec);
        setenv("MESA_SHADER_CACHE_DIR", mesa_cache.c_str(), 0);
        rt::log("Switch prototype: Direct3D 11 -> DXVK -> NVK (separate data directory)");
#endif
        rt::set_discovery_log(data + "/recomp_discovered.txt");
        tee_stderr(data + "/dirt2.log");
#if defined(DIRT2_SWITCH_DXVK)
        if (!switch_platform::check_c11_abis()) {
            rt::log("Stopping before GPU initialization: C11 ABI self-check failed");
            return 1;
        }
#endif
        platform::metalfx_init();
        // DIRT2_PROFILE=1, or a file named "profile" in the data folder (iOS has no environment).
        if (std::getenv("DIRT2_PROFILE") || std::filesystem::exists(data + "/profile")) rt::start_profiler();
        setup_pipeline_warmup(data);
    }
    // SDL's GameController (MFi) driver reports some pads (Backbone One) as
    // connected but never delivers their input. Without it, Xbox/PlayStation/
    // Switch pads still use SDL's HIDAPI drivers and others use IOKit HID with
    // the gamecontrollerdb.txt mappings below. DIRT2_JOYSTICK_MFI=1 restores it.
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS) || defined(SDL_PLATFORM_VISIONOS)
    // On iOS GameController (MFi) is the only way controllers reach an app.
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "1");
#else
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, std::getenv("DIRT2_JOYSTICK_MFI") ? "1" : "0");
#endif
#if defined(SDL_PLATFORM_SWITCH)
    // The D3D9-on-deko3d device owns the NWindow's buffers: SDL must not put an EGL surface on it.
    SDL_SetHint("SDL_SWITCH_NATIVE_WINDOW", "1");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
#if defined(DIRT2_SWITCH_DXVK)
    if (const char* test = std::getenv("DIRT2_DX11_PREFLIGHT_ONLY"); test && std::strcmp(test, "0")) {
        // Keep game pipeline replay and HUD draws out of the isolated GPU test.
        setenv("DXVK_PIPELINE_WARMUP", "", 1);
        setenv("DXVK_PIPELINE_RECORD", "", 1);
        setenv("DXVK_SHADER_CACHE_NAME", "preflight", 1);
        setenv("DXVK_HUD", "", 1);
        return switch_platform::run_dx11_preflight();
    }
#endif
    SDL_AddEventWatch(lifecycle_watch, nullptr);
    platform::set_app_icon(dir + "/dirt2.ico");
    // Community mappings shipped next to the executable, then optional user
    // additions in the data directory (later entries override earlier ones).
    // The Switch's own controllers need none (SDL's Switch joystick driver).
#if !defined(SDL_PLATFORM_SWITCH)
    if (const char* base = SDL_GetBasePath()) {
        const std::string db = std::string(base) + "gamecontrollerdb.txt";
        const int n = SDL_AddGamepadMappingsFromFile(db.c_str());
        if (n < 0) rt::log("gamepad mappings: cannot load %s: %s", db.c_str(), SDL_GetError());
        else rt::log("gamepad mappings: %d from %s", n, db.c_str());
    }
#endif
    if (std::filesystem::exists(data + "/gamecontrollerdb.txt"))
        SDL_AddGamepadMappingsFromFile((data + "/gamecontrollerdb.txt").c_str());

    g_saves_dir = data + "/C/Users/Player/Documents/My Games/DiRT2/savegame";
#if defined(DIRT2_SWITCH_DXVK)
    vfs::set_save_dir("sdmc:/switch/dirt2/C/Users/Player/Documents/My Games/DiRT2/savegame");
    g_saves_dir = vfs::save_dir();
    rt::log("Switch DX11: shared saves at %s", vfs::save_dir().c_str());
#endif
    g_hw_config = data + "/C/Users/Player/Documents/My Games/DiRT2/hardwaresettings/hardware_settings_config.xml";

    if (!rt::mem_init()) return 1;
    std::string err;
    if (!rt::load_image(dir + "/dirt2_game.exe", err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (chdir(dir.c_str()) != 0) rt::log("warning: cannot chdir to %s", dir.c_str());
#if defined(SDL_PLATFORM_SWITCH)
    switch_platform::install_resolution_restrictions();
#endif
    shim::install_resolution_labels();
    shim::install_menu_patches(platform::metalfx_supported());

#if defined(DIRT2_SWITCH_DXVK)
    switch_platform::start_dx11_startup_watchdog();
#endif
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Guest call depth nests host frames. Horizon commits the whole stack up front.
#if defined(SDL_PLATFORM_SWITCH)
    pthread_attr_setstacksize(&attr, 64u << 20);
#else
    pthread_attr_setstacksize(&attr, 256u << 20);
#endif
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
