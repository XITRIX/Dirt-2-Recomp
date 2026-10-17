// Host-only Bink decoder on FFmpeg: no guest memory, no shim plumbing, so it
// can be tested on its own (tests/video). shims/video/bink.cpp drives it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace bink {

// A destination plane: Bink frame-buffer layout (Y, cR, cB, A).
struct Plane {
    uint8_t* buf = nullptr;
    uint32_t pitch = 0;
};

class Decoder {
public:
    // Interleaved float samples, channel count, sample rate.
    using AudioSink = std::function<void(const float* samples, int frames, int channels, int rate)>;

    ~Decoder();
    bool open(const std::string& path, bool with_audio);
    void close();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t frames() const { return frames_; }
    int rate_num() const { return rate_num_; }
    int rate_den() const { return rate_den_; }
    bool has_alpha() const;
    int audio_channels() const;
    int audio_rate() const;

    void set_audio_sink(AudioSink sink) { audio_sink_ = std::move(sink); }

    // Decodes the next video frame (audio met on the way goes to the sink).
    bool decode_next();
    // Copies the last decoded frame into Bink-ordered planes {Y, cR, cB, A}.
    // A missing alpha plane in the stream is written opaque.
    void copy_planes(const Plane planes[4]) const;
    void rewind();

private:
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* vdec_ = nullptr;
    AVCodecContext* adec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* aframe_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int vstream_ = -1, astream_ = -1;
    uint32_t width_ = 0, height_ = 0, frames_ = 0;
    int rate_num_ = 30, rate_den_ = 1;
    AudioSink audio_sink_;
};

}  // namespace bink
