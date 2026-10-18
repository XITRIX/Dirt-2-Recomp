// binkw32 on FFmpeg: Bink 1 video (binkvideo) and audio (binkaudio_*).
//
// The game uses frame-buffer mode: it asks for the YUV plane layout
// (BinkGetFrameBuffersInfo), allocates the planes itself, registers them
// (BinkRegisterFrameBuffers), then per frame calls BinkWait / BinkDoFrame /
// BinkNextFrame and uploads the planes named by BINKFRAMEBUFFERS.FrameNum to
// textures. Audio is decoded alongside and played through an SDL audio stream.
//
// HBINK is a guest block whose leading fields match Bink 1.9: Width, Height,
// Frames, FrameNum (1-based, the frame BinkDoFrame decodes next), LastFrameNum,
// FrameRate, FrameRateDiv. The host-side player is looked up by its address.
#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/rational.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../shim.h"
#include "../vfs.h"
#include "bink_decoder.h"

using namespace shim;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kHbinkSize = 0x400;

struct Player {
    uint32_t hbink = 0;
    bink::Decoder dec;
    SDL_AudioStream* audio = nullptr;
    uint32_t width = 0, height = 0, frames = 0;
    AVRational rate{30, 1};
    uint32_t buffers = 0;       // guest BINKFRAMEBUFFERS registered by the game
    uint32_t next_set = 0;
    // Timing: wall-clock time at which frame 1 is due; paused time is excluded.
    Clock::time_point start{};
    bool play = true;           // false: skipped (one black, silent frame)
    bool started = false, paused = false;
    Clock::time_point pause_at{};
    std::mutex mu;

    ~Player() {
        if (audio) SDL_DestroyAudioStream(audio);
    }

    double frame_seconds() const { return double(rate.den) / double(rate.num); }

    uint32_t frame_num() const { return rd32(hbink + 0x0C); }
    void set_frame_num(uint32_t n) {
        wr32(hbink + 0x10, rd32(hbink + 0x0C));
        wr32(hbink + 0x0C, n);
    }

    void rewind() {
        dec.rewind();
        if (audio) SDL_ClearAudioStream(audio);
        started = false;
    }

    // Copies the decoded frame into both registered buffer sets.
    void write_buffers() {
        if (!buffers) return;
        for (uint32_t set = 0; set < 2; ++set) {
            bink::Plane planes[4];
            for (uint32_t p = 0; p < 4; ++p) {
                const uint32_t e = buffers + 24 + set * 48 + p * 12;
                const uint32_t buf = rd32(e + 4), pitch = rd32(e + 8);
                if (buf && pitch) planes[p] = {static_cast<uint8_t*>(rt::host_ptr(buf)), pitch};
            }
            dec.copy_planes(planes);
        }
        if (std::getenv("DIRT2_LOG_LOCKS")) {
            const uint32_t y = rd32(buffers + 24 + 4), pitch = rd32(buffers + 24 + 8);
            uint64_t sum = 0;
            for (uint32_t x = 0; x < width; ++x) sum += rd8(y + (height / 2) * pitch + x);
            rt::log("bink: frame %u Y mid-row sum %llu", frame_num(), (unsigned long long)sum);
        }
        wr32(buffers + 20, next_set);
        next_set ^= 1;
    }
};

std::mutex g_mu;
std::unordered_map<uint32_t, std::shared_ptr<Player>> g_players;

std::shared_ptr<Player> player(uint32_t hbink) {
    std::lock_guard lk(g_mu);
    auto it = g_players.find(hbink);
    return it == g_players.end() ? nullptr : it->second;
}

bool g_audio_init = false;

}  // namespace

// Decorated stdcall names (_Name@bytes).
#define BINK(name, bytes) SHIM_DEFINE(imp_binkw32__##name##_##bytes, binkw32, "_" #name "@" #bytes, 0, bytes)

