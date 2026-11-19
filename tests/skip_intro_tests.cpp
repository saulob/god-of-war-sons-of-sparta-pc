// Skip Intro selection: which builds support it and how the setting reaches the player. The player's own behaviour is
// tested by tools\avplayer_tests.cpp.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "game_profile.h"

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

std::string Read(const std::string& relative) {
    std::ifstream in(std::string(SOS_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    for (const sos::GameBuild& build : sos::SupportedBuilds()) {
        const std::string version = build.version;
        Check(build.skip_intro == (version == "1.00"), "Skip Intro is verified for 1.00 only: " + version);
    }

    const std::string loader = Read("src/loader.c");
    Check(Has(loader, "sos_skip_intro_supported(paths.image_dir) ? DCP_GPU_FEATURE_SKIP_INTRO : 0"),
          "the loader grants Skip Intro by build");
    const std::string gpu = Read("gpu/shim/dcpgpu.cpp");
    Check(Has(gpu, "DcpSettings::Get().skip_intro && (config->features & DCP_GPU_FEATURE_SKIP_INTRO)"),
          "the video core needs both the setting and the build feature");
    Check(Has(gpu, "SkipIntro::Global().Configure(skip_intro)"), "the player policy is configured once at start");
    const std::string settings = Read("gpu/shim/dcp_settings.cpp");
    Check(Has(settings, "key == \"skip_intro\""), "the settings file's skip_intro is read");
    const std::string source = Read("gpu/shadps4/core/libraries/avplayer/avplayer_source.cpp");
    Check(Has(source, "m_skip_intro = SkipIntro::Global().ClaimSource();"), "the player claims the first source");
    Check(Has(source, "m_is_looping && !m_skip_intro"), "a cut source never loops back");
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t found = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++found;
        return found;
    };
    Check(count(source, "ClaimSource()") == 1, "only the player's Init claims a source");
    Check(Has(source, "if (m_skip_intro && past_skip_window(*up_packet)) {\n            natural_eof = true;\n            break;"),
          "the cut leaves the demuxer loop through the natural end-of-file path");
    Check(Has(source, "m_state.OnEOF();") && Has(source, "m_video_decoder_thread.Join();"),
          "the end-of-file path joins the decoders and signals the end");
    Check(Has(source, "std::memset(p_buffer, 0, size);"), "a cut source's audio is silent");

    const std::string overlay = Read("gpu/shim/dcp_overlay_generic.cpp");
    Check(Has(overlay, "return initialized && (loading || background_total || DcpSettings::Get().show_fps || NoticeShown());"),
          "frame counter: the overlay is visible only while a readout is on");
    Check(Has(overlay, "bool CapturesInput() { return false; }") && Has(overlay, "bool HandleEvent(const SDL_Event&) { return false; }"),
          "frame counter: the overlay takes no input");
    Check(Has(overlay, "if (!BeginPresent()) {\n        return;\n    }"), "frame counter: hidden means no frame is built");
    Check(Has(overlay, "if (DcpSettings::Get().show_fps && frame_ms_avg > 0.0f) {"), "frame counter: drawn only while shown");
    const std::string presenter = Read("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Has(presenter, "const bool overlay = DcpOverlay::Visible();") && Has(presenter, "if (overlay) {\n            DcpOverlay::Render("),
          "frame counter: the presenter draws the overlay only when it is visible");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
