#include "bink_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bink {

namespace {

AVCodecContext* open_codec(AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->thread_count = 0;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

}  // namespace

Decoder::~Decoder() { close(); }

void Decoder::close() {
    av_frame_free(&frame_);
    av_frame_free(&aframe_);
    av_packet_free(&pkt_);
    avcodec_free_context(&vdec_);
    avcodec_free_context(&adec_);
    avformat_close_input(&fmt_);
}

bool Decoder::open(const std::string& path, bool with_audio) {
    close();
    // Through FFmpeg's file protocol explicitly: on the Switch, "sdmc:/..." reads
    // as a URL with an unknown "sdmc" protocol and the open fails.
    const std::string url = "file:" + path;
    if (avformat_open_input(&fmt_, url.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(fmt_, nullptr) < 0)
        return false;
    vstream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream_ < 0 || !(vdec_ = open_codec(fmt_->streams[vstream_]))) return false;
    if (with_audio) {
        astream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (astream_ >= 0) adec_ = open_codec(fmt_->streams[astream_]);
    }
    AVStream* vs = fmt_->streams[vstream_];
    width_ = uint32_t(vdec_->width);
    height_ = uint32_t(vdec_->height);
    const AVRational r = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    rate_num_ = r.num ? r.num : 30;
    rate_den_ = r.num ? r.den : 1;
    // FFmpeg does not report nb_frames for Bink; the file header has it (offset 8).
    frames_ = vs->nb_frames > 0 ? uint32_t(vs->nb_frames) : 0;
    if (!frames_)
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            uint32_t hdr[3] = {};
            if (std::fread(hdr, 4, 3, f) == 3) frames_ = hdr[2];
            std::fclose(f);
        }
    if (!frames_) frames_ = 1;
    frame_ = av_frame_alloc();
    aframe_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    return true;
}

bool Decoder::has_alpha() const { return vdec_ && vdec_->pix_fmt == AV_PIX_FMT_YUVA420P; }
int Decoder::audio_channels() const { return adec_ ? adec_->ch_layout.nb_channels : 0; }
int Decoder::audio_rate() const { return adec_ ? adec_->sample_rate : 0; }

bool Decoder::decode_next() {
    for (;;) {
        if (avcodec_receive_frame(vdec_, frame_) == 0) return true;
        if (av_read_frame(fmt_, pkt_) < 0) {
            avcodec_send_packet(vdec_, nullptr);
            return avcodec_receive_frame(vdec_, frame_) == 0;
        }
        if (pkt_->stream_index == vstream_) {
            avcodec_send_packet(vdec_, pkt_);
        } else if (pkt_->stream_index == astream_ && adec_) {
            if (avcodec_send_packet(adec_, pkt_) == 0)
                while (avcodec_receive_frame(adec_, aframe_) == 0) {
                    if (!audio_sink_ || !aframe_->nb_samples) continue;
                    const int ch = aframe_->ch_layout.nb_channels;
                    std::vector<float> inter(size_t(aframe_->nb_samples) * ch);
                    if (aframe_->format == AV_SAMPLE_FMT_FLTP) {
                        for (int s = 0; s < aframe_->nb_samples; ++s)
                            for (int c = 0; c < ch; ++c)
                                inter[size_t(s) * ch + c] = reinterpret_cast<const float*>(aframe_->data[c])[s];
                    } else if (aframe_->format == AV_SAMPLE_FMT_FLT) {
                        std::memcpy(inter.data(), aframe_->data[0], inter.size() * 4);
                    } else {
                        continue;
                    }
                    audio_sink_(inter.data(), aframe_->nb_samples, ch, aframe_->sample_rate);
                }
        }
        av_packet_unref(pkt_);
    }
}

void Decoder::copy_planes(const Plane planes[4]) const {
    // Bink order Y, cR (V), cB (U), A  <-  FFmpeg planes Y, U, V, A.
    const int src[4] = {0, 2, 1, 3};
    for (int p = 0; p < 4; ++p) {
        const Plane& d = planes[p];
        if (!d.buf || !d.pitch) continue;
        const bool full = p == 0 || p == 3;
        const uint32_t w = full ? width_ : (width_ + 1) / 2;
        const uint32_t h = full ? height_ : (height_ + 1) / 2;
        const uint8_t* s = frame_->data[src[p]];
        if (!s) {
            if (p == 3)
                for (uint32_t y = 0; y < h; ++y) std::memset(d.buf + size_t(y) * d.pitch, 0xFF, std::min(d.pitch, w));
            continue;
        }
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(d.buf + size_t(y) * d.pitch, s + size_t(y) * frame_->linesize[src[p]], std::min(d.pitch, w));
    }
}

void Decoder::rewind() {
    av_seek_frame(fmt_, vstream_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vdec_);
    if (adec_) avcodec_flush_buffers(adec_);
}

}  // namespace bink
