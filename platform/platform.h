// Host platform services (SDL3 on every target). Window functions may be
// called from any thread; they run on the main thread as Cocoa/UIKit require.
#pragma once

#include <cstdint>

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

}  // namespace platform
