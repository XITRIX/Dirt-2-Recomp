// XInput 1.3 on SDL3 gamepads (GameController on Apple platforms).
// Player 1 falls back to a keyboard-driven virtual pad when no gamepad is
// connected, which is what development on a Mac without a controller needs.
#include <SDL3/SDL.h>
#include "../../platform/sdl/virtual_controller.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../shim.h"
#include "../d3d9/capture.h"

using namespace shim;

namespace {

constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;

enum : uint16_t {
    XI_DPAD_UP = 0x0001, XI_DPAD_DOWN = 0x0002, XI_DPAD_LEFT = 0x0004, XI_DPAD_RIGHT = 0x0008,
    XI_START = 0x0010, XI_BACK = 0x0020, XI_LTHUMB = 0x0040, XI_RTHUMB = 0x0080,
    XI_LSHOULDER = 0x0100, XI_RSHOULDER = 0x0200, XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000,
};

struct PadState {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool operator==(const PadState&) const = default;
};

std::mutex g_mu;
SDL_Gamepad* g_pads[4] = {};
PadState g_last[4];
uint32_t g_packet[4] = {};
#ifdef __SWITCH__
bool g_dump_chord[4] = {};
std::atomic<unsigned> g_dump_pad{0};
Uint64 g_dump_feedback_until = 0;
#endif

// DIRT2_INPUT_TRACE=1 logs the first call per (function, index, result) and
// every gamepad state change, to diagnose pads the game ignores.
bool trace_on() {
    static const bool on = std::getenv("DIRT2_INPUT_TRACE") != nullptr;
    return on;
}

void trace_call(int fn, uint32_t idx, uint32_t result) {
    if (!trace_on() || idx > 3) return;
    static bool seen[5][4][2];
    bool& s = seen[fn][idx][result == 0];
    if (s) return;
    s = true;
    rt::log("xinput: ord%d(%u) -> %u", fn, idx, result);
}

// Opens newly connected gamepads into free player slots.
void refresh_pads() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        bool open = false;
        for (SDL_Gamepad* p : g_pads)
            if (p && SDL_GetGamepadID(p) == ids[i]) open = true;
        if (open) continue;
        for (SDL_Gamepad*& p : g_pads)
            if (!p) {
                p = SDL_OpenGamepad(ids[i]);
                if (p) rt::log("xinput: player slot -> %s", SDL_GetGamepadName(p));
                break;
            }
    }
    SDL_free(ids);
    for (SDL_Gamepad*& p : g_pads)
        if (p && !SDL_GamepadConnected(p)) {
            SDL_CloseGamepad(p);
            p = nullptr;
        }
}

int16_t axis(SDL_Gamepad* p, SDL_GamepadAxis a, bool invert) {
    const int v = SDL_GetGamepadAxis(p, a);
    return int16_t(std::clamp(invert ? -v : v, -32767, 32767));
}

PadState read_pad(SDL_Gamepad* p) {
    PadState s;
    struct { SDL_GamepadButton b; uint16_t x; } map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XI_DPAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XI_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XI_DPAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XI_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, XI_START}, {SDL_GAMEPAD_BUTTON_BACK, XI_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XI_LTHUMB}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XI_RTHUMB},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XI_LSHOULDER}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XI_RSHOULDER},
        {SDL_GAMEPAD_BUTTON_SOUTH, XI_A}, {SDL_GAMEPAD_BUTTON_EAST, XI_B},
        {SDL_GAMEPAD_BUTTON_WEST, XI_X}, {SDL_GAMEPAD_BUTTON_NORTH, XI_Y},
    };
    for (auto& m : map)
        if (SDL_GetGamepadButton(p, m.b)) s.buttons |= m.x;
    s.lt = uint8_t(std::clamp(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, 0, 255));
    s.rt = uint8_t(std::clamp(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128, 0, 255));
    s.lx = axis(p, SDL_GAMEPAD_AXIS_LEFTX, false);
    s.ly = axis(p, SDL_GAMEPAD_AXIS_LEFTY, true);  // XInput: up is positive
    s.rx = axis(p, SDL_GAMEPAD_AXIS_RIGHTX, false);
    s.ry = axis(p, SDL_GAMEPAD_AXIS_RIGHTY, true);
    return s;
}

