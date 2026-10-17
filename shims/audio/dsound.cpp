// dsound: only GetDeviceID is imported (hardware detection). Audio itself
// goes through OpenAL.
#include "../shim.h"

STDCALL_ORD(dsound, 9, 2) {  // GetDeviceID(const GUID* src, GUID* dst)
    static const uint8_t kDevice[16] = {0x44, 0x49, 0x52, 0x54, 0x32, 0x00, 0x00, 0x10,
                                        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    if (!a[1]) return 0x80070057;  // E_INVALIDARG
    std::memcpy(rt::host_ptr(a[1]), kDevice, 16);
    return 0;  // DS_OK
}

// Resolved with GetProcAddress for CLSID_DirectSoundPrivate (device property
// enumeration during hardware detection); the game skips it on failure.
STDCALL(dsound, DllGetClassObject, 3) {
    if (a[2]) wr32(a[2], 0);
    return 0x80040111;  // CLASS_E_CLASSNOTAVAILABLE
}
