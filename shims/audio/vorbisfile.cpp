// vorbisfile (loaded by the game with LoadLibrary/GetProcAddress) bridged to
// native libvorbisfile. The game's OggVorbis_File is only used as a key: the
// real decoder state lives on the host. Its I/O callbacks are guest functions,
// invoked through a guest-memory staging buffer. All functions are cdecl.
#include <vorbis/vorbisfile.h>

#include <memory>
#include <mutex>
#include <unordered_map>

#include "../shim.h"

using namespace shim;

namespace {

struct Stream {
    OggVorbis_File vf{};
    uint32_t datasource = 0;
    uint32_t read_fn = 0, seek_fn = 0, close_fn = 0, tell_fn = 0;
    uint32_t staging = 0, staging_size = 0;
    uint32_t info = 0, comment = 0;  // guest copies handed out by ov_info / ov_comment
};

std::mutex g_mu;
std::unordered_map<uint32_t, std::unique_ptr<Stream>> g_streams;  // guest OggVorbis_File* -> stream

Stream* stream_of(uint32_t vf) {
    std::lock_guard lk(g_mu);
    auto it = g_streams.find(vf);
    return it == g_streams.end() ? nullptr : it->second.get();
}

uint32_t cdecl_call(uint32_t fn, std::initializer_list<uint32_t> args) {
    return rt::invoke(rt::current_cpu(), fn, args, false);
}

size_t cb_read(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* s = static_cast<Stream*>(user);
    if (!s->read_fn) return 0;
    const size_t bytes = size * nmemb;
    if (bytes > s->staging_size) {
        if (s->staging) rt::heap_free(s->staging);
        s->staging_size = uint32_t(std::max<size_t>(bytes, 64 * 1024));
        s->staging = rt::heap_alloc(s->staging_size);
    }
    const uint32_t n = cdecl_call(s->read_fn, {s->staging, uint32_t(size), uint32_t(nmemb), s->datasource});
    const size_t got = std::min<size_t>(n, nmemb) * size;
    std::memcpy(ptr, rt::host_ptr(s->staging), got);
    return got / (size ? size : 1);
}

int cb_seek(void* user, ogg_int64_t offset, int whence) {
    auto* s = static_cast<Stream*>(user);
    if (!s->seek_fn) return -1;  // not seekable
    return int(cdecl_call(s->seek_fn, {s->datasource, uint32_t(uint64_t(offset)), uint32_t(uint64_t(offset) >> 32), uint32_t(whence)}));
}

int cb_close(void* user) {
    auto* s = static_cast<Stream*>(user);
    return s->close_fn ? int(cdecl_call(s->close_fn, {s->datasource})) : 0;
}

long cb_tell(void* user) {
    auto* s = static_cast<Stream*>(user);
    return s->tell_fn ? long(int32_t(cdecl_call(s->tell_fn, {s->datasource}))) : -1;
}

}  // namespace

// ov_open_callbacks(datasource, vf, initial, ibytes, ov_callbacks{read, seek, close, tell} by value)
CDECL(vorbisfile, ov_open_callbacks) {
    auto s = std::make_unique<Stream>();
    s->datasource = a[0];
    s->read_fn = a[4];
    s->seek_fn = a[5];
    s->close_fn = a[6];
    s->tell_fn = a[7];
    const ov_callbacks cb{cb_read, cb_seek, cb_close, cb_tell};
    Stream* raw = s.get();
    const int r = ov_open_callbacks(raw, &raw->vf, a[2] ? reinterpret_cast<const char*>(rt::host_ptr(a[2])) : nullptr,
                                    long(int32_t(a[3])), cb);
    if (r == 0) {
        std::lock_guard lk(g_mu);
        g_streams[a[1]] = std::move(s);
    }
    return uint32_t(r);
}

CDECL(vorbisfile, ov_clear) {
    std::unique_ptr<Stream> s;
    {
        std::lock_guard lk(g_mu);
        auto it = g_streams.find(a[0]);
        if (it == g_streams.end()) return 0;
        s = std::move(it->second);
        g_streams.erase(it);
    }
    ov_clear(&s->vf);
    if (s->staging) rt::heap_free(s->staging);
    return 0;
}

// ov_read(vf, buffer, length, bigendianp, word, sgned, *bitstream)
CDECL(vorbisfile, ov_read) {
    Stream* s = stream_of(a[0]);
    if (!s) return uint32_t(OV_EINVAL);
    int bitstream = 0;
    const long r = ov_read(&s->vf, reinterpret_cast<char*>(rt::host_ptr(a[1])), int(a[2]), int(a[3]), int(a[4]), int(a[5]),
                           &bitstream);
    if (a[6]) wr32(a[6], uint32_t(bitstream));
    return uint32_t(int32_t(r));
}

// ov_pcm_seek(vf, ogg_int64_t pos): the 64-bit position takes two slots.
CDECL(vorbisfile, ov_pcm_seek) {
    Stream* s = stream_of(a[0]);
    if (!s) return uint32_t(OV_EINVAL);
    return uint32_t(ov_pcm_seek(&s->vf, ogg_int64_t((uint64_t(a[2]) << 32) | a[1])));
}

// ov_pcm_total(vf, link) returns ogg_int64_t in edx:eax.
CDECL(vorbisfile, ov_pcm_total) {
    Stream* s = stream_of(a[0]);
    const ogg_int64_t r = s ? ov_pcm_total(&s->vf, int(a[1])) : OV_EINVAL;
    c->edx = uint32_t(uint64_t(r) >> 32);
    return uint32_t(uint64_t(r));
}

// ov_info(vf, link) -> vorbis_info* (32-bit layout: 7 longs + codec_setup pointer).
CDECL(vorbisfile, ov_info) {
    Stream* s = stream_of(a[0]);
    if (!s) return 0;
    const vorbis_info* vi = ov_info(&s->vf, int(a[1]));
    if (!vi) return 0;
    if (!s->info) s->info = rt::heap_alloc(32, true);
    wr32(s->info + 0, uint32_t(vi->version));
    wr32(s->info + 4, uint32_t(vi->channels));
    wr32(s->info + 8, uint32_t(vi->rate));
    wr32(s->info + 12, uint32_t(vi->bitrate_upper));
    wr32(s->info + 16, uint32_t(vi->bitrate_nominal));
    wr32(s->info + 20, uint32_t(vi->bitrate_lower));
    wr32(s->info + 24, uint32_t(vi->bitrate_window));
    return s->info;
}

// ov_comment(vf, link) -> vorbis_comment* { char** user_comments; int* lengths; int comments; char* vendor }.
CDECL(vorbisfile, ov_comment) {
    Stream* s = stream_of(a[0]);
    if (!s) return 0;
    const vorbis_comment* vc = ov_comment(&s->vf, int(a[1]));
    if (!vc) return 0;
    if (!s->comment) {
        const uint32_t n = uint32_t(vc->comments);
        const uint32_t g = rt::heap_alloc(16, true);
        const uint32_t ptrs = rt::heap_alloc(4 * (n + 1), true), lens = rt::heap_alloc(4 * (n + 1), true);
        for (uint32_t i = 0; i < n; ++i) {
            wr32(ptrs + 4 * i, rt::guest_strdup(std::string(vc->user_comments[i], size_t(vc->comment_lengths[i]))));
            wr32(lens + 4 * i, uint32_t(vc->comment_lengths[i]));
        }
        wr32(g + 0, ptrs);
        wr32(g + 4, lens);
        wr32(g + 8, n);
        wr32(g + 12, rt::guest_strdup(vc->vendor ? vc->vendor : ""));
        s->comment = g;
    }
    return s->comment;
}