PadState read_keyboard() {
    PadState s;
    const bool* k = SDL_GetKeyboardState(nullptr);
    if (!k) return s;
    if (k[SDL_SCANCODE_UP]) s.buttons |= XI_DPAD_UP;
    if (k[SDL_SCANCODE_DOWN]) s.buttons |= XI_DPAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) { s.buttons |= XI_DPAD_LEFT; s.lx = -32767; }
    if (k[SDL_SCANCODE_RIGHT]) { s.buttons |= XI_DPAD_RIGHT; s.lx = 32767; }
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]) s.buttons |= XI_A;
    if (k[SDL_SCANCODE_ESCAPE]) s.buttons |= XI_B;
    if (k[SDL_SCANCODE_TAB]) s.buttons |= XI_START;
    if (k[SDL_SCANCODE_BACKSPACE]) s.buttons |= XI_BACK;
    if (k[SDL_SCANCODE_W]) s.rt = 255;
    if (k[SDL_SCANCODE_S]) s.lt = 255;
    if (k[SDL_SCANCODE_Q]) s.buttons |= XI_LSHOULDER;
    if (k[SDL_SCANCODE_E]) s.buttons |= XI_RSHOULDER;
    if (k[SDL_SCANCODE_X]) s.buttons |= XI_X;
    if (k[SDL_SCANCODE_Y]) s.buttons |= XI_Y;
    return s;
}

// DIRT2_INPUT_SCRIPT drives player 1 from a timeline, for reproducible test
// runs (menus to a race) on every platform:
//   "<seconds> <input>[+<input>...] [hold seconds, default 0.15]; ..."
// inputs: A B X Y start back up down left right lb rb lt rt, and axes
// lx=<-1..1> ly= rx= ry= (lt= rt= take 0..1). Times count from the first poll.
struct ScriptStep {
    double at = 0, hold = 0.15;
    PadState pad;
};

std::vector<ScriptStep> parse_script(const char* text) {
    std::vector<ScriptStep> steps;
    std::stringstream all(text);
    std::string item;
    while (std::getline(all, item, ';')) {
        std::stringstream in(item);
        ScriptStep st;
        std::string inputs;
        if (!(in >> st.at >> inputs)) continue;
        in >> st.hold;
        std::stringstream parts(inputs);
        std::string part;
        while (std::getline(parts, part, '+')) {
            const auto eq = part.find('=');
            const std::string key = part.substr(0, eq);
            const double v = eq == std::string::npos ? 1.0 : std::atof(part.c_str() + eq + 1);
            const int16_t ax = int16_t(std::clamp(v, -1.0, 1.0) * 32767);
            const uint8_t tr = uint8_t(std::clamp(v, 0.0, 1.0) * 255);
            static const struct { const char* n; uint16_t b; } names[] = {
                {"A", XI_A}, {"B", XI_B}, {"X", XI_X}, {"Y", XI_Y}, {"start", XI_START}, {"back", XI_BACK},
                {"up", XI_DPAD_UP}, {"down", XI_DPAD_DOWN}, {"left", XI_DPAD_LEFT}, {"right", XI_DPAD_RIGHT},
                {"lb", XI_LSHOULDER}, {"rb", XI_RSHOULDER}};
            bool known = false;
            for (auto& n : names)
                if (key == n.n) { st.pad.buttons |= n.b; known = true; }
            if (key == "lt") st.pad.lt = tr, known = true;
            else if (key == "rt") st.pad.rt = tr, known = true;
            else if (key == "lx") st.pad.lx = ax, known = true;
            else if (key == "ly") st.pad.ly = ax, known = true;
            else if (key == "rx") st.pad.rx = ax, known = true;
            else if (key == "ry") st.pad.ry = ax, known = true;
            if (!known) rt::log("input script: unknown input '%s'", key.c_str());
        }
        steps.push_back(st);
    }
    return steps;
}

// Adds the scripted inputs active now to player 1's state.
void apply_script(PadState& s) {
    static const char* text = std::getenv("DIRT2_INPUT_SCRIPT");
    if (!text) return;
    static const std::vector<ScriptStep> steps = parse_script(text);
    static const Uint64 t0 = SDL_GetTicksNS();
    const double now = double(SDL_GetTicksNS() - t0) / 1e9;
    for (const auto& st : steps) {
        if (now < st.at || now >= st.at + st.hold) continue;
        s.buttons |= st.pad.buttons;
        s.lt = std::max(s.lt, st.pad.lt);
        s.rt = std::max(s.rt, st.pad.rt);
        if (st.pad.lx) s.lx = st.pad.lx;
        if (st.pad.ly) s.ly = st.pad.ly;
        if (st.pad.rx) s.rx = st.pad.rx;
        if (st.pad.ry) s.ry = st.pad.ry;
    }
}

}  // namespace

