// Nintendo Switch (libnx) parts of the platform layer: what the Apple builds
// get from platform/apple (dialogs, cloud saves, MetalFX), plus the Switch's
// own start-up checks. Windows, events and input are SDL3 (platform/sdl), the
// devkitPro SDL3 port (third_party/patches/sdl3).
#include <switch.h>
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iterator>
#include "resolution_restrictions.h"
#include "worker_topology.h"
#include "../../shims/vfs.h"
#include <string>

#include "../platform.h"
#include "recomp/host.h"

namespace {

// The error applet: the one dialog every launch mode can show. Blocks until dismissed.
void show_error(const std::string& message, const std::string& details) {
    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, message.c_str(), details.empty() ? nullptr : details.c_str())))
        errorApplicationShow(&c);
}

}  // namespace

namespace platform {

// Serialize the modal HOS applet with SDL event polling. XShowKeyboardUI queues
// its asynchronous request on this thread; other callers may use a worker.
bool prompt_text(const std::string& title, const std::string& message, const std::string& initial,
                 std::string& result) {
    struct Prompt {
        const std::string& title;
        const std::string& message;
        const std::string& initial;
        std::string& result;
        bool ok = false;
    } prompt{title, message, initial, result};
    auto show = [](void* data) {
        auto& p = *static_cast<Prompt*>(data);
        SwkbdConfig kbd;
        Result rc = swkbdCreate(&kbd, 0);
        if (R_FAILED(rc)) {
            rt::log("platform: HOS keyboard creation failed: %08X", unsigned(rc));
            return;
        }
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetHeaderText(&kbd, p.title.c_str());
        swkbdConfigSetSubText(&kbd, p.message.c_str());
        swkbdConfigSetInitialText(&kbd, p.initial.c_str());
        swkbdConfigSetStringLenMax(&kbd, 64);
        char out[256] = {};
        rc = swkbdShow(&kbd, out, sizeof out);
        swkbdClose(&kbd);
        p.ok = R_SUCCEEDED(rc);
        if (p.ok) p.result = out;
        else rt::log("platform: HOS keyboard closed without input: %08X", unsigned(rc));
    };
    // The pending XLive request lets the game keep presenting. Park it while
    // the system owns the screen, and resume on success, cancellation or error.
    set_game_paused(true);
    const bool dispatched = SDL_RunOnMainThread(show, &prompt, true);
    set_game_paused(false);
    if (!dispatched) rt::log("platform: HOS keyboard dispatch failed: %s", SDL_GetError());
    return dispatched && prompt.ok;
}

// No two-button dialog on Horizon outside a graphics stack: the first option.
int choose(const std::string& title, const std::string& message, const std::string&, const std::string&) {
    rt::log("platform: %s: %s (first option taken)", title.c_str(), message.c_str());
    return 0;
}

bool choose_folder(const std::string&, std::string&) { return false; }
void set_app_icon(const std::string&) {}  // the NRO carries its icon

void cloud_saves_start(const std::string&) {}
void cloud_saves_flush() {}

void metalfx_init() {}
bool metalfx_supported() { return false; }
bool metalfx_enabled() { return false; }
void set_metalfx_enabled(bool) {}

}  // namespace platform

namespace switch_platform {

void install_resolution_restrictions() {
    constexpr const char* relative = "system/hardware_settings_restrictions.xml";
    std::ifstream input(vfs::host_path(std::string(vfs::kInstallDir) + "/" + relative), std::ios::binary);
    if (!input) { rt::log("Switch: minimum-resolution rules unavailable"); return; }
    std::string xml((std::istreambuf_iterator<char>(input)), {});
    if (!allow_half_resolution(xml)) { rt::log("Switch: unknown minimum-resolution rules; leaving unchanged"); return; }
    const std::string directory = vfs::data_dir() + "/switch-overrides";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::string path = directory + "/hardware_settings_restrictions.xml";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(xml.data(), std::streamsize(xml.size()));
    output.close();
    if (!output) { rt::log("Switch: cannot write minimum-resolution override"); return; }
    vfs::set_override(relative, path);
    rt::log("Switch: minimum resolution 640x360; private settings override installed");
}


void install_worker_topology() {
    const char* setting = std::getenv("DIRT2_SWITCH_PARALLEL_JOBS");
    if (!setting || std::string_view(setting) != "1") return;
    const std::string directory = vfs::data_dir() + "/switch-overrides";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::string path = directory + "/workerMapSwitch.xml";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::string xml = parallel_worker_map();
    output.write(xml.data(), std::streamsize(xml.size()));
    output.close();
    if (!output) { rt::log("Switch: cannot write parallel worker override"); return; }
    // Override either normal selector without modifying the user's settings or
    // installed assets. The serial2-core map is deliberately left alone.
    vfs::set_override("system/workerMap4Core.xml", path);
    vfs::set_override("system/workerMap8Core.xml", path);
    rt::log("Switch: parallel job map enabled:3 simulation workers,2 grass workers; solver/softbody disjoint");
}


// The game needs title-takeover mode (an application's ~3.2 GB); the album
// applet gives homebrew about 400 MB. Returns false after telling the user.
bool check_launch_mode() {
    const AppletType t = appletGetAppletType();
    if (t == AppletType_Application || t == AppletType_SystemApplication) return true;
    show_error("DiRT 2 needs the full memory of a game.",
               "Start it from a game instead of the Album: hold R while launching any installed game, "
               "then pick DiRT 2 in the Homebrew Menu.");
    return false;
}

void report_missing_game(const std::string& dir) {
    show_error("DiRT 2 game files not found.",
               "Copy the \"Dirt 2\" game folder (the one with dirt2_game.exe) to " + dir + " on the SD card.");
}

}  // namespace switch_platform
