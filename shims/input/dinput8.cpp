// DirectInput 8 for the system keyboard and mouse, on SDL3. Game controllers
// are not enumerated: the game reads pads through XInput.
#include <SDL3/SDL.h>

#include <mutex>
#include <unordered_map>

#include "../guest_com.h"

using namespace shim;

namespace {

constexpr uint32_t DI_OK = 0, DI_NOEFFECT = 1, DIERR_UNSUPPORTED = 0x80004001, DIERR_INVALIDPARAM = 0x80070057,
                   DIERR_DEVICENOTREG = 0x80040154;
constexpr uint32_t DI8DEVCLASS_ALL = 0, DI8DEVCLASS_POINTER = 2, DI8DEVCLASS_KEYBOARD = 3;
constexpr uint32_t DI8DEVTYPE_MOUSE = 0x12, DI8DEVTYPE_KEYBOARD = 0x13;

// GUID_SysMouse / GUID_SysKeyboard differ only in the first dword.
constexpr uint32_t kSysMouse = 0x6F1D2B60, kSysKeyboard = 0x6F1D2B61;
const uint8_t kSysGuidTail[12] = {0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};

enum class DevKind { Keyboard, Mouse };
struct Device {
    DevKind kind;
    uint32_t data_size = 0;
};
std::mutex g_mu;
std::unordered_map<uint32_t, Device> g_devices;  // guest object -> state

void write_guid(uint32_t dst, uint32_t first) {
    wr32(dst, first);
    std::memcpy(rt::host_ptr(dst + 4), kSysGuidTail, 12);
}

// DIDEVICEINSTANCEA (580 bytes).
void fill_instance(uint32_t d, DevKind k) {
    std::memset(rt::host_ptr(d + 4), 0, 576);
    const uint32_t g = k == DevKind::Keyboard ? kSysKeyboard : kSysMouse;
    write_guid(d + 4, g);
    write_guid(d + 20, g);
    wr32(d + 36, k == DevKind::Keyboard ? DI8DEVTYPE_KEYBOARD | 0x100 : DI8DEVTYPE_MOUSE | 0x100);
    copy_out(d + 40, 260, k == DevKind::Keyboard ? "Keyboard" : "Mouse");
    copy_out(d + 300, 260, k == DevKind::Keyboard ? "Keyboard" : "Mouse");
}

// SDL scancode -> DirectInput (PC set 1) scancode.
uint8_t dik_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        return letters[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return uint8_t(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return uint8_t(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_MINUS: return 0x0C;
        case SDL_SCANCODE_EQUALS: return 0x0D;
        case SDL_SCANCODE_BACKSPACE: return 0x0E;
        case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
        case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_RETURN: return 0x1C;
        case SDL_SCANCODE_LCTRL: return 0x1D;
        case SDL_SCANCODE_SEMICOLON: return 0x27;
        case SDL_SCANCODE_APOSTROPHE: return 0x28;
        case SDL_SCANCODE_GRAVE: return 0x29;
        case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_BACKSLASH: return 0x2B;
        case SDL_SCANCODE_COMMA: return 0x33;
        case SDL_SCANCODE_PERIOD: return 0x34;
        case SDL_SCANCODE_SLASH: return 0x35;
        case SDL_SCANCODE_RSHIFT: return 0x36;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
        case SDL_SCANCODE_LALT: return 0x38;
        case SDL_SCANCODE_SPACE: return 0x39;
        case SDL_SCANCODE_CAPSLOCK: return 0x3A;
        case SDL_SCANCODE_F11: return 0x57;
        case SDL_SCANCODE_F12: return 0x58;
        case SDL_SCANCODE_KP_ENTER: return 0x9C;
        case SDL_SCANCODE_RCTRL: return 0x9D;
        case SDL_SCANCODE_RALT: return 0xB8;
        case SDL_SCANCODE_HOME: return 0xC7;
        case SDL_SCANCODE_UP: return 0xC8;
        case SDL_SCANCODE_PAGEUP: return 0xC9;
        case SDL_SCANCODE_LEFT: return 0xCB;
        case SDL_SCANCODE_RIGHT: return 0xCD;
        case SDL_SCANCODE_END: return 0xCF;
        case SDL_SCANCODE_DOWN: return 0xD0;
        case SDL_SCANCODE_PAGEDOWN: return 0xD1;
        case SDL_SCANCODE_INSERT: return 0xD2;
        case SDL_SCANCODE_DELETE: return 0xD3;
        default: return 0;
    }
}

// ---- IDirectInputDevice8A ----

uint32_t dev_get_caps(X86*, const Args& a) {  // (DIDEVCAPS*)
    std::lock_guard lk(g_mu);
    const Device& d = g_devices[a[0]];
    const uint32_t p = a[1];
    const uint32_t size = rd32(p);
    std::memset(rt::host_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    wr32(p + 4, 1);  // DIDC_ATTACHED
    wr32(p + 8, d.kind == DevKind::Keyboard ? DI8DEVTYPE_KEYBOARD | 0x100 : DI8DEVTYPE_MOUSE | 0x100);
    wr32(p + 12, d.kind == DevKind::Keyboard ? 0 : 3);    // axes
    wr32(p + 16, d.kind == DevKind::Keyboard ? 128 : 5);  // buttons
    return DI_OK;
}
uint32_t dev_ok(X86*, const Args&) { return DI_OK; }
uint32_t dev_unsupported(X86*, const Args&) { return DIERR_UNSUPPORTED; }
uint32_t dev_set_data_format(X86*, const Args& a) {  // (LPCDIDATAFORMAT)
    std::lock_guard lk(g_mu);
    g_devices[a[0]].data_size = a[1] ? rd32(a[1] + 12) : 0;
    return DI_OK;
}
uint32_t dev_get_state(X86*, const Args& a) {  // (cbData, lpvData)
    Device d;
    {
        std::lock_guard lk(g_mu);
        d = g_devices[a[0]];
    }
    const uint32_t size = a[1], out = a[2];
    if (!out) return DIERR_INVALIDPARAM;
    std::memset(rt::host_ptr(out), 0, size);
    if (d.kind == DevKind::Keyboard) {
        int n = 0;
        const bool* keys = SDL_GetKeyboardState(&n);
        for (int s = 0; keys && s < n; ++s)
            if (keys[s])
                if (const uint8_t dik = dik_of(SDL_Scancode(s)); dik && dik < size) wr8(out + dik, 0x80);
    } else {
        float dx = 0, dy = 0;
        const SDL_MouseButtonFlags b = SDL_GetRelativeMouseState(&dx, &dy);
        if (size >= 12) {
            wr32(out + 0, uint32_t(int32_t(dx)));
            wr32(out + 4, uint32_t(int32_t(dy)));
        }
        for (uint32_t i = 0; i < 5 && 12 + i < size; ++i)
            if (b & SDL_BUTTON_MASK(i + 1)) wr8(out + 12 + i, 0x80);
    }
    return DI_OK;
}
uint32_t dev_get_data(X86*, const Args& a) {  // (cbObjectData, rgdod, pdwInOut, flags): buffered input
    if (a[3]) wr32(a[3], 0);
    return DI_OK;
}
uint32_t dev_get_info(X86*, const Args& a) {  // (DIDEVICEINSTANCEA*)
    std::lock_guard lk(g_mu);
    fill_instance(a[1], g_devices[a[0]].kind);
    return DI_OK;
}
uint32_t dev_poll(X86*, const Args&) { return DI_NOEFFECT; }

uint32_t device_vtable() {
    static uint32_t vt = make_vtable({
        &COM_FN<2, com_qi_self>, &COM_FN<0, com_addref>, &COM_FN<0, com_release>,
        &COM_FN<1, dev_get_caps>,        // GetCapabilities
        &COM_FN<3, dev_ok>,              // EnumObjects
        &COM_FN<2, dev_unsupported>,     // GetProperty
        &COM_FN<2, dev_ok>,              // SetProperty
        &COM_FN<0, dev_ok>,              // Acquire
        &COM_FN<0, dev_ok>,              // Unacquire
        &COM_FN<2, dev_get_state>,       // GetDeviceState
        &COM_FN<4, dev_get_data>,        // GetDeviceData
        &COM_FN<1, dev_set_data_format>, // SetDataFormat
        &COM_FN<1, dev_ok>,              // SetEventNotification
        &COM_FN<2, dev_ok>,              // SetCooperativeLevel
        &COM_FN<3, dev_unsupported>,     // GetObjectInfo
        &COM_FN<1, dev_get_info>,        // GetDeviceInfo
        &COM_FN<2, dev_ok>,              // RunControlPanel
        &COM_FN<3, dev_ok>,              // Initialize
        &COM_FN<4, dev_unsupported>,     // CreateEffect
        &COM_FN<3, dev_ok>,              // EnumEffects
        &COM_FN<2, dev_unsupported>,     // GetEffectInfo
        &COM_FN<1, dev_unsupported>,     // GetForceFeedbackState
        &COM_FN<1, dev_unsupported>,     // SendForceFeedbackCommand
        &COM_FN<3, dev_ok>,              // EnumCreatedEffectObjects
        &COM_FN<1, dev_unsupported>,     // Escape
        &COM_FN<0, dev_poll>,            // Poll
        &COM_FN<4, dev_unsupported>,     // SendDeviceData
        &COM_FN<4, dev_unsupported>,     // EnumEffectsInFile
        &COM_FN<4, dev_unsupported>,     // WriteEffectToFile
        &COM_FN<3, dev_unsupported>,     // BuildActionMap
        &COM_FN<3, dev_unsupported>,     // SetActionMap
        &COM_FN<1, dev_unsupported>,     // GetImageInfo
    });
    return vt;
}

// ---- IDirectInput8A ----

uint32_t di_create_device(X86*, const Args& a) {  // (REFGUID, IDirectInputDevice8A**, outer)
    const uint32_t first = a[1] ? rd32(a[1]) : 0;
    if (first != kSysKeyboard && first != kSysMouse) {
        if (a[2]) wr32(a[2], 0);
        return DIERR_DEVICENOTREG;
    }
    const uint32_t o = new_object(device_vtable(), 0);
    {
        std::lock_guard lk(g_mu);
        g_devices[o] = Device{first == kSysKeyboard ? DevKind::Keyboard : DevKind::Mouse};
    }
    if (a[2]) wr32(a[2], o);
    return DI_OK;
}
uint32_t di_enum_devices(X86* c, const Args& a) {  // (devType, callback, pvRef, flags)
    const uint32_t cls = a[1] & 0xFF;
    const uint32_t inst = rt::heap_alloc(580, true);
    wr32(inst, 580);
    for (DevKind k : {DevKind::Keyboard, DevKind::Mouse}) {
        const bool want = cls == DI8DEVCLASS_ALL || (k == DevKind::Keyboard && cls == DI8DEVCLASS_KEYBOARD) ||
                          (k == DevKind::Mouse && cls == DI8DEVCLASS_POINTER);
        if (!want) continue;
        fill_instance(inst, k);
        if (callback(c, a[2], {inst, a[3]}) == 0) break;  // DIENUM_STOP
    }
    rt::heap_free(inst);
    return DI_OK;
}
uint32_t di_device_status(X86*, const Args& a) {
    const uint32_t first = a[1] ? rd32(a[1]) : 0;
    return first == kSysKeyboard || first == kSysMouse ? DI_OK : 1;  // DI_NOTATTACHED
}
uint32_t di_ok(X86*, const Args&) { return DI_OK; }
uint32_t di_unsupported(X86*, const Args&) { return DIERR_UNSUPPORTED; }

uint32_t dinput_vtable() {
    static uint32_t vt = make_vtable({
        &COM_FN<2, com_qi_self>, &COM_FN<0, com_addref>, &COM_FN<0, com_release>,
        &COM_FN<3, di_create_device>,  // CreateDevice
        &COM_FN<4, di_enum_devices>,   // EnumDevices
        &COM_FN<1, di_device_status>,  // GetDeviceStatus
        &COM_FN<2, di_ok>,             // RunControlPanel
        &COM_FN<2, di_ok>,             // Initialize
        &COM_FN<3, di_unsupported>,    // FindDevice
        &COM_FN<5, di_unsupported>,    // EnumDevicesBySemantics
        &COM_FN<4, di_unsupported>,    // ConfigureDevices
    });
    return vt;
}

}  // namespace

STDCALL(dinput8, DirectInput8Create, 5) {  // (hinst, version, riid, ppvOut, punkOuter)
    if (!a[3]) return DIERR_INVALIDPARAM;
    wr32(a[3], new_object(dinput_vtable(), 0));
    return DI_OK;
}