BINK(BinkOpen, 8) {  // (const char* name, flags) -> HBINK
    const std::string win = a.str(0) ? a.str(0) : "";
    const std::string host = vfs::host_path(win);
    auto p = std::make_shared<Player>();
    // The start-up stings (publisher and hardware logos) are skipped: the clip
    // reports a single frame, so the intro sequence finishes at once, silently.
    // Everything else plays (the trailer TV, tutorials, attract).
    // DIRT2_PLAY_VIDEOS=1 plays the stings too, =0 skips every video.
    const char* env = std::getenv("DIRT2_PLAY_VIDEOS");
    std::string file = win.substr(win.find_last_of("\\/") + 1);
    std::transform(file.begin(), file.end(), file.begin(), [](unsigned char ch) { return std::tolower(ch); });
    const bool sting = file.find("sting") != std::string::npos;
    const bool play = env ? std::strcmp(env, "0") != 0 : !sting;
    p->play = play;
    if (!p->dec.open(host, play)) {
        rt::log("BinkOpen(%s): cannot open", win.c_str());
        return 0;
    }
    p->width = p->dec.width();
    p->height = p->dec.height();
    p->frames = play ? p->dec.frames() : 1;
    p->rate = {p->dec.rate_num(), p->dec.rate_den()};

    if (p->dec.audio_channels()) {
        if (!g_audio_init) g_audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO);
        const SDL_AudioSpec spec{SDL_AUDIO_F32, p->dec.audio_channels(), p->dec.audio_rate()};
        p->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!p->audio) rt::log("BinkOpen: no audio output: %s", SDL_GetError());
        if (SDL_AudioStream* s = p->audio)
            p->dec.set_audio_sink([s](const float* x, int n, int ch, int) { SDL_PutAudioStreamData(s, x, n * ch * 4); });
    }

    const uint32_t b = rt::heap_alloc(kHbinkSize, true);
    p->hbink = b;
    wr32(b + 0x00, p->width);
    wr32(b + 0x04, p->height);
    wr32(b + 0x08, p->frames);
    wr32(b + 0x0C, 1);
    wr32(b + 0x10, 1);
    wr32(b + 0x14, uint32_t(p->rate.num));
    wr32(b + 0x18, uint32_t(p->rate.den));
    {
        std::lock_guard lk(g_mu);
        g_players[b] = p;
    }
    rt::log("BinkOpen(%s, 0x%X): %ux%u, %u frames at %.2f fps%s", win.c_str(), a[1], p->width, p->height, p->frames,
            av_q2d(p->rate), p->audio ? ", audio" : "");
    return b;
}

BINK(BinkClose, 4) {
    std::shared_ptr<Player> p;
    {
        std::lock_guard lk(g_mu);
        auto it = g_players.find(a[0]);
        if (it == g_players.end()) return 0;
        p = it->second;
        g_players.erase(it);
    }
    rt::heap_free(a[0]);
    return 0;
}

// Decodes frame FrameNum into the registered buffers.
BINK(BinkDoFrame, 4) {
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    if (!p->started) {
        p->start = Clock::now();
        p->started = true;
        if (p->audio) SDL_ResumeAudioStreamDevice(p->audio);
    }
    if (p->play) {
        if (p->dec.decode_next()) p->write_buffers();
    } else if (p->buffers) {
        // Skipped video: one black frame (Y=16, chroma neutral).
        for (uint32_t set = 0; set < 2; ++set)
            for (uint32_t pl = 0; pl < 3; ++pl) {
                const uint32_t e = p->buffers + 24 + set * 48 + pl * 12;
                const uint32_t buf = rd32(e + 4), pitch = rd32(e + 8);
                const uint32_t h = pl ? (p->height + 1) / 2 : p->height;
                if (buf && pitch) std::memset(rt::host_ptr(buf), pl ? 128 : 16, size_t(pitch) * h);
            }
    }
    return 0;
}

// Advances to the next frame; after the last one Bink loops to frame 1.
BINK(BinkNextFrame, 4) {
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    const uint32_t n = p->frame_num();
    if (n >= p->frames) {
        p->rewind();
        p->set_frame_num(1);
    } else {
        p->set_frame_num(n + 1);
    }
    return 0;
}

