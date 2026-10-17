// Layer 2 of the video isolation: decodes one frame through bink::Decoder into
// planes laid out like the game's Bink frame buffers (Y pitch 2048, chroma
// pitch 1024), converts to RGB on the CPU and writes a PPM, plus raw planes.
// Usage: bink_decode_test <file.bik> <frame> <out_prefix>
#include "../../shims/video/bink_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    bink::Decoder dec;
    if (!dec.open(argv[1], false)) {
        std::printf("open failed\n");
        return 1;
    }
    const int target = std::atoi(argv[2]);
    const uint32_t w = dec.width(), h = dec.height();
    std::printf("%ux%u frames=%u rate=%d/%d alpha=%d\n", w, h, dec.frames(), dec.rate_num(), dec.rate_den(), dec.has_alpha());

    std::vector<uint8_t> y(2048 * 1024), cr(1024 * 512), cb(1024 * 512), a(2048 * 1024);
    const bink::Plane planes[4] = {{y.data(), 2048}, {cr.data(), 1024}, {cb.data(), 1024}, {a.data(), 2048}};
    for (int i = 0; i <= target; ++i)
        if (!dec.decode_next()) {
            std::printf("decode failed at %d\n", i);
            return 1;
        }
    dec.copy_planes(planes);

    const std::string prefix = argv[3];
    std::FILE* f = std::fopen((prefix + ".ppm").c_str(), "wb");
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    auto clamp = [](float v) { return uint8_t(std::clamp(v, 0.f, 255.f) + 0.5f); };
    for (uint32_t r = 0; r < h; ++r)
        for (uint32_t c = 0; c < w; ++c) {
            const float Y = 1.164f * (y[r * 2048 + c] - 16);
            const float V = cr[(r / 2) * 1024 + c / 2] - 128.f, U = cb[(r / 2) * 1024 + c / 2] - 128.f;
            const uint8_t px[3] = {clamp(Y + 1.596f * V), clamp(Y - 0.392f * U - 0.813f * V), clamp(Y + 2.017f * U)};
            std::fwrite(px, 1, 3, f);
        }
    std::fclose(f);
    for (auto [name, buf] : {std::pair{"y", &y}, {"cr", &cr}, {"cb", &cb}}) {
        std::FILE* p = std::fopen((prefix + "_" + name + ".raw").c_str(), "wb");
        std::fwrite(buf->data(), 1, buf->size(), p);
        std::fclose(p);
    }
    std::printf("Y[center]=%u Cr=%u Cb=%u\n", y[(h / 2) * 2048 + w / 2], cr[(h / 4) * 1024 + w / 4], cb[(h / 4) * 1024 + w / 4]);
    return 0;
}
