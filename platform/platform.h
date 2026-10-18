// Host platform services (SDL3 on every target). Window functions may be
// called from any thread; they run on the main thread as Cocoa/UIKit require.
#pragma once

#include <cstdint>
#include <string>

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

// Asks the user for a line of text in a native dialog (the system keyboard on
// iOS). Blocks the calling thread, which must not be the main thread. Returns
// false when the user cancels.
bool prompt_text(const std::string& title, const std::string& message, const std::string& initial,
                 std::string& result);

// Uses the image at path (e.g. the game's dirt2.ico) as the application icon
// where the platform takes it from the process (macOS without a bundle).
void set_app_icon(const std::string& path);

}  // namespace platform