// Non-zero while it is too early to show the current frame.
BINK(BinkWait, 4) {
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    if (!p->started) return 0;
    if (p->paused) return 1;
    const double due = (p->frame_num() - 1) * p->frame_seconds();
    const double now = std::chrono::duration<double>(Clock::now() - p->start).count();
    return now < due ? 1 : 0;
}

// Non-zero when playback is more than a frame behind, so the game can drop frames.
BINK(BinkShouldSkip, 4) {
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    if (!p->started || p->paused) return 0;
    const double due = (p->frame_num() - 1) * p->frame_seconds();
    const double now = std::chrono::duration<double>(Clock::now() - p->start).count();
    return now > due + 2 * p->frame_seconds() ? 1 : 0;
}

BINK(BinkPause, 8) {  // (HBINK, pause) -> paused state
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    const bool pause = a[1] != 0;
    if (pause && !p->paused) {
        p->pause_at = Clock::now();
        if (p->audio) SDL_PauseAudioStreamDevice(p->audio);
    } else if (!pause && p->paused) {
        p->start += Clock::now() - p->pause_at;
        if (p->audio) SDL_ResumeAudioStreamDevice(p->audio);
    }
    p->paused = pause;
    return pause ? 1 : 0;
}

// BinkGoto(HBINK, frame, flags): decodes forward from the start up to the frame.
BINK(BinkGoto, 12) {
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    const uint32_t target = std::clamp<uint32_t>(a[1], 1, p->frames);
    p->rewind();
    for (uint32_t f = 1; f < target; ++f)
        if (!p->dec.decode_next()) break;
    if (p->audio) SDL_ClearAudioStream(p->audio);
    p->set_frame_num(target);
    p->start = Clock::now() - std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<double>((target - 1) * p->frame_seconds()));
    p->started = true;
    return 0;
}

BINK(BinkSetVolume, 12) {  // (HBINK, track id, volume 0..32768 = unity)
    auto p = player(a[0]);
    if (p && p->audio) SDL_SetAudioStreamGain(p->audio, float(a[2]) / 32768.0f);
    return 0;
}
BINK(BinkSetMixBins, 16) { return 0; }      // speaker routing: stereo only
BINK(BinkSetMemory, 8) { return 0; }        // custom allocators: FFmpeg uses its own
BINK(BinkSetSoundTrack, 8) { return 0; }    // the first audio track is used
BINK(BinkSetSoundSystem, 8) { return 1; }   // audio goes to SDL, whatever system is named
BINK(BinkOpenDirectSound, 4) { return 1; }  // only ever passed to BinkSetSoundSystem

// BINKFRAMEBUFFERS: TotalFrames, YABufferWidth, YABufferHeight, cRcBBufferWidth,
// cRcBBufferHeight, FrameNum, then 2 x { Y, cR, cB, A } planes of
// { Allocate, Buffer, BufferPitch }. The game allocates the planes it is told to.
BINK(BinkGetFrameBuffersInfo, 8) {  // (HBINK, BINKFRAMEBUFFERS*)
    const uint32_t f = a[1];
    auto p = player(a[0]);
    if (!f || !p) return 0;
    std::memset(rt::host_ptr(f), 0, 24 + 2 * 48);
    wr32(f + 0, 2);
    wr32(f + 4, p->width);
    wr32(f + 8, p->height);
    wr32(f + 12, (p->width + 1) / 2);
    wr32(f + 16, (p->height + 1) / 2);
    const bool alpha = p->dec.has_alpha();
    for (uint32_t set = 0; set < 2; ++set)
        for (uint32_t plane = 0; plane < (alpha ? 4u : 3u); ++plane) wr32(f + 24 + set * 48 + plane * 12, 1);
    return 0;
}

BINK(BinkRegisterFrameBuffers, 8) {  // (HBINK, BINKFRAMEBUFFERS*)
    auto p = player(a[0]);
    if (!p) return 0;
    std::lock_guard lk(p->mu);
    p->buffers = a[1];
    p->next_set = 0;
    return 0;
}
