#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <vector>

namespace platform::touch {
struct State {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
struct Vertex { float x, y; uint32_t color; };
struct Frame { float width = 0, height = 0; std::vector<Vertex> vertices; };
#ifdef DIRT2_TOUCH_CONTROLLER
// Window/event functions run on SDL's main thread. Snapshots are thread safe.
void attach(SDL_Window* window);
void detach(SDL_Window* window);
void event(const SDL_Event& event);
void update();
State state();
Frame frame();
#else
inline void attach(SDL_Window*) {}
inline void detach(SDL_Window*) {}
inline void event(const SDL_Event&) {}
inline void update() {}
inline State state() { return {}; }
inline Frame frame() { return {}; }
#endif
}
