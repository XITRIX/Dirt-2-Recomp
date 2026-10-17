// OpenAL 1.1 (cdecl) passed through to OpenAL Soft. Devices and contexts are
// host pointers, so the game gets small handles; strings returned by OpenAL
// are copied into guest memory once. EAX and EFX are reported unavailable.
#include <AL/al.h>
#include <AL/alc.h>

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../shim.h"

using namespace shim;

namespace {

std::mutex g_mu;
std::unordered_map<uint32_t, void*> g_handles;  // guest handle -> ALCdevice*/ALCcontext*
std::unordered_map<void*, uint32_t> g_rev;
uint32_t g_next = 0x00A10000;
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_strings;  // (handle, enum) -> guest string

uint32_t to_guest(void* p) {
    if (!p) return 0;
    std::lock_guard lk(g_mu);
    if (auto it = g_rev.find(p); it != g_rev.end()) return it->second;
    const uint32_t h = g_next += 0x10;
    g_handles[h] = p;
    g_rev[p] = h;
    return h;
}
template <typename T>
T* from_guest(uint32_t h) {
    std::lock_guard lk(g_mu);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : static_cast<T*>(it->second);
}
void forget(uint32_t h) {
    std::lock_guard lk(g_mu);
    if (auto it = g_handles.find(h); it != g_handles.end()) {
        g_rev.erase(it->second);
        g_handles.erase(it);
    }
}

// Copies a (possibly double-NUL-terminated list) string to guest memory, cached.
uint32_t guest_string(uint32_t handle, uint32_t e, const char* s, bool list) {
    if (!s) return 0;
    std::lock_guard lk(g_mu);
    if (auto it = g_strings.find({handle, e}); it != g_strings.end()) return it->second;
    size_t len = std::strlen(s);
    if (list)
        while (s[len + 1]) len += 1 + std::strlen(s + len + 1);
    const uint32_t g = rt::heap_alloc(uint32_t(len + 2), true);
    std::memcpy(rt::host_ptr(g), s, len + 1);
    g_strings[{handle, e}] = g;
    return g;
}

float f(uint32_t bits) {
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
template <typename T>
T* p(uint32_t g) {
    return g ? reinterpret_cast<T*>(rt::host_ptr(g)) : nullptr;
}

bool extension_hidden(const char* name) {
    // EAX and EFX need shim support for their function tables; until then
    // the game uses plain OpenAL. Rapture3D licensing extensions are absent too.
    return !name || strncasecmp(name, "EAX", 3) == 0 || strcasecmp(name, "ALC_EXT_EFX") == 0 ||
           strncasecmp(name, "ALC_EXT_BRS", 11) == 0;
}

}  // namespace

// ---- ALC ----
CDECL(openal32, alcOpenDevice) {
    ALCdevice* d = alcOpenDevice(nullptr);  // the default device, whatever name the game asks for
    rt::log("alcOpenDevice(%s) -> %p", a.str(0) ? a.str(0) : "default", static_cast<void*>(d));
    return to_guest(d);
}
CDECL(openal32, alcCloseDevice) {
    ALCdevice* d = from_guest<ALCdevice>(a[0]);
    forget(a[0]);
    return d ? alcCloseDevice(d) : ALC_FALSE;
}
CDECL(openal32, alcCreateContext) {
    ALCdevice* d = from_guest<ALCdevice>(a[0]);
    return d ? to_guest(alcCreateContext(d, p<const ALCint>(a[1]))) : 0;
}
CDECL(openal32, alcDestroyContext) {
    if (ALCcontext* ctx = from_guest<ALCcontext>(a[0])) alcDestroyContext(ctx);
    forget(a[0]);
    return 0;
}
CDECL(openal32, alcMakeContextCurrent) { return alcMakeContextCurrent(from_guest<ALCcontext>(a[0])); }
CDECL(openal32, alcProcessContext) {
    if (ALCcontext* ctx = from_guest<ALCcontext>(a[0])) alcProcessContext(ctx);
    return 0;
}
CDECL(openal32, alcSuspendContext) {
    if (ALCcontext* ctx = from_guest<ALCcontext>(a[0])) alcSuspendContext(ctx);
    return 0;
}
CDECL(openal32, alcGetError) { return uint32_t(alcGetError(from_guest<ALCdevice>(a[0]))); }
CDECL(openal32, alcGetIntegerv) {
    alcGetIntegerv(from_guest<ALCdevice>(a[0]), ALCenum(a[1]), ALCsizei(a[2]), p<ALCint>(a[3]));
    return 0;
}
CDECL(openal32, alcGetString) {
    ALCdevice* d = from_guest<ALCdevice>(a[0]);
    const ALCenum e = ALCenum(a[1]);
    const bool list = !d && (e == ALC_DEVICE_SPECIFIER || e == 0x1013 /*ALC_ALL_DEVICES_SPECIFIER*/ ||
                             e == ALC_CAPTURE_DEVICE_SPECIFIER);
    return guest_string(a[0], uint32_t(e), alcGetString(d, e), list);
}
CDECL(openal32, alcIsExtensionPresent) {
    if (extension_hidden(a.str(1))) return ALC_FALSE;
    return alcIsExtensionPresent(from_guest<ALCdevice>(a[0]), a.str(1));
}
CDECL(openal32, alcGetEnumValue) { return uint32_t(alcGetEnumValue(from_guest<ALCdevice>(a[0]), a.str(1))); }
CDECL(openal32, alcGetProcAddress) {
    rt::log("alcGetProcAddress(%s): not provided", a.str(1) ? a.str(1) : "");
    return 0;
}

// ---- AL ----
CDECL(openal32, alGetError) { return uint32_t(alGetError()); }
CDECL(openal32, alGetString) { return guest_string(0, a[0], reinterpret_cast<const char*>(alGetString(ALenum(a[0]))), false); }
CDECL(openal32, alIsExtensionPresent) {
    if (extension_hidden(a.str(0))) return AL_FALSE;
    return alIsExtensionPresent(a.str(0));
}
CDECL(openal32, alGetEnumValue) { return uint32_t(alGetEnumValue(a.str(0))); }
CDECL(openal32, alGetProcAddress) {
    rt::log("alGetProcAddress(%s): not provided", a.str(0) ? a.str(0) : "");
    return 0;
}
CDECL(openal32, alDopplerFactor) {
    alDopplerFactor(f(a[0]));
    return 0;
}
CDECL(openal32, alDistanceModel) {
    alDistanceModel(ALenum(a[0]));
    return 0;
}
CDECL(openal32, alGenSources) {
    alGenSources(ALsizei(a[0]), p<ALuint>(a[1]));
    return 0;
}
CDECL(openal32, alDeleteSources) {
    alDeleteSources(ALsizei(a[0]), p<const ALuint>(a[1]));
    return 0;
}
CDECL(openal32, alGenBuffers) {
    alGenBuffers(ALsizei(a[0]), p<ALuint>(a[1]));
    return 0;
}
CDECL(openal32, alDeleteBuffers) {
    alDeleteBuffers(ALsizei(a[0]), p<const ALuint>(a[1]));
    return 0;
}
CDECL(openal32, alBufferData) {  // (buffer, format, data, size, freq)
    alBufferData(ALuint(a[0]), ALenum(a[1]), p<const void>(a[2]), ALsizei(a[3]), ALsizei(a[4]));
    return 0;
}
CDECL(openal32, alSourcei) {
    alSourcei(ALuint(a[0]), ALenum(a[1]), ALint(a[2]));
    return 0;
}
CDECL(openal32, alSource3i) {
    alSource3i(ALuint(a[0]), ALenum(a[1]), ALint(a[2]), ALint(a[3]), ALint(a[4]));
    return 0;
}
CDECL(openal32, alSourcef) {
    alSourcef(ALuint(a[0]), ALenum(a[1]), f(a[2]));
    return 0;
}
CDECL(openal32, alSource3f) {
    alSource3f(ALuint(a[0]), ALenum(a[1]), f(a[2]), f(a[3]), f(a[4]));
    return 0;
}
CDECL(openal32, alSourcefv) {
    alSourcefv(ALuint(a[0]), ALenum(a[1]), p<const ALfloat>(a[2]));
    return 0;
}
CDECL(openal32, alGetSourcei) {
    alGetSourcei(ALuint(a[0]), ALenum(a[1]), p<ALint>(a[2]));
    return 0;
}
CDECL(openal32, alSourcePlay) {
    alSourcePlay(ALuint(a[0]));
    return 0;
}
CDECL(openal32, alSourceStop) {
    alSourceStop(ALuint(a[0]));
    return 0;
}
CDECL(openal32, alSourcePause) {
    alSourcePause(ALuint(a[0]));
    return 0;
}
CDECL(openal32, alSourceRewind) {
    alSourceRewind(ALuint(a[0]));
    return 0;
}
CDECL(openal32, alSourceQueueBuffers) {
    alSourceQueueBuffers(ALuint(a[0]), ALsizei(a[1]), p<const ALuint>(a[2]));
    return 0;
}
CDECL(openal32, alSourceUnqueueBuffers) {
    alSourceUnqueueBuffers(ALuint(a[0]), ALsizei(a[1]), p<ALuint>(a[2]));
    return 0;
}
