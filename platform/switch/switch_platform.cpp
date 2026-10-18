// Nintendo Switch (libnx) parts of the platform layer: what the Apple builds
// get from platform/apple (dialogs, cloud saves, MetalFX), plus the Switch's
// own start-up checks. Windows, events and input are SDL3 (platform/sdl), the
// devkitPro SDL3 port (third_party/patches/sdl3).
#include <switch.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// Software keyboard; the caller is the game thread (never SDL's main thread).
bool prompt_text(const std::string& title, const std::string& message, const std::string& initial,
                 std::string& result) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, title.c_str());
    swkbdConfigSetSubText(&kbd, message.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 64);
    char out[256] = {};
    const bool ok = R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out));
    swkbdClose(&kbd);
    if (ok) result = out;
    return ok;
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

std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

}  // namespace switch_platform

// Gate 0 (headless boot): no GPU backend yet. The game has run its CRT and
// engine start-up when it asks for Direct3D: log how long that took and how
// much guest memory it committed, then end the run cleanly.
struct IDirect3D9;
extern "C" IDirect3D9* Direct3DCreate9(unsigned sdk_version) {
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - switch_platform::g_start).count();
    rt::log("gate 0: Direct3DCreate9(%u) reached %.2f s after start, %llu MB of guest memory committed", sdk_version, s,
            (unsigned long long)(rt::committed_bytes() >> 20));
    std::fflush(stderr);
    std::exit(0);
}
