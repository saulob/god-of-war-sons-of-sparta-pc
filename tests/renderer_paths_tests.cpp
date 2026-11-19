// Headless proof of the renderer paths behind the Graphics and Display options: texture filtering, SMAA, FSR 1, RCAS,
// VSync, Presentation and frame pacing. Pure helpers are tested numerically; the Vulkan code is checked to call them
// in the right order. No window, GPU or game.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dcp_frame_pacing.h"
#include "dcp_graphics.h"
#include "dcp_present.h"
#include "dcp_settings.h"

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

std::string Source(const char* path) {
    std::ifstream in(fs::path(SOS_SOURCE_DIR) / path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), char(13)), text.end());
    return text;
}

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
    const size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a + first.size()) != std::string::npos;
}

constexpr const char* kPresenter = "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp";
constexpr const char* kRasterizer = "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp";

void TestTextureFiltering() {
    using DcpGraphics::EffectiveMaxAnisotropy;
    using DcpGraphics::ForcedAnisotropyRatio;
    for (const int forced : {2, 4, 8, 16}) {
        Check(ForcedAnisotropyRatio(forced, false, true, true, false, 1.0f) == forced,
              "a plain filtered sampler is forced to " + std::to_string(forced) + "x");
    }
    Check(ForcedAnisotropyRatio(0, false, true, true, false, 1.0f) == 0, "Game leaves the sampler alone");
    Check(ForcedAnisotropyRatio(1, false, true, true, false, 1.0f) == 0, "a ratio of 1 is not an override");
    Check(ForcedAnisotropyRatio(8, true, true, true, false, 1.0f) == 0, "depth and compute samplers are not changed");
    Check(ForcedAnisotropyRatio(8, false, false, true, false, 1.0f) == 0, "samplers without mips are not changed");
    Check(ForcedAnisotropyRatio(8, false, true, false, false, 1.0f) == 0, "point filtered samplers are not changed");
    Check(ForcedAnisotropyRatio(2, false, true, true, true, 16.0f) == 0, "a sampler that asks for more is kept");
    Check(ForcedAnisotropyRatio(8, false, true, true, true, 2.0f) == 8, "a sampler that asks for less is raised");
    Check(ForcedAnisotropyRatio(16, false, true, true, true, 16.0f) == 0, "a sampler already at the ratio is kept");

    Check(EffectiveMaxAnisotropy(true, true, 16.0f, 16.0f) == 16.0f, "16x on a 16x device");
    Check(EffectiveMaxAnisotropy(true, true, 16.0f, 8.0f) == 8.0f, "16x is clamped to an 8x device");
    Check(EffectiveMaxAnisotropy(true, true, 2.0f, 16.0f) == 2.0f && EffectiveMaxAnisotropy(true, true, 4.0f, 16.0f) == 4.0f,
          "2x and 4x reach the sampler as asked");
    Check(EffectiveMaxAnisotropy(false, true, 16.0f, 16.0f) == 1.0f, "no samplerAnisotropy feature gives 1");
    Check(EffectiveMaxAnisotropy(true, false, 16.0f, 16.0f) == 1.0f, "a non-anisotropic filter gives 1");
    Check(EffectiveMaxAnisotropy(true, true, 0.0f, 16.0f) == 1.0f && EffectiveMaxAnisotropy(true, true, 16.0f, 0.0f) == 1.0f,
          "degenerate values stay valid");

    const std::string rasterizer = Source(kRasterizer);
    Check(InOrder(rasterizer, "ForceAnisotropy(ssharp, sampler.is_depth);", "texture_cache.GetSampler("),
          "the override is applied before the sampler is looked up");
    Check(Contains(rasterizer, "prepared ? prepared->sampler_sharps[sampler_index] : sampler.GetSharp(stage)") &&
              Contains(rasterizer, "auto ssharp ="),
          "the override changes a copy of the game's sampler");
    const std::string forcing = Source("gpu/shadps4/video_core/texture_cache/forced_anisotropy.h");
    Check(Contains(rasterizer, "VideoCore::ForceSamplerAnisotropy(sharp, DcpSettings::Get().anisotropy, is_depth || pipeline_is_compute)"),
          "the rasterizer forces the setting on every sampler it binds");
    Check(Contains(forcing, "sharp.max_aniso.Assign(ratio);") && Contains(forcing, "sharp.xy_min_filter.Assign(AmdGpu::Filter::AnisoLinear);"),
          "the forced ratio and the anisotropic filter are written into the sampler");
    Check(Contains(Source("gpu/shadps4/video_core/texture_cache/texture_cache.cpp"), "XXH3_64bits(&sampler, sizeof(sampler))"),
          "the sampler cache key hashes the whole sampler, effective ratio included");
    const std::string sampler = Source("gpu/shadps4/video_core/texture_cache/sampler.cpp");
    Check(Contains(sampler, "DcpGraphics::EffectiveMaxAnisotropy(") && Contains(sampler, "instance.MaxSamplerAnisotropy()") &&
              Contains(sampler, ".anisotropyEnable = anisotropy_enable,") && Contains(sampler, ".maxAnisotropy = max_anisotropy,"),
          "the Vulkan sampler gets the clamped value");
    Check(Contains(Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp"), ".samplerAnisotropy = features.samplerAnisotropy,"),
          "the device enables samplerAnisotropy when the card has it");
    Check(Contains(Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.h"), "return properties.limits.maxSamplerAnisotropy;"),
          "the device limit is the card's maxSamplerAnisotropy");
}

void TestSmaa() {
    using DcpGraphics::PlanPostProcess;
    using DcpGraphics::TargetsStale;
    Check(!PlanPostProcess(false, false, true, 1920, 1080, 1920, 1080).smaa, "Off bypasses SMAA");
    Check(PlanPostProcess(true, false, true, 1920, 1080, 1920, 1080).smaa, "SMAA runs when on");
    Check(PlanPostProcess(true, true, true, 1920, 1080, 3840, 2160).smaa, "SMAA runs with FSR too");
    Check(TargetsStale(0, 0, 1920, 1080), "the first frame creates the targets");
    Check(!TargetsStale(1920, 1080, 1920, 1080), "an unchanged size keeps the targets");
    Check(TargetsStale(1920, 1080, 2560, 1440) && TargetsStale(1920, 1080, 3840, 2160) && TargetsStale(1920, 1080, 1280, 720),
          "a render resolution change recreates the targets");
    Check(TargetsStale(1920, 1080, 2560, 1080) && TargetsStale(1920, 1080, 1440, 1080), "an aspect ratio change recreates the targets");

    const std::string presenter = Source(kPresenter);
    Check(InOrder(presenter, "if (graphics.smaa) {", "image_view = smaa_pass.Render("), "the SMAA output replaces the image");
    Check(InOrder(presenter, "image_view = smaa_pass.Render(", "fsr_pass.Render(cmdbuf, image_view"),
          "the scaling pass reads the SMAA output");
    Check(InOrder(presenter, "image_view = fsr_pass.Render(", "pp_pass.Render(cmdbuf, image_view"),
          "the final pass reads the scaled output");
    Check(Contains(presenter, "smaa_pass.Create(device, instance.GetAllocator());"),
          "the pass is created with the presenter");
    const std::string pass = Source("gpu/shadps4/video_core/renderer_vulkan/host_passes/smaa_pass.cpp");
    Check(Contains(pass, "DcpGraphics::TargetsStale(cur_size.width, cur_size.height, size.width, size.height)"),
          "the pass recreates its targets when the size changes");
    Check(InOrder(pass, "DeferDestroy(scheduler, std::move(edges), std::move(weights), std::move(output));", "CreateTarget(edges,") &&
              InOrder(pass, "CreateTarget(edges,", "CreateTarget(weights,") && InOrder(pass, "CreateTarget(weights,", "CreateTarget(output,"),
          "the old targets are destroyed only after the GPU is done, then edges, weights and output are made");
    Check(InOrder(pass, "Draw(cmdbuf, 0, edges", "Draw(cmdbuf, 1, weights") && InOrder(pass, "Draw(cmdbuf, 1, weights", "Draw(cmdbuf, 2, output"),
          "edge detection, blending weights and neighbourhood blending run in order");
    Check(Contains(pass, "return *output.view;"), "the neighbourhood blend result is what the pass returns");
    Check(Contains(pass, "if (!lookup_ready) {"), "the lookup textures are uploaded once");
    Check(Contains(pass, "CreateTarget(output, ColorFormat, size"), "the output keeps the presenter's colour format");
}

void TestFsrAndRcas() {
    using DcpGraphics::PlanPostProcess;
    const auto plan = [](bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
        return PlanPostProcess(false, fsr1, rcas, in_w, in_h, out_w, out_h);
    };
    for (const auto& output : std::vector<std::pair<int, int>>{{1920, 1080}, {2560, 1440}, {1280, 720}, {3840, 2160}}) {
        Check(!plan(false, false, 1920, 1080, output.first, output.second).fsr_pass, "Off with no sharpening never runs the pass");
    }
    Check(plan(true, false, 1920, 1080, 2560, 1440).fsr_pass && !plan(true, false, 1920, 1080, 2560, 1440).rcas,
          "FSR 1 upscales a bigger window without sharpening");
    Check(plan(true, true, 1920, 1080, 3840, 2160).fsr_pass && plan(true, true, 1920, 1080, 3840, 2160).rcas,
          "FSR 1 with sharpening in a bigger window");
    Check(!plan(true, false, 1920, 1080, 1920, 1080).fsr_pass, "FSR 1 does not run at the render size");
    Check(!plan(true, false, 1920, 1080, 1280, 720).fsr_pass && !plan(true, true, 1920, 1080, 1280, 720).fsr_pass,
          "a smaller output is never upscaled");
    Check(!plan(true, false, 2560, 1080, 2560, 1440).fsr_pass, "an output wider but not taller is not upscaled");
    Check(!plan(true, false, 1920, 1080, 1920, 1440).fsr_pass, "an output taller but not wider is not upscaled");
    Check(plan(false, true, 1920, 1080, 1920, 1080).rcas, "sharpening alone applies at the render size");
    Check(!plan(false, true, 1920, 1080, 2560, 1440).rcas, "sharpening alone does not apply in a bigger window");
    Check(plan(false, true, 1920, 1080, 1920, 1080).fsr_pass && !plan(false, false, 1920, 1080, 1920, 1080).rcas,
          "Off bypasses RCAS");

    const struct { int strength; float attenuation; } table[] = {{0, 2.0f}, {25, 1.5f}, {50, 1.0f}, {75, 0.5f}, {100, 0.0f}};
    for (const auto& row : table) {
        Check(DcpGraphics::RcasAttenuation(row.strength) == row.attenuation,
              "strength " + std::to_string(row.strength) + " attenuates by " + std::to_string(row.attenuation));
    }
    float previous = 3.0f;
    for (int strength = 0; strength <= 100; ++strength) {
        const float value = DcpGraphics::RcasAttenuation(strength);
        Check(value >= 0.0f && value <= 2.0f && value <= previous, "attenuation stays in 0..2 and falls with strength");
        previous = value;
    }
    for (const int outside : {-1, -100, 101, 1000}) {
        const float value = DcpGraphics::RcasAttenuation(outside);
        Check(value >= 0.0f && value <= 2.0f && value == 0.5f, "strength " + std::to_string(outside) + " cannot leave the shader range");
    }
    const std::string presenter = Source(kPresenter);
    Check(Contains(presenter, "fsr_settings.rcas_attenuation = DcpGraphics::RcasAttenuation(graphics.rcas_strength);"),
          "the strength reaches the pass settings");
    Check(Contains(presenter, "DcpGraphics::PlanPostProcess(graphics.smaa, fsr_settings.enable, fsr_settings.use_rcas,"),
          "the readout of whether sharpening ran uses the same plan");
    const std::string pass = Source("gpu/shadps4/video_core/renderer_vulkan/host_passes/fsr_pass.cpp");
    Check(Contains(pass, "DcpGraphics::FsrPassRuns(settings.enable, settings.use_rcas, int(input_size.width),"),
          "the FSR pass bails out with the plan's condition");
    Check(InOrder(pass, "DcpGraphics::FsrPassRuns(", "return input;") && InOrder(pass, "return input;", "ResizeAndInvalidate("),
          "a bypassed pass returns the input unchanged");
    Check(Contains(pass, "if (output_size != cur_size) {") && Contains(pass, "ResizeAndInvalidate(output_size.width, output_size.height);"),
          "the FSR images are recreated when the output size changes");
    Check(Contains(pass, "rcas_attenuation"), "the strength is an RCAS shader constant");

    const auto read = [&](const std::string& text) {
        const fs::path file = fs::temp_directory_path() / ("sos_rcas_" + std::to_string(std::rand()) + ".ini");
        std::ofstream(file) << text;
        DcpSettings::Values fresh;
        DcpSettings::Read(file.string(), fresh);
        std::error_code ec;
        fs::remove(file, ec);
        return fresh.rcas_strength.load();
    };
    for (const int strength : {0, 25, 50, 75, 100}) {
        Check(read("rcas_strength = " + std::to_string(strength) + "\n") == strength, "strength " + std::to_string(strength) + " is read");
    }
    Check(read("rcas_strength = 250\n") == 75 && read("rcas_strength = -5\n") == 75 && read("rcas_strength = abc\n") == 75,
          "out of range or invalid strength reads as the default 75");
}

void TestVsync() {
    using DcpPresent::ChooseFromSurface;
    using DcpPresent::ChooseMode;
    using DcpPresent::Mode;
    Check(int(Mode::Immediate) == 0 && int(Mode::Mailbox) == 1 && int(Mode::Fifo) == 2, "modes are the VkPresentModeKHR values");
    const std::vector<int> fifo{2}, immediate_fifo{0, 2}, mailbox_fifo{1, 2}, all{0, 1, 2}, none;
    Check(ChooseFromSurface(true, all) == Mode::Fifo && ChooseFromSurface(true, fifo) == Mode::Fifo, "On is FIFO");
    Check(ChooseFromSurface(false, all) == Mode::Mailbox, "Off prefers MAILBOX");
    Check(ChooseFromSurface(false, immediate_fifo) == Mode::Immediate, "Off falls back to IMMEDIATE");
    Check(ChooseFromSurface(false, mailbox_fifo) == Mode::Mailbox, "Off uses MAILBOX when IMMEDIATE is missing");
    Check(ChooseFromSurface(false, fifo) == Mode::Fifo && ChooseFromSurface(false, none) == Mode::Fifo,
          "Off falls back to FIFO when nothing else is offered");
    for (int subset = 0; subset < 4; ++subset) {
        std::vector<int> supported{2};
        if (subset & 1) supported.push_back(1);
        if (subset & 2) supported.push_back(0);
        for (const bool vsync : {true, false}) {
            const int chosen = int(ChooseFromSurface(vsync, supported));
            Check(std::find(supported.begin(), supported.end(), chosen) != supported.end(),
                  "the chosen mode is one the surface offers");
        }
    }
    Check(ChooseMode(false, true, false) == Mode::Mailbox && ChooseMode(false, false, true) == Mode::Immediate,
          "the flag based choice agrees");

    const std::string swapchain = Source("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    Check(InOrder(swapchain, "vsync = DcpSettings::Get().vsync;", "Failed to query available present modes"),
          "the applied VSync is recorded even when the mode query fails");
    Check(InOrder(swapchain, "void Swapchain::UpdateVsync()", "if (DcpSettings::Get().vsync == vsync) {") &&
              InOrder(swapchain, "WaitDeviceIdle(instance, \"before a VSync change\");", "FindPresentMode();") &&
              InOrder(swapchain, "FindPresentMode();\n    Recreate(width, height);", "void Swapchain::SetSurfaceProperties()"),
          "a changed setting picks a new mode and recreates the swapchain");
    Check(Contains(swapchain, ".presentMode = present_mode,"), "the swapchain is created with the chosen mode");
    const std::string presenter = Source(kPresenter);
    size_t updates = 0;
    for (size_t at = presenter.find("swapchain.UpdateVsync();"); at != std::string::npos; at = presenter.find("swapchain.UpdateVsync();", at + 1)) {
        ++updates;
    }
    Check(updates == 2, "both the render and the present path apply a VSync change");
    DcpSettings::Values defaults;
    Check(defaults.vsync, "an unreadable or stale VSync value is On");
}

void TestPresentation() {
    using DcpPresent::DestinationRegion;
    using DcpPresent::GameFrameSize;
    using DcpPresent::Region;
    using DcpPresent::Size;
    const struct { float ratio; const char* name; } images[] = {{16.0f / 9.0f, "16:9"}, {16.0f / 10.0f, "16:10"}, {2560.0f / 1080.0f, "21:9"},
                                                                  {32.0f / 9.0f, "32:9"}, {4.0f / 3.0f, "4:3"}, {5.0f / 4.0f, "5:4"}};
    const Size surfaces[] = {{1920, 1080}, {2560, 1440}, {1280, 1024}, {3440, 1440}, {1600, 1000}, {800, 600}, {3840, 1080}, {1000, 1000}, {3840, 2160}};
    for (const auto& image : images) {
        for (const Size surface : surfaces) {
            const std::string label = std::string(image.name) + " in " + std::to_string(surface.width) + "x" + std::to_string(surface.height);
            const Size frame = GameFrameSize(surface, image.ratio);
            Check(frame.width <= surface.width && frame.height <= surface.height && frame.width > 0 && frame.height > 0,
                  "the frame fits the surface: " + label);
            const Region fit = DestinationRegion(frame, surface, false);
            Check(fit.x >= 0 && fit.y >= 0 && fit.x + int(fit.width) <= int(surface.width) && fit.y + int(fit.height) <= int(surface.height),
                  "letterbox stays inside the surface: " + label);
            Check(std::abs(fit.x * 2 + int(fit.width) - int(surface.width)) <= 1 && std::abs(fit.y * 2 + int(fit.height) - int(surface.height)) <= 1,
                  "letterbox is centred: " + label);
            const bool fills_width = std::abs(int(fit.width) - int(surface.width)) <= 2;
            const bool fills_height = std::abs(int(fit.height) - int(surface.height)) <= 2;
            Check(fills_width || fills_height, "bars on one axis only, no double letterbox: " + label);
            const float shown = float(fit.width) / float(fit.height);
            Check(std::abs(shown - image.ratio) / image.ratio < 0.01f, "letterbox keeps the image shape: " + label);
            const float surface_ratio = float(surface.width) / float(surface.height);
            const float expected_w = image.ratio > surface_ratio ? float(surface.width) : float(surface.height) * image.ratio;
            Check(std::abs(float(fit.width) - expected_w) <= 3.0f, "letterbox is the largest fit of the image: " + label);
            Check(DestinationRegion(frame, surface, true) == Region{0, 0, surface.width, surface.height}, "stretch fills the surface: " + label);
        }
    }
    Check(DestinationRegion({1920, 1080}, {1280, 1024}, false) == Region{0, 152, 1280, 720}, "16:9 in a 5:4 window has bars above and below");
    Check(DestinationRegion({1920, 1080}, {2560, 1080}, false) == Region{320, 0, 1920, 1080}, "16:9 in a 21:9 window has bars at the sides");
    Check(DestinationRegion({1920, 1080}, {1920, 1080}, false) == Region{0, 0, 1920, 1080}, "an exact fit has no bars");
    Check(GameFrameSize({1920, 1080}, 4.0f / 3.0f) == DcpPresent::Size{1440, 1080}, "a 4:3 image in a 16:9 surface is a 1440x1080 frame");
    Check(GameFrameSize({1920, 1080}, 2560.0f / 1080.0f) == DcpPresent::Size{1920, 810}, "a 21:9 image in a 16:9 surface is a 1920x810 frame");

    const std::string presenter = Source(kPresenter);
    Check(Contains(presenter, "DcpSettings::Get().presentation.load() == DcpGraphics::Stretch") &&
              InOrder(presenter, "const bool stretch =", "DcpPresent::DestinationRegion("),
          "Presentation reaches the destination rectangle");
    Check(InOrder(presenter, "DestinationRegion(", "cmdbuf.blitImage(frame->image,"), "the blit uses that rectangle");
    Check(Contains(presenter, "const DcpPresent::Size frame = DcpPresent::GameFrameSize("), "the frame is sized from the image's shape");
    Check(DcpGraphics::ParsePresentation("stretch") == DcpGraphics::Stretch && DcpGraphics::ParsePresentation("letterbox") == DcpGraphics::Letterbox &&
              DcpGraphics::ParsePresentation("zoom") == DcpGraphics::Letterbox,
          "presentation values parse, an invalid one letterboxes");
}

void TestFramePacing() {
    namespace Pacing = DcpFramePacing;
    for (const int setting : Pacing::kFrameRateModes) {
        int shown = 0;
        for (std::uint64_t flip = 0; flip < 60; ++flip) {
            shown += Pacing::PresentsFlip(setting, flip) ? 1 : 0;
        }
        Check(shown == (setting == 30 ? 30 : 60), "one second of 60 Hz flips shows the right count at " + std::to_string(setting));
        int halved = 0;
        for (std::uint64_t flip = 0; flip < 30; ++flip) {
            halved += Pacing::PresentsFlip(setting, flip, 1) ? 1 : 0;
        }
        Check(halved == 30, "a game already flipping at 30 is never shown at 15: " + std::to_string(setting));
    }
    const struct { int setting; unsigned vblank; unsigned shown; } free_run[] = {
        {30, 60, 30}, {60, 60, 60}, {90, 90, 90}, {120, 120, 120}, {144, 144, 144}, {165, 165, 165}, {240, 240, 240}, {0, 480, 480}};
    for (const auto& row : free_run) {
        Check(Pacing::GuestVblankHz(row.setting) == row.vblank, "VSync off guest vblank at " + std::to_string(row.setting));
        Check(Pacing::ShownFramesPerSecond(row.setting) == row.shown, "VSync off new frames shown at " + std::to_string(row.setting));
    }
    const struct { int setting; double display; unsigned vblank; } synced[] = {
        {90, 144.0, 90}, {165, 144.0, 144}, {240, 144.0, 144}, {0, 144.0, 144}, {0, 59.94, 60}, {144, 59.94, 60},
        {120, 60.0, 60}, {240, 240.0, 240}, {0, 165.0, 165}, {60, 144.0, 60}, {30, 144.0, 60}, {120, 50.0, 60}};
    for (const auto& row : synced) {
        Check(Pacing::GuestVblankHz(row.setting, true, row.display) == row.vblank,
              "VSync on, " + std::to_string(int(row.display)) + " Hz display, setting " + std::to_string(row.setting));
    }
    Check(Pacing::GuestVblankHz(240, true, 0.0) == 240, "an unknown display refresh leaves the rate unbounded");
    Check(Pacing::GuestVblankHz(61) == 60 && Pacing::GuestVblankHz(-1) == 60, "an unknown setting is the original 60");
    for (const int setting : Pacing::kFrameRateModes) {
        Check(Pacing::GuestVblankHz(setting) >= Pacing::kGameHz, "the guest vblank is never below 60: " + std::to_string(setting));
    }
    Check(Pacing::kGameHz == 60, "the original game rate is 60");

    const std::string driver = Source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    Check(Contains(driver, "PresentsFlip(DcpSettings::Get().frame_rate.load()") && Contains(driver, "req.port->flip_rate"),
          "the driver filters flips by the setting and the port's flip rate");
    Check(Contains(driver, "EmulatorSettings.SetVblankFrequency(DcpFramePacing::GuestVblankHz(") &&
              Contains(driver, "settings.frame_rate.load(), settings.vsync.load(), settings.display_refresh_hz.load()"),
          "the guest vblank comes from the Frame rate setting, VSync and the display refresh");
    Check(InOrder(driver, "SetVblankFrequency(", "if (EmulatorSettings.GetVblankFrequency() >= 120)"),
          "the vblank is chosen before the reported refresh rate");
    Check(Contains(driver, "vblank_period(1000000000 /") && Contains(driver, "EmulatorSettings.GetVblankFrequency());"),
          "the present thread ticks at the guest vblank");
    Check(!Contains(driver, "RepeatLastFrame") && !Contains(driver, "FrameLimiter"), "no frame is shown again to reach a rate");
    Check(Contains(Source("gpu/shim/core/emulator_settings.h"), "u32 GetVblankFrequency() { return vblank_frequency; }") &&
              Contains(Source("gpu/shim/core/emulator_settings.h"), "u32 vblank_frequency = 60;"),
          "the vblank is 60 Hz until the driver sets it");
    Check(Contains(driver, "vblank %u Hz, frame rate %s, new frames up to %u a second"), "the run log states the vblank and the new frames");
    const std::string presenter = Source(kPresenter);
    Check(InOrder(presenter, "if (!is_reusing_frame && is_game_frame) {", "DcpOverlay::LogPresent();") &&
              InOrder(presenter, "DcpOverlay::LogPresent();", "DcpOverlay::Render(cmdbuf"),
          "only a new game frame counts for the title and the counter");
}

} // namespace

int main() {
    TestTextureFiltering();
    TestSmaa();
    TestFsrAndRcas();
    TestVsync();
    TestPresentation();
    TestFramePacing();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