STDCALL_ORD(xinput1_3, 2, 2) {  // XInputGetState(index, XINPUT_STATE*)
    const uint32_t idx = a[0];
    if (idx > 3 || !a[1]) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lk(g_mu);
    refresh_pads();
    PadState s;
    if (g_pads[idx]) s = read_pad(g_pads[idx]);
    else if (idx == 0) {
        s = read_keyboard();
#ifdef DIRT2_TOUCH_CONTROLLER
        const auto t = platform::touch::state();
        s.buttons |= t.buttons;
        s.lt = std::max(s.lt, t.lt); s.rt = std::max(s.rt, t.rt);
        if (t.lx) s.lx=t.lx; if (t.ly) s.ly=t.ly;
        if (t.rx) s.rx=t.rx; if (t.ry) s.ry=t.ry;
#endif
    }
    else {
        trace_call(2, idx, ERROR_DEVICE_NOT_CONNECTED);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
#ifdef __SWITCH__
    // Minus/Plus together: one request per full release, hidden from the game
    // while the chord is held so it does not also pause/back out of this screen.
    constexpr uint16_t dump_buttons = XI_START | XI_BACK;
    if ((s.buttons & dump_buttons) == dump_buttons && !g_dump_chord[idx]) {
        g_dump_chord[idx] = true;
        if (request_gpu_dump()) {
            g_dump_pad = idx;
            if (g_pads[idx]) SDL_RumbleGamepad(g_pads[idx], 0x5000, 0x5000, 120);
            g_dump_feedback_until = SDL_GetTicks() + 120;
            rt::log("xinput: minus+plus GPU capture requested");
        }
    }
    if (g_dump_chord[idx]) {
        if (!(s.buttons & dump_buttons)) g_dump_chord[idx] = false;
        s.buttons &= ~dump_buttons;
    }
#endif
    if (idx == 0) apply_script(s);
    if (!(s == g_last[idx])) {
        g_last[idx] = s;
        ++g_packet[idx];
        if (trace_on())
            rt::log("xinput: pad%u buttons=%04X lt=%u rt=%u lx=%d ly=%d", idx, s.buttons, s.lt, s.rt, s.lx, s.ly);
    }
    trace_call(2, idx, 0);
    const uint32_t d = a[1];
    wr32(d + 0, g_packet[idx]);
    wr16(d + 4, s.buttons);
    wr8(d + 6, s.lt);
    wr8(d + 7, s.rt);
    wr16(d + 8, uint16_t(s.lx));
    wr16(d + 10, uint16_t(s.ly));
    wr16(d + 12, uint16_t(s.rx));
    wr16(d + 14, uint16_t(s.ry));
    return 0;
}

namespace shim {
void gpu_dump_finished(bool success) {
#ifdef __SWITCH__
    std::lock_guard lk(g_mu);
    if (SDL_Gamepad* p = g_pads[g_dump_pad.load()])
        SDL_RumbleGamepad(p, success ? 0x9000 : 0x3000, success ? 0x9000 : 0, success ? 450 : 900);
    g_dump_feedback_until = SDL_GetTicks() + (success ? 450 : 900);
#endif
}
}  // namespace shim

STDCALL_ORD(xinput1_3, 3, 2) {  // XInputSetState(index, XINPUT_VIBRATION*)
    const uint32_t idx = a[0];
    if (idx > 3) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lk(g_mu);
    if (!g_pads[idx]) return idx == 0 ? 0 : ERROR_DEVICE_NOT_CONNECTED;
#ifdef __SWITCH__
    if (idx == g_dump_pad && SDL_GetTicks() < g_dump_feedback_until) return 0;
#endif
    if (a[1]) SDL_RumbleGamepad(g_pads[idx], rd16(a[1]), rd16(a[1] + 2), 10000);
    return 0;
}

STDCALL_ORD(xinput1_3, 4, 3) {  // XInputGetCapabilities(index, flags, XINPUT_CAPABILITIES*)
    const uint32_t idx = a[0];
    if (idx > 3 || !a[2]) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lk(g_mu);
    refresh_pads();
    if (!g_pads[idx] && idx != 0) {
        trace_call(4, idx, ERROR_DEVICE_NOT_CONNECTED);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    trace_call(4, idx, 0);
    const uint32_t d = a[2];
    std::memset(rt::host_ptr(d), 0, 20);
    wr8(d + 0, 1);         // XINPUT_DEVTYPE_GAMEPAD
    wr8(d + 1, 1);         // XINPUT_DEVSUBTYPE_GAMEPAD
    wr16(d + 4, 0xF3FF);   // all buttons
    wr8(d + 6, 0xFF);
    wr8(d + 7, 0xFF);
    for (int i = 0; i < 4; ++i) wr16(d + 8 + 2 * i, 0xFFC0);
    wr16(d + 16, 0xFFFF);
    wr16(d + 18, 0xFFFF);
    return 0;
}
