// Host platform services (SDL3 on every target). Window functions may be
// called from any thread; they run on the main thread as Cocoa/UIKit require.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

using Window = void*;  // SDL_Window*

Window create_window(int width, int height, const char* title);
void destroy_window(Window w);
void set_window_size(Window w, int width, int height);
void show_window(Window w, bool show);
void drawable_size(Window w, int* width, int* height);
// Borderless desktop fullscreen (never an exclusive display-mode switch).
void set_fullscreen(Window w, bool fullscreen);

// Display size in pixels, for GetSystemMetrics / EnumDisplaySettings.
void display_size(int* width, int* height);

// The modes offered to the game (its Resolution menu): the display's native
// pixel size scaled by the render-scale factors, smallest first. Below 1 the
// frame is upscaled (MetalFX when enabled), above 1 it is supersampled.
struct DisplayMode {
    uint32_t width, height, refresh;
    float scale;
};
const std::vector<DisplayMode>& display_modes();

// Asks the user for a line of text in a native dialog (the system keyboard on
// iOS). Blocks the calling thread, which must not be the main thread. Returns
// false when the user cancels.
bool prompt_text(const std::string& title, const std::string& message, const std::string& initial,
                 std::string& result);

// Called by the render thread before each Present. While the app is in the
// background (iOS forbids GPU work there: the Metal device is lost), it blocks
// until the app is back in the foreground.
void gpu_checkpoint();

// Holds the game (its render thread, at the next Present) while paused, e.g.
// while the user answers a dialog the game must not run past.
void set_game_paused(bool paused);

// Save-game sync through iCloud Drive (no-op without iCloud). start: called on
// the game thread before the game runs (it may ask the user, so the main thread
// must be pumping events); pulls another device's newer snapshot into local_dir,
// then uploads after each save. flush: uploads now (going to the background).
void cloud_saves_start(const std::string& local_dir);
void cloud_saves_flush();

// MetalFX spatial upscaling of the frame when the render scale is below 1
// (platform/apple/metalfx.mm). init: before the game creates its device; the
// setting persists in <data>/metalfx.txt (default on). Unsupported devices
// never upscale with MetalFX, and the menu row is not shown there.
void metalfx_init(const std::string& data_dir);
bool metalfx_supported();
bool metalfx_enabled();
void set_metalfx_enabled(bool on);

// Asks the user to pick one of two options in a native dialog; returns 0 or 1.
// Blocks the calling thread, which must not be the main thread.
int choose(const std::string& title, const std::string& message, const std::string& first, const std::string& second);

// Uses the image at path (e.g. the game's dirt2.ico) as the application icon
// where the platform takes it from the process (macOS without a bundle).
void set_app_icon(const std::string& path);

}  // namespace platform
