// Decoded NV12 frames as the guest receives them, on the production CopyNV12Data: rows land at the 64-byte guest pitch
// whatever the decoder's row size (per plane), the rows below the picture repeat its last row, and a decoder row that
// happens to equal the guest pitch is the only case that may be copied in bulk. No window, GPU, launcher or game.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "core/libraries/videodec/video_utils.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

u8 LumaAt(u32 x, u32 y) {
    return u8(16 + (x * 7 + y * 13) % 200);
}

u8 ChromaAt(u32 x, u32 y) {
    return u8(64 + (x * 3 + y * 11) % 120);
}

void TestFrame(u32 width, u32 height, u32 luma_linesize, u32 chroma_linesize) {
    std::vector<u8> luma(luma_linesize * height, 0xEE), chroma(chroma_linesize * (height / 2), 0xEE);
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) luma[y * luma_linesize + x] = LumaAt(x, y);
    for (u32 y = 0; y < height / 2; ++y)
        for (u32 x = 0; x < width; ++x) chroma[y * chroma_linesize + x] = ChromaAt(x, y);

    AVFrame frame{};
    frame.width = int(width);
    frame.height = int(height);
    frame.data[0] = luma.data();
    frame.data[1] = chroma.data();
    frame.linesize[0] = int(luma_linesize);
    frame.linesize[1] = int(chroma_linesize);

    const u32 pitch = (width + 63) & ~63u;
    const u32 padded = (height + 15) & ~15u;
    std::vector<u8> out(pitch * padded * 3 / 2, 0);
    Libraries::Videodec::CopyNV12Data(out.data(), out.size(), frame);

    bool luma_ok = true, chroma_ok = true;
    for (u32 y = 0; y < padded; ++y) {
        const u32 source_row = y < height ? y : height - 1;
        for (u32 x = 0; x < width; ++x) luma_ok &= out[y * pitch + x] == LumaAt(x, source_row);
    }
    const u8* uv = out.data() + pitch * padded;
    for (u32 y = 0; y < padded / 2; ++y) {
        const u32 source_row = y < height / 2 ? y : height / 2 - 1;
        for (u32 x = 0; x < width; ++x) chroma_ok &= uv[y * pitch + x] == ChromaAt(x, source_row);
    }
    const std::string label = std::to_string(width) + "x" + std::to_string(height) + " rows " +
                              std::to_string(luma_linesize) + "/" + std::to_string(chroma_linesize) + " pitch " +
                              std::to_string(pitch);
    Check(luma_ok, "luma plane of " + label);
    Check(chroma_ok, "chroma plane of " + label);
}

} // namespace

int main() {
    // Decoder rows wider than the picture, on and off the guest pitch.
    TestFrame(70, 20, 94, 94);
    TestFrame(66, 34, 66, 66);
    TestFrame(1920, 1080, 1984, 1984);
    TestFrame(1920, 1080, 2048, 2048);
    TestFrame(128, 64, 160, 160);
    // A picture as wide as the pitch whose decoder rows are wider than it.
    TestFrame(1920, 1080, 1984, 1920);
    TestFrame(64, 16, 128, 128);
    // Planes with different row sizes.
    TestFrame(66, 34, 66, 98);
    TestFrame(1920, 1080, 1920, 2048);
    // Rows exactly at the guest pitch: the bulk copy.
    TestFrame(1920, 1080, 1920, 1920);
    TestFrame(64, 16, 64, 64);
    TestFrame(70, 20, 128, 128);
    // The height padded to 16 rows below the picture.
    TestFrame(96, 18, 128, 128);
    std::printf("video frame tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
