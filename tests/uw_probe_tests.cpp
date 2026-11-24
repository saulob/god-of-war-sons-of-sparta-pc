// DEBUG_SAULO: verifies the minimal F5 capture trigger, read-only camera telemetry and baseline invariants.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dcp_capture_window.h"
#include "dcp_f5_capture.h"
#include "sos_uw_probe.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

namespace fs = std::filesystem;

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

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

const fs::path kRoot = SOS_SOURCE_DIR;

std::vector<float> g_published;
uint32_t g_serial = 0;

void Publish(float x, float y, float ortho) {
    g_published = {x, y, ortho};
}

uint32_t Serial() {
    return g_serial;
}

void TestHook() {
    const uint32_t site = sos_uw_probe_site("CUSA57767-1.01");
    Check(site == 0x577e230 && sos_uw_probe_site("CUSA57767-1.00") == 0x57736b0 && sos_uw_probe_site("CUSA00000-1.00") == 0,
          "hook: PushStateToUnityCamera of both builds");
    const uint8_t prefix[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                              0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38, 0x4c, 0x8b, 0x2d};
    std::vector<uint8_t> image(site + 256 + SOS_UW_PROBE_STUB_BYTES, 0xcc);
    std::memcpy(image.data() + site, prefix, sizeof(prefix));
    uint8_t* stubs = image.data() + site + 256;
    char msg[200] = "";
    image[site + 18] = 0;
    Check(!sos_uw_probe_install(image.data(), image.size(), "CUSA57767-1.01", stubs, SOS_UW_PROBE_STUB_BYTES, msg, sizeof(msg)) &&
              Contains(msg, "bytes mismatch") && image[site] == 0x55,
          "hook: other bytes at the camera site are left alone");
    image[site + 18] = prefix[18];
    Check(!sos_uw_probe_install(image.data(), image.size(), "CUSA57767-2.00", stubs, SOS_UW_PROBE_STUB_BYTES, msg, sizeof(msg)) &&
              Contains(msg, "not a supported game build"),
          "hook: unknown build refused");
    Check(!sos_uw_probe_install(image.data(), site + 10, "CUSA57767-1.01", stubs, SOS_UW_PROBE_STUB_BYTES, msg, sizeof(msg)),
          "hook: truncated image refused");
    Check(!sos_uw_probe_install(image.data(), image.size(), "CUSA57767-1.01", stubs, SOS_UW_PROBE_STUB_BYTES - 1, msg, sizeof(msg)) &&
              image[site] == 0x55,
          "hook: small stub buffer refused before patching");
    Check(sos_uw_probe_install(image.data(), image.size(), "CUSA57767-1.01", stubs, SOS_UW_PROBE_STUB_BYTES, msg, sizeof(msg)) == 1,
          "hook: installed " + std::string(msg));
    int32_t rel = 0;
    std::memcpy(&rel, image.data() + site + 1, 4);
    Check(image[site] == 0xe9 && image.data() + site + 5 + rel == stubs && image[site + 5] == 0x90 &&
              std::memcmp(image.data() + site + 6, prefix + 6, sizeof(prefix) - 6) == 0,
          "hook: entry jumps to the stub, the rest of the method is untouched");
    const uint8_t save_head[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
    uint64_t handler = 0;
    std::memcpy(&handler, stubs + sizeof(save_head), 8);
    const size_t stolen_at = sizeof(save_head) + 8 + 14;
    std::memcpy(&rel, stubs + stolen_at + SOS_UW_PROBE_STOLEN + 1, 4);
    Check(std::memcmp(stubs, save_head, sizeof(save_head)) == 0 && handler == uint64_t(uintptr_t(&sos_uw_probe_on_camera)) &&
              std::memcmp(stubs + stolen_at, prefix, SOS_UW_PROBE_STOLEN) == 0 && stubs[stolen_at + SOS_UW_PROBE_STOLEN] == 0xe9 &&
              stubs + stolen_at + SOS_UW_PROBE_STOLEN + 5 + rel == image.data() + site + SOS_UW_PROBE_STOLEN,
          "hook: the stub saves the arguments, calls the handler, replays the 6 stolen bytes and returns to the method");

    uint8_t state[0x90] = {};
    const float ortho = 4.8f, raw[3] = {-86.f, 4.5f, -10.f}, correction[3] = {1.25f, 0.25f, 0.f};
    std::memcpy(state + SOS_UW_PROBE_ORTHO, &ortho, 4);
    std::memcpy(state + SOS_UW_PROBE_RAW_POSITION, raw, sizeof(raw));
    std::memcpy(state + SOS_UW_PROBE_CORRECTION, correction, sizeof(correction));
    g_published.clear();
    sos_uw_probe_on_camera(nullptr, state);
    Check(g_published.empty(), "hook: nothing is published before arming");
    sos_uw_probe_arm(Publish, Serial);
    sos_uw_probe_on_camera(nullptr, state);
    Check(g_published.size() == 3 && g_published[0] == -84.75f && g_published[1] == 4.75f && g_published[2] == 4.8f,
          "hook: camera = RawPosition + PositionCorrection, with the ortho size");
    g_published.clear();
    ++g_serial;
    sos_uw_probe_on_camera(nullptr, state);
    Check(g_published.size() == 3 && g_published[0] == -84.75f,
          "f5: the one-shot layer diagnostic leaves camera telemetry intact");
    const float nan = NAN;
    std::memcpy(state + SOS_UW_PROBE_RAW_POSITION, &nan, 4);
    g_published.clear();
    sos_uw_probe_on_camera(nullptr, state);
    sos_uw_probe_on_camera(nullptr, nullptr);
    Check(g_published.empty(), "hook: invalid states are ignored");
    sos_uw_probe_arm(nullptr, nullptr);
}

void TestF5Capture() {
    DcpF5Capture::Gate gate;
    int calls = 0;

    Check(gate.Count() == 0 && !Vulkan::FrameCapture::Active(),
          "f5: starts idle with no automatic capture");

    Check(!gate.TryRequest(true, [&] {
              ++calls;
              return true;
          }) && calls == 0 && gate.Count() == 0,
          "f5: key repeat is ignored");

    Check(gate.TryRequest(false, [&] {
              ++calls;
              return true;
          }) && calls == 1 && gate.Count() == 1,
          "f5: single press requests one capture");

    Check(!gate.TryRequest(false, [&] {
              ++calls;
              return false;
          }) && calls == 2 && gate.Count() == 1,
          "f5: busy capture does not create another request or consume cap");

    for (uint32_t i = 1; i < DcpF5Capture::kMaxCaptures; ++i) {
        Check(gate.TryRequest(false, [&] {
                  ++calls;
                  return true;
              }),
              "f5: capture " + std::to_string(i + 1) + " of 8 succeeds");
    }
    Check(gate.Count() == 8 && DcpF5Capture::kMaxCaptures == 8, "f5: cap is 8 captures per process");

    const int calls_at_cap = calls;
    Check(!gate.TryRequest(false, [&] {
              ++calls;
              return true;
          }) && calls == calls_at_cap && gate.Count() == 8,
          "f5: 9th capture is rejected without requesting");
}

void TestPassWindow() {
    using DcpCaptureWindow::Phase;
    using DcpCaptureWindow::Step;
    constexpr size_t kMax = 512;

    Check(DcpCaptureWindow::PassStep(Phase::Idle, 0, kMax) == Step::Skip &&
              DcpCaptureWindow::PassStep(Phase::Idle, 9999, kMax) == Step::Skip,
          "window: idle capture does nothing");

    // Mirrors FrameCapture::BeginPass: every pass is a new entry, whatever it renders to.
    auto run = [&](size_t passes, size_t max) {
        Phase phase = Phase::Armed;
        size_t recorded = 0;
        for (size_t i = 0; i < passes && phase != Phase::Idle; ++i) {
            const Step step = DcpCaptureWindow::PassStep(phase, recorded, max);
            if (step == Step::Begin) {
                phase = Phase::Recording;
                recorded = 0;
            }
            if (step == Step::Finish) {
                phase = Phase::Idle;
                continue;
            }
            ++recorded;
        }
        return std::make_pair(phase, recorded);
    };
    const auto longer = run(40, kMax);
    Check(longer.first == Phase::Recording && longer.second == 40,
          "window: more than 2 passes are recorded, with no frame boundary needed");
    const auto bounded = run(100000, kMax);
    Check(bounded.first == Phase::Idle && bounded.second == kMax,
          "window: recording stops at the bound and returns to idle");
    const auto single_target = run(kMax + 5, kMax);
    Check(single_target.first == Phase::Idle && single_target.second == kMax,
          "window: one render target throughout still completes the capture");
    Check(DcpCaptureWindow::PassStep(Phase::Armed, 7, kMax) == Step::Begin,
          "window: the first pass after F5 starts the capture");

    const std::string capture = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_frame_capture.cpp");
    Check(Contains(capture, "kMaxPasses = 512") && Contains(capture, "DcpCaptureWindow::PassStep(phase, entries.size(), kMaxPasses)") &&
              !Contains(capture, "new_pass") &&
              !fs::exists(kRoot / "gpu" / "shim" / "dcp_frame_boundary.h"),
          "window: BeginPass is bounded and does not use display-buffer identity");
    const size_t begin = capture.find("void FrameCapture::BeginPass");
    const size_t end = capture.find("void FrameCapture::State");
    const std::string body = begin == std::string::npos || end == std::string::npos ? "" : capture.substr(begin, end - begin);
    Check(!body.empty() && !Contains(body, "display") && !Contains(body, "3360") && !Contains(body, "1920") &&
              !Contains(body, "scene") && !Contains(body, "room") && !Contains(body, "camera") &&
              !Contains(body, "aspect"),
          "window: BeginPass has no display, scene, camera or resolution conditions");
    Check(!Contains(capture, "vkQueueWaitIdle") && !Contains(capture, "waitIdle") && !Contains(capture, "vkWaitForFences") &&
              !Contains(capture, "DownloadImage") && !Contains(capture, "readback") && !Contains(capture, "dcp_png.h"),
          "window: no GPU wait, synchronization, readback or PNG");
    const std::string rasterizer = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_rasterizer.cpp");
    const size_t state_at = rasterizer.find("FrameCapture::State(text)");
    const size_t gate_at = rasterizer.rfind("if (FrameCapture::Active()) {", state_at);
    Check(state_at != std::string::npos && gate_at != std::string::npos && state_at - gate_at < 2500,
          "window: extra draw state is recorded only inside the FrameCapture::Active() branch");
}

void TestParallaxGeometry() {
    struct Vec2 {
        float x, y;
    };
    struct Layer {
        const char* guid;
        const char* name;
        Vec2 initial;
        Vec2 factor;
        Vec2 offset;
        Vec2 effect_center;
        Vec2 effect_size;
    };
    const Vec2 f2bd{-43.94f, 2.72f}, f2bd_size{96.10f, 24.40f};
    const Vec2 b7fa{-61.35f, -5.23f}, b7fa_size{61.27f, 8.51f};
    const Vec2 v0{-199.68f, -24.00f}, v0_size{399.36f, 67.20f};
    const Layer layers[] = {
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista_6_Room 8", {-92.16f, -9.60f}, {0.25f, 0.0f}, {-11.91f, 0.0f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista 1 Room 2", {-87.56f, -2.49f}, {1.0f, 1.0f}, {-47.63f, -9.53f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "FG1 (Room_Bounds_Wide)", {0.0f, 0.0f}, {-0.4f, 0.0f}, {19.05f, 0.0f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "BG_0_2", {-92.16f, -9.60f}, {0.0f, 0.0f}, {0.0f, 0.0f}, f2bd, f2bd_size},
        {"b7fa8611-0145-406a-a4b7-407b38534962", "FG", {-92.16f, -9.60f}, {0.0f, 0.0f}, {0.0f, 0.0f}, b7fa, b7fa_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista 6 Room 2", {-70.52f, -3.50f}, {0.9f, 0.9f}, {-42.86f, -8.57f}, f2bd, f2bd_size},
        {"b7fa8611-0145-406a-a4b7-407b38534962", "FG1", {0.0f, 0.0f}, {-0.4f, 0.0f}, {12.08f, 0.0f}, b7fa, b7fa_size},
        {"b7fa8611-0145-406a-a4b7-407b38534962", "BG0", {0.0f, 0.0f}, {0.3f, 0.3f}, {-9.06f, -0.47f}, b7fa, b7fa_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "BG_2_SPR", {-92.16f, -9.60f}, {0.15f, 0.15f}, {-7.14f, -1.43f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "BG_1 (Room_Bounds_Wide)", {-92.16f, -9.60f}, {0.2f, 0.0f}, {-9.53f, 0.0f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista 1_Overhaul Room 7", {141.92f, 43.19f}, {0.99f, 1.0f}, {-47.15f, -9.53f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista 2 Room 2", {-78.35f, -2.94f}, {0.985f, 1.0f}, {-46.91f, -9.53f}, f2bd, f2bd_size},
        {"52de40f1-7ec1-4e45-ae59-a46bccc29d09", "V_0", {-263.12f, -48.24f}, {0.01f, 0.0f}, {1.08f, 0.0f}, v0, v0_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista 2_Overhaul Room 7", {141.92f, 43.19f}, {0.98f, 1.0f}, {-46.68f, -9.53f}, f2bd, f2bd_size},
        {"b7fa8611-0145-406a-a4b7-407b38534962", "IS0 Parallax Room 7", {35.19f, 16.70f}, {0.05f, 0.0f}, {-1.51f, 0.0f}, b7fa, b7fa_size},
        {"b7fa8611-0145-406a-a4b7-407b38534962", "BG_1", {-92.16f, -9.60f}, {0.2f, 0.2f}, {-6.04f, -0.32f}, b7fa, b7fa_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "BG_2_2", {-92.16f, -9.60f}, {0.2f, 0.2f}, {-9.53f, -1.91f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "Vista_5_3", {-92.16f, -9.60f}, {0.35f, 0.35f}, {-16.67f, -3.33f}, f2bd, f2bd_size},
        {"b7fa8611-0145-406a-a4b7-407b38534962", "FG0_1", {0.0f, 0.0f}, {-0.2f, 0.0f}, {6.04f, 0.0f}, b7fa, b7fa_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "FG_1_SPR", {-92.16f, -9.60f}, {-0.4f, 0.0f}, {19.05f, 0.0f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "BG_0_SPR", {-92.16f, -9.60f}, {0.25f, 0.25f}, {-11.91f, -2.38f}, f2bd, f2bd_size},
        {"f2bd43f6-4ca9-482c-9595-0a1dc9dc2473", "FG_0_SPR", {-92.16f, -9.60f}, {-0.25f, 0.0f}, {11.91f, 0.0f}, f2bd, f2bd_size},
    };
    const Vec2 camera{-91.56f, -6.81f};
    const float ortho = 3.36f;
    const Vec2 frustum_16{2.0f * ortho * 16.0f / 9.0f, 2.0f * ortho};
    const Vec2 frustum_21{2.0f * ortho * 21.0f / 9.0f, 2.0f * ortho};
    const Vec2 frustum_32{2.0f * ortho * 32.0f / 9.0f, 2.0f * ortho};

    auto offset = [](Vec2 cam, Vec2 center, Vec2 size, Vec2 frustum, Vec2 factor) {
        const Vec2 limit{2.0f * frustum.x + 0.5f * size.x, 2.0f * frustum.y + 0.5f * size.y};
        Vec2 delta{cam.x - center.x, cam.y - center.y};
        delta.x = delta.x < -limit.x ? -limit.x : (delta.x > limit.x ? limit.x : delta.x);
        delta.y = delta.y < -limit.y ? -limit.y : (delta.y > limit.y ? limit.y : delta.y);
        return Vec2{delta.x * factor.x, delta.y * factor.y};
    };
    auto close = [](Vec2 a, Vec2 b, float tolerance) {
        return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance;
    };
    int matched = 0, aspect_equal = 0, unclamped = 0;
    for (const Layer& layer : layers) {
        const Vec2 at16 = offset(camera, layer.effect_center, layer.effect_size, frustum_16, layer.factor);
        const Vec2 at21 = offset(camera, layer.effect_center, layer.effect_size, frustum_21, layer.factor);
        const Vec2 at32 = offset(camera, layer.effect_center, layer.effect_size, frustum_32, layer.factor);
        Check(close(at21, layer.offset, 0.02f),
              "parallax: 21:9 offset of " + std::string(layer.name) + " matches the captured frame");
        if (close(at21, layer.offset, 0.02f)) ++matched;
        if (close(at16, at21, 1e-6f) && close(at21, at32, 1e-6f)) ++aspect_equal;
        const Vec2 limit16{2.0f * frustum_16.x + 0.5f * layer.effect_size.x, 2.0f * frustum_16.y + 0.5f * layer.effect_size.y};
        const Vec2 limit21{2.0f * frustum_21.x + 0.5f * layer.effect_size.x, 2.0f * frustum_21.y + 0.5f * layer.effect_size.y};
        const float dx = std::fabs(camera.x - layer.effect_center.x), dy = std::fabs(camera.y - layer.effect_center.y);
        if (dx <= limit16.x && dy <= limit16.y && dx <= limit21.x && dy <= limit21.y) ++unclamped;
    }
    Check(matched == int(std::size(layers)), "parallax: the documented formula reproduces all 22 captured offsets");
    Check(aspect_equal == int(std::size(layers)), "parallax: offsets are identical at 16:9, 21:9 and 32:9");
    Check(unclamped == int(std::size(layers)), "parallax: the frustum clamp is inactive for every captured layer");

    const Vec2 clamped = offset(Vec2{100.0f, 100.0f}, Vec2{1.0f, 1.0f}, Vec2{2.0f, 2.0f}, Vec2{2.0f, 2.0f}, Vec2{1.0f, 1.0f});
    Check(close(clamped, Vec2{5.0f, 5.0f}, 1e-6f), "parallax: the clamp path still limits an out-of-bounds camera");

    const std::string probe = ReadFile(kRoot / "src" / "sos_uw_probe.c");
    Check(Contains(probe, "renderers=%s") && Contains(probe, "disabled + 0x18") && Contains(probe, "candidates=%d") &&
              !Contains(probe, "sos_parallax_scene_table") && !Contains(probe, "staticRenderers") &&
              !Contains(probe, "game_object + 0x38") && !Contains(probe, "disabled + 0x24"),
          "parallax: F5 reports live renderer state without the temporary authored scene table");
}

void TestBaselineInvariants() {
    Check(fs::exists(kRoot / "gpu" / "shim" / "dcp_f5_capture.h"), "f5: dcp_f5_capture.h exists");
    Check(!fs::exists(kRoot / "gpu" / "shim" / "dcp_frame_boundary.h"), "baseline: dcp_frame_boundary.h does not exist");
    Check(!fs::exists(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_uw_probe.cpp"),
          "baseline: vk_uw_probe.cpp removed");
    Check(!fs::exists(kRoot / "gpu" / "shim" / "dcp_uw_probe.h"), "baseline: dcp_uw_probe.h removed");

    for (const fs::path& rel : {
             fs::path("src") / "dcp_keys.c",
             fs::path("src") / "sos_keys.c",
             fs::path("src") / "runtime_pad.c",
             fs::path("gpu") / "shim" / "dcp_settings.h",
             fs::path("gpu") / "shim" / "dcp_options_menu.h",
             fs::path("gpu") / "shim" / "dcp_ping.h",
             fs::path("gpu") / "shim" / "dcp_overlay.cpp",
         }) {
        const std::string text = ReadFile(kRoot / rel);
        Check(!Contains(text, "SDL_SCANCODE_F5") && !Contains(text, "SDLK_F5"),
              "baseline: no F5 key handling in " + rel.generic_string());
    }

    const std::string window = ReadFile(kRoot / "gpu" / "shim" / "window.cpp");
    Check(Contains(window, "#include \"dcp_f5_capture.h\"") &&
              Contains(window, "#include \"video_core/renderer_vulkan/vk_frame_capture.h\"") &&
              Contains(window, "SDL_SCANCODE_F5") && Contains(window, "HandleF5Capture") &&
              Contains(window, "Vulkan::FrameCapture::Request") && !Contains(window, "dcpgpu_debug_ultrawide_f5"),
          "f5: window.cpp has only the minimal F5 host handler");

    const std::string presenter = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_presenter.cpp");
    Check(!Contains(presenter, "FrameCapture") && !Contains(presenter, "UwSampleFrameEnd") &&
              !Contains(presenter, "UwFinishCapture") && !Contains(presenter, "UwPendingOrActive"),
          "baseline: presenter unchanged");

    const std::string rasterizer = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_rasterizer.cpp");
    const std::string rasterizer_h = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_rasterizer.h");
    Check(!Contains(rasterizer, "UwSampleAfterDraw") && !Contains(rasterizer, "FrameCapture::DrawDetail") &&
              !Contains(rasterizer, "FrameCapture::PassState") && !Contains(rasterizer_h, "uw_readback") &&
              !Contains(rasterizer_h, "uw_grid") && !Contains(rasterizer_h, "uw_image"),
          "baseline: rasterizer unchanged (no grid/readback/capture additions)");

    const std::string capture_h = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_frame_capture.h");
    const std::string capture = ReadFile(kRoot / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_frame_capture.cpp");
    Check(!Vulkan::FrameCapture::Active(), "baseline: FrameCapture starts idle");
    Check(Contains(capture_h, "static bool Request();") && !Contains(capture_h, "OnPresent") &&
              !Contains(capture_h, "FinishAuto") && !Contains(capture_h, "RequestUwCapture") &&
              !Contains(capture_h, "WantsDrawSample") && !Contains(capture_h, "FinalImage") &&
              !Contains(capture_h, "dcp_frame_boundary"),
          "f5: FrameCapture header has no automatic capture or readback API");
    Check(Contains(capture, "bool FrameCapture::Request()") &&
              Contains(capture, "state.compare_exchange_strong(expected, Armed") &&
              !Contains(capture, "dcp_f5_capture.h") && !Contains(capture, "dcp_uw_probe.h") &&
              !Contains(capture, "dcp_frame_boundary.h") && !Contains(capture, "DcpFrameBoundary") &&
              !Contains(capture, "dcp_png.h") && !Contains(capture, "WriteRgba") &&
              !Contains(capture, "dcpgpu_debug_ultrawide_f5") && !Contains(capture, "dcpgpu_debug_ultrawide_arm") &&
              !Contains(capture, "uw_manual_") && !Contains(capture, "StartAuto") &&
              Contains(capture, "void FrameCapture::Poll() {}"),
          "baseline: FrameCapture has no automatic capture, PNG or readback");
    Check(Contains(capture, "extern \"C\" void dcpgpu_debug_camera(float x, float y, float ortho)"),
          "baseline: read-only camera telemetry sink unchanged");

    const std::string png_h = ReadFile(kRoot / "gpu" / "shim" / "dcp_png.h");
    const std::string png_cpp = ReadFile(kRoot / "gpu" / "shim" / "dcp_png.cpp");
    Check(!Contains(png_h, "WriteRgba") && !Contains(png_cpp, "png_image_write_to_file"),
          "baseline: no PNG write additions in dcp_png");

    const std::string dcpgpu_h = ReadFile(kRoot / "src" / "dcpgpu.h");
    Check(Contains(dcpgpu_h, "void dcpgpu_debug_camera(float x, float y, float ortho);") &&
              !Contains(dcpgpu_h, "dcpgpu_debug_ultrawide_f5") && !Contains(dcpgpu_h, "dcpgpu_debug_ultrawide_arm"),
          "baseline: dcpgpu.h exposes only read-only camera telemetry");

    const std::string loader = ReadFile(kRoot / "src" / "loader.c");
    const size_t install = loader.find("sos_uw_probe_install");
    const size_t wide = install == std::string::npos ? std::string::npos
                                                     : loader.rfind("sos_render_target_wider_than_16_9(&render_target)", install);
    const size_t open = wide == std::string::npos ? std::string::npos : loader.find('{', wide);
    const size_t close = open == std::string::npos ? std::string::npos : loader.find("\n    }", open);
    Check(Contains(loader, "sos_uw_probe_arm(dcpgpu_debug_camera, dcpgpu_debug_f5_serial)") &&
              !Contains(loader, "sos_uw_probe_arm_ultrawide") &&
              !Contains(loader, "dcpgpu_debug_ultrawide_arm") && !Contains(loader, "uw_diagnostics_dir"),
          "baseline: loader arms only the camera telemetry");
    Check(wide != std::string::npos && install != std::string::npos && open != std::string::npos &&
              close != std::string::npos && open < install && install < close,
          "baseline: the camera telemetry is installed only for a wider-than-16:9 target");

    const std::string probe = ReadFile(kRoot / "src" / "sos_uw_probe.c");
    Check(!Contains(probe, "SetResolution") && !Contains(probe, "confiner") && !Contains(probe, "arm_aspect") &&
              !Contains(probe, "SetActive") && !Contains(probe, "set_enabled") && !Contains(probe, "gameObject") &&
              !Contains(probe, "level") && !Contains(probe, "room"),
          "baseline: no gameplay objects or scene data");
    Check(!Contains(probe, "g_should_update") && !Contains(probe, "SosUwParallaxShouldUpdate") &&
              !Contains(probe, "sos_uw_parallax_thunk") && !Contains(probe, "sos_uw_parallax_decide") &&
              !Contains(probe, "ShouldUpdateParallax") && !Contains(probe, "0x4b65471") && !Contains(probe, "0x4b65860") &&
              !Contains(probe, "0x4b6b930") && !Contains(probe, "0x4b6c040") &&
              !Contains(probe, "VirtualProtect") && !Contains(probe, "mprotect") && !Contains(probe, "0x41, 0x5b") &&
              !Contains(probe, "0x41, 0x53"),
          "baseline: the ShouldUpdateParallax hook and its guest thunk are gone");
    Check(Contains(probe, "log_parallax") && Contains(probe, "g_last_serial") &&
              !Contains(probe, "DisableAndTrackRenderers") && !Contains(probe, "ShowRenderers") &&
              !Contains(probe, "SetActive") && !Contains(probe, "renderer.enabled") &&
              !Contains(probe, "0x31, 0xf6, 0xff, 0xd0") && !Contains(probe, "0x4b66c40") &&
              !Contains(probe, "0x4b6d420") && !Contains(probe, "0x22b"),
          "baseline: the Parallax renderer keepalive is gone; the F5 diagnostic only reads layer state");
}

} // namespace

int main() {
    TestHook();
    TestF5Capture();
    TestPassWindow();
    TestParallaxGeometry();
    TestBaselineInvariants();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
