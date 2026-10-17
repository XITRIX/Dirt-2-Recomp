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
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../shim.h"
#include "../vfs.h"

using namespace shim;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kHbinkSize = 0x400;

struct Player {
    uint32_t hbink = 0;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    int vstream = -1, astream = -1;
    AVFrame* frame = nullptr;   // last decoded video frame
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    SDL_AudioStream* audio = nullptr;
    uint32_t width = 0, height = 0, frames = 0;
    AVRational rate{30, 1};
    uint32_t buffers = 0;       // guest BINKFRAMEBUFFERS registered by the game
    uint32_t next_set = 0;
    // Timing: wall-clock time at which frame 1 is due; paused time is excluded.
    Clock::time_point start{};
    bool started = false, paused = false;
    Clock::time_point pause_at{};
    float volume = 1.0f;
    std::mutex mu;

    ~Player() {
        if (audio) SDL_DestroyAudioStream(audio);
        av_frame_free(&frame);
        av_frame_free(&aframe);
        av_packet_free(&pkt);
        avcodec_free_context(&vdec);
        avcodec_free_context(&adec);
        avformat_close_input(&fmt);
    }

    double frame_seconds() const { return double(rate.den) / double(rate.num); }

    uint32_t frame_num() const { return rd32(hbink + 0x0C); }
    void set_frame_num(uint32_t n) {
        wr32(hbink + 0x10, rd32(hbink + 0x0C));
        wr32(hbink + 0x0C, n);
    }

    void push_audio(const AVFrame* f) {
        if (!audio || !f->nb_samples) return;
        const int ch = f->ch_layout.nb_channels;
        std::vector<float> interleaved(size_t(f->nb_samples) * ch);
        if (f->format == AV_SAMPLE_FMT_FLTP) {
            for (int s = 0; s < f->nb_samples; ++s)
                for (int c = 0; c < ch; ++c) interleaved[size_t(s) * ch + c] = reinterpret_cast<const float*>(f->data[c])[s];
        } else if (f->format == AV_SAMPLE_FMT_FLT) {
            std::memcpy(interleaved.data(), f->data[0], interleaved.size() * 4);
        } else {
            return;
        }
        SDL_PutAudioStreamData(audio, interleaved.data(), int(interleaved.size() * 4));
    }

    // Reads packets until one video frame is decoded. Audio packets met on the way are played.
    bool decode_video() {
        for (;;) {
            if (avcodec_receive_frame(vdec, frame) == 0) return true;
            const int r = av_read_frame(fmt, pkt);
            if (r < 0) {
                avcodec_send_packet(vdec, nullptr);  // drain
                return avcodec_receive_frame(vdec, frame) == 0;
            }
            if (pkt->stream_index == vstream) {
                avcodec_send_packet(vdec, pkt);
            } else if (pkt->stream_index == astream && adec) {
                if (avcodec_send_packet(adec, pkt) == 0)
                    while (avcodec_receive_frame(adec, aframe) == 0) push_audio(aframe);
            }
            av_packet_unref(pkt);
        }
    }

    void rewind() {
        av_seek_frame(fmt, vstream, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(vdec);
        if (adec) avcodec_flush_buffers(adec);
        if (audio) SDL_ClearAudioStream(audio);
        started = false;
    }

    // Copies the decoded frame into the next registered buffer set.
    void write_buffers() {
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            rt::log("bink: write_buffers bufs=%08X fmt=%d", buffers, frame->format);
            if (buffers)
                for (int set = 0; set < 2; ++set)
                    for (int pl = 0; pl < 4; ++pl) {
                        const uint32_t e = buffers + 24 + set * 48 + pl * 12;
                        rt::log("  set %d plane %d: alloc %u buf %08X pitch %u", set, pl, rd32(e), rd32(e + 4), rd32(e + 8));
                    }
        }
        if (!buffers) return;
        for (uint32_t set = 0; set < 2; ++set) write_set(set);
        if (std::getenv("DIRT2_LOG_LOCKS")) {
            const uint32_t y = rd32(buffers + 24 + 4), pitch = rd32(buffers + 24 + 8);
            uint64_t sum = 0;
            for (uint32_t x = 0; x < width; ++x) sum += rd8(y + (height / 2) * pitch + x);
            rt::log("bink: frame %u Y mid-row sum %llu (src linesize %d)", frame_num(), (unsigned long long)sum, frame->linesize[0]);
        }
        wr32(buffers + 20, next_set);
        next_set ^= 1;
    }

    void write_set(uint32_t set) {
        const uint32_t planes = buffers + 24 + set * 48;
        // Plane order in the set: Y, cR (V), cB (U), A.
        const int src_plane[4] = {0, 2, 1, 3};
        for (int p = 0; p < 4; ++p) {
            const uint32_t pl = planes + 12 * uint32_t(p);
            const uint32_t buf = rd32(pl + 4), pitch = rd32(pl + 8);
            if (!buf || !pitch) continue;
            const int sp = src_plane[p];
            if (sp == 3 && !frame->data[3]) {  // no alpha in the stream: opaque
                const uint32_t h = height;
                for (uint32_t y = 0; y < h; ++y) std::memset(rt::host_ptr(buf + y * pitch), 0xFF, std::min(pitch, width));
                continue;
            }
            if (!frame->data[sp]) continue;
            const uint32_t w = sp == 0 || sp == 3 ? width : (width + 1) / 2;
            const uint32_t h = sp == 0 || sp == 3 ? height : (height + 1) / 2;
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(rt::host_ptr(buf + y * pitch), frame->data[sp] + size_t(y) * frame->linesize[sp], std::min(pitch, w));
        }
    }
};

std::mutex g_mu;
std::unordered_map<uint32_t, std::shared_ptr<Player>> g_players;

std::shared_ptr<Player> player(uint32_t hbink) {
    std::lock_guard lk(g_mu);
    auto it = g_players.find(hbink);
    return it == g_players.end() ? nullptr : it->second;
}

AVCodecContext* open_decoder(AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->thread_count = 0;  // automatic frame/slice threading
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

bool g_audio_init = false;

}  // namespace

// Decorated stdcall names (_Name@bytes).
#define BINK(name, bytes) SHIM_DEFINE(imp_binkw32__##name##_##bytes, binkw32, "_" #name "@" #bytes, 0, bytes)

BINK(BinkOpen, 8) {  // (const char* name, flags) -> HBINK
    const std::string win = a.str(0) ? a.str(0) : "";
    const std::string host = vfs::host_path(win);
    auto p = std::make_shared<Player>();
    if (avformat_open_input(&p->fmt, host.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(p->fmt, nullptr) < 0) {
        rt::log("BinkOpen(%s): cannot open", win.c_str());
        return 0;
    }
    p->vstream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    p->astream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (p->vstream < 0 || !(p->vdec = open_decoder(p->fmt->streams[p->vstream]))) {
        rt::log("BinkOpen(%s): no decodable video stream", win.c_str());
        return 0;
    }
    AVStream* vs = p->fmt->streams[p->vstream];
    p->width = uint32_t(p->vdec->width);
    p->height = uint32_t(p->vdec->height);
    // FFmpeg does not report nb_frames for Bink; the file header has it (offset 8).
    p->frames = vs->nb_frames > 0 ? uint32_t(vs->nb_frames) : 0;
    if (!p->frames)
        if (FILE* f = std::fopen(host.c_str(), "rb")) {
            uint32_t hdr[3] = {};
            if (std::fread(hdr, 4, 3, f) == 3) p->frames = hdr[2];
            std::fclose(f);
        }
    if (!p->frames) p->frames = 1;
    // Videos are skipped unless DIRT2_PLAY_VIDEOS=1: the clip reports a single
    // frame, so the intro sequence finishes at once. (Playback decodes and
    // uploads correctly but renders green in-game: see docs/STATUS.md.)
    static const bool play = std::getenv("DIRT2_PLAY_VIDEOS") != nullptr;
    if (!play) {
        p->frames = 1;
        if (p->astream >= 0) p->astream = -1;  // and silent
    }
    p->rate = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    if (!p->rate.num) p->rate = {30, 1};
    p->frame = av_frame_alloc();
    p->aframe = av_frame_alloc();
    p->pkt = av_packet_alloc();

    if (p->astream >= 0 && (p->adec = open_decoder(p->fmt->streams[p->astream]))) {
        if (!g_audio_init) g_audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO);
        const SDL_AudioSpec spec{SDL_AUDIO_F32, p->adec->ch_layout.nb_channels, p->adec->sample_rate};
        p->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!p->audio) rt::log("BinkOpen: no audio output: %s", SDL_GetError());
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
    static const bool play = std::getenv("DIRT2_PLAY_VIDEOS") != nullptr;
    if (play) {
        if (p->decode_video()) p->write_buffers();
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
        if (!p->decode_video()) break;
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
    const bool alpha = p->vdec->pix_fmt == AV_PIX_FMT_YUVA420P;
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
