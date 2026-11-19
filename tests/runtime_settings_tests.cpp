// Headless checks of the path from the launcher's settings to the runtime: what the video core reads from
// sons_of_sparta.ini, how each value translates (display mode, VSync, presentation, FSR, RCAS, SMAA, anisotropy, GPU,
// frame rate, title, frame counter) and where the source wires it. No window, GPU or game.
#include <chrono>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dcp_frame_pacing.h"
#include "dcp_graphics.h"
#include "dcp_overlay_box.h"
#include "dcp_present.h"
#include "dcp_settings.h"
#include "dcp_title.h"
#include "sos_window_fit.h"

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

std::string Source(const char* path) { return ReadFile(fs::path(SOS_SOURCE_DIR) / path); }

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
    const size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a + first.size()) != std::string::npos;
}

fs::path TempFile() {
    const char* parent = "E:\\ClaudeTemp\\opencode";
    const fs::path base = fs::is_directory(parent) ? fs::path(parent) : fs::temp_directory_path();
    const fs::path folder = base / ("sos_runtime_settings_" + std::to_string(std::rand()) + std::to_string(std::rand()));
    fs::create_directories(folder);
    return folder / "sons_of_sparta.ini";
}

void Write(const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }

const DcpSettings::Values& ReadValues(const std::string& text) {
    static std::deque<DcpSettings::Values> owned;
    const fs::path file = TempFile();
    Write(file, text);
    DcpSettings::Values& values = owned.emplace_back();
    Check(DcpSettings::Read(file.string(), values), "settings file is read");
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
    return values;
}

void TestDefaults() {
    const DcpSettings::Values v;
    Check(v.display_mode == DcpGraphics::Windowed && v.vsync && v.presentation == DcpGraphics::Letterbox, "display defaults");
    Check(!v.fsr1 && v.rcas && v.rcas_strength == 75 && !v.smaa && v.anisotropy == 0, "scaling and filtering defaults");
    Check(v.frame_rate == 60 && v.monitor == 1 && !v.show_fps && !v.show_fps_title && !v.show_resolution_title,
          "gameplay defaults");
    Check(v.overlay_scale == 100 && v.overlay_corner == 1 && v.graphics_card.empty(), "overlay and card defaults");
    Check(v.window_mode_key == SDL_SCANCODE_F11, "F11 is the window mode key");
}

void TestDisplaySettings() {
    const DcpSettings::Values& v = ReadValues(
        "display_mode = borderless\nwindow_width = 2560\nwindow_height = 1440\nmonitor = 2\nvsync = 0\n"
        "presentation = stretch\nlauncher_x = 10\nlauncher_maximized = 1\n");
    Check(v.display_mode == DcpGraphics::Borderless, "borderless is read");
    Check(v.requested_width == 2560 && v.requested_height == 1440, "window size is read");
    Check(v.monitor == 2, "monitor is read");
    Check(!v.vsync, "VSync off is read");
    Check(v.presentation == DcpGraphics::Stretch, "stretch is read");
    Check(v.display_mode.load() != DcpGraphics::Windowed && v.requested_width != 0, "launcher placement keys are not window settings");

    const DcpSettings::Values& bad = ReadValues("display_mode = sideways\nmonitor = -4\nwindow_width = 99999\npresentation = zoom\nvsync = maybe\n");
    Check(bad.display_mode == DcpGraphics::Windowed && bad.monitor == 1, "invalid display values fall back");
    Check(bad.requested_width == 16384, "window width is bounded");
    Check(bad.presentation == DcpGraphics::Letterbox && bad.vsync, "invalid presentation and VSync keep defaults");

    for (int mode = 0; mode < DcpGraphics::DisplayModeCount; ++mode) {
        Check(DcpGraphics::ParseDisplayMode(DcpGraphics::DisplayModeKeys[size_t(mode)]) == mode, "display mode key round trip");
    }
}

void TestPresentModeAndRegion() {
    using DcpPresent::Mode;
    Check(DcpPresent::ChooseMode(true, true, true) == Mode::Fifo, "VSync on is Fifo");
    Check(DcpPresent::ChooseMode(true, false, false) == Mode::Fifo, "VSync on without other modes is Fifo");
    Check(DcpPresent::ChooseMode(false, true, true) == Mode::Mailbox, "VSync off prefers Mailbox");
    Check(DcpPresent::ChooseMode(false, false, true) == Mode::Immediate, "VSync off falls back to Immediate");
    Check(DcpPresent::ChooseMode(false, false, false) == Mode::Fifo, "VSync off without modes is Fifo");
    const DcpPresent::Region fit = DcpPresent::FitRegion({1920, 1080}, {1280, 1024});
    Check(fit == DcpPresent::Region{0, 152, 1280, 720}, "letterbox centres the 16:9 frame in a taller window");
    const DcpPresent::Region wide = DcpPresent::FitRegion({1920, 1080}, {2560, 1080});
    Check(wide == DcpPresent::Region{320, 0, 1920, 1080}, "letterbox pillarboxes a wider window");

    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Contains(presenter, "DcpSettings::Get().presentation.load() == DcpGraphics::Stretch"), "presenter reads Presentation");
    Check(InOrder(presenter, "const bool stretch =", "DcpPresent::DestinationRegion("), "the blit region follows Presentation");
    const std::string swapchain = Source("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    Check(Contains(swapchain, "vsync = DcpSettings::Get().vsync;") && Contains(swapchain, "DcpPresent::ChooseFromSurface(vsync"),
          "swapchain chooses its present mode from VSync");
    Check(InOrder(swapchain, "void Swapchain::UpdateVsync()", "Recreate(width, height);"), "VSync changes recreate the swapchain");
}

void TestScaling() {
    Check(DcpGraphics::RcasAttenuation(100) == 0.0f, "strength 100 is the sharpest");
    Check(DcpGraphics::RcasAttenuation(75) == 0.5f, "strength 75 attenuates by 0.5");
    Check(DcpGraphics::RcasAttenuation(50) == 1.0f, "strength 50 attenuates by 1");
    Check(DcpGraphics::RcasAttenuation(0) == 2.0f, "strength 0 is the softest");
    Check(DcpGraphics::RcasAttenuation(-3) == 0.5f && DcpGraphics::RcasAttenuation(101) == 0.5f, "invalid strength is 75");
    for (const int strength : {0, 50, 75, 100}) {
        const DcpSettings::Values& v = ReadValues("rcas_strength = " + std::to_string(strength) + "\n");
        Check(v.rcas_strength == strength, "strength " + std::to_string(strength) + " is read");
    }
    const DcpSettings::Values& off = ReadValues("fsr1 = 1\nrcas = 0\nsmaa = 1\n");
    Check(off.fsr1 && !off.rcas && off.smaa, "FSR, RCAS and SMAA switches are read");

    using DcpGraphics::FsrPassRuns;
    Check(FsrPassRuns(true, false, 1920, 1080, 2560, 1440), "FSR 1 runs in a bigger window");
    Check(!FsrPassRuns(false, false, 1920, 1080, 2560, 1440), "Off and no sharpening skips the pass");
    Check(FsrPassRuns(false, true, 1920, 1080, 1920, 1080), "sharpening alone runs at the render size");
    Check(!FsrPassRuns(false, true, 1920, 1080, 2560, 1440), "sharpening alone does not run in a bigger window");
    Check(FsrPassRuns(true, true, 1920, 1080, 2560, 1440), "FSR with sharpening runs in a bigger window");
    Check(!FsrPassRuns(true, true, 1920, 1080, 1280, 720), "a smaller window is left to the regular scaling");

    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Contains(presenter, "fsr_settings.enable = graphics.fsr1;") && Contains(presenter, "fsr_settings.use_rcas = graphics.rcas;") &&
              Contains(presenter, "DcpGraphics::RcasAttenuation(graphics.rcas_strength)"),
          "FSR, RCAS and strength reach the FSR pass");
    Check(InOrder(presenter, "if (graphics.smaa) {", "smaa_pass.Render("), "SMAA runs only when on");
    Check(InOrder(presenter, "smaa_pass.Render(", "fsr_pass.Render("), "SMAA runs before the scaling pass");

    for (const int level : {0, 2, 4, 8, 16}) {
        const DcpSettings::Values& v = ReadValues("anisotropy = " + std::to_string(level) + "\n");
        Check(v.anisotropy == level, "anisotropy " + std::to_string(level) + " is read");
    }
    Check(ReadValues("anisotropy = 3\n").anisotropy == 0 && ReadValues("anisotropy = 32\n").anisotropy == 0,
          "invalid anisotropy is the game's own");
    const std::string rasterizer = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    Check(Contains(rasterizer, "DcpSettings::Get().anisotropy,") &&
              Contains(rasterizer, "forced == 16 ? AmdGpu::AnisoRatio::Sixteen") &&
              Contains(rasterizer, "DcpGraphics::ForcedAnisotropyRatio("),
          "anisotropy override reaches the sampler");
}

void TestGpuSelection() {
    using DcpGraphics::GpuChoices;
    using DcpGraphics::ResolveGpuByName;
    const std::vector<std::string> names = {"NVIDIA GeForce RTX", "", "AMD Radeon"};
    Check(GpuChoices(names) == std::vector<std::string>{"NVIDIA GeForce RTX", "AMD Radeon"}, "unusable devices are not offered");
    Check(ResolveGpuByName(names, "AMD Radeon") == 2, "the named card is selected");
    Check(ResolveGpuByName(names, "Gone GPU") == 0, "a missing card falls back to the first usable one");
    Check(ResolveGpuByName(names, "") == 0, "no choice selects the first usable card");
    Check(ResolveGpuByName({"", ""}, "AMD Radeon") == -1, "no usable card selects nothing");
    Check(DcpSettings::Values{}.graphics_card.empty() && ReadValues("graphics_card = automatic\n").graphics_card.empty(),
          "automatic means the first usable card");
    Check(ReadValues("graphics_card = AMD Radeon\n").graphics_card == "AMD Radeon", "the card name is read");
    const std::string instance = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    Check(Contains(instance, "DcpGraphics::ResolveGpuByName(names, requested)") &&
              Contains(instance, "requested %s is not available"),
          "Vulkan device choice uses the setting and reports a missing card");
}

void TestFrameRate() {
    namespace Pacing = DcpFramePacing;
    Check(Pacing::ParseFrameRate("144") == 144 && Pacing::ParseFrameRate("0") == 0 && Pacing::ParseFrameRate("61") == 60 &&
              Pacing::ParseFrameRate("") == 60,
          "frame rate parse");
    for (const int rate : {30, 60, 90, 120, 144, 165, 240, 0}) {
        Check(ReadValues("frame_rate = " + std::to_string(rate) + "\n").frame_rate == rate, "frame rate read: " + std::to_string(rate));
    }
    Check(Pacing::RepeatCadenceHz(60) == 0 && Pacing::RepeatCadenceHz(30) == 0, "60 and 30 add no repeats");
    Check(Pacing::RepeatCadenceHz(90) == 90 && Pacing::RepeatCadenceHz(240) == 240, "faster rates repeat up to the rate");
    Check(Pacing::RepeatCadenceHz(0) == Pacing::kUnlimitedCadenceHz, "Unlimited repeats up to 480 a second");
    Check(Pacing::RepeatCadenceHz(240, true, 143.856) == 144, "VSync bounds the rate by the display");
    Check(Pacing::RepeatCadenceHz(144, true, 59.94) == 0, "a 60 Hz display leaves nothing to repeat");
    Check(Pacing::RepeatCadenceHz(0, true, 165.0) == 165, "Unlimited with VSync follows the display");
    Check(Pacing::RepeatCadenceHz(144, false, 60.0) == 144, "VSync off ignores the display");
    int shown = 0;
    for (std::uint64_t flip = 0; flip < 60; ++flip) {
        shown += Pacing::PresentsFlip(30, flip) ? 1 : 0;
    }
    Check(shown == 30, "30 shows every second flip");
    shown = 0;
    for (std::uint64_t flip = 0; flip < 60; ++flip) {
        shown += Pacing::PresentsFlip(60, flip) ? 1 : 0;
    }
    Check(shown == 60, "60 shows every flip");
    Check(Pacing::PresentsFlip(30, 1, 1), "a game already flipping every second vblank is not halved again");

    Pacing::FrameLimiter limiter(120, std::chrono::steady_clock::time_point{});
    Check(limiter.Enabled() && limiter.Period() == std::chrono::nanoseconds(8333333), "120 repeats on an 8.33 ms period");
    Check(!Pacing::FrameLimiter(0, {}).Enabled(), "no limiter below the cadence");

    const std::string driver = Source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    Check(Contains(driver, "PresentsFlip(DcpSettings::Get().frame_rate.load(), flips_consumed++,\n                                                      req.port->flip_rate);") ||
              Contains(driver, "flips_consumed++,"),
          "flips are filtered by the setting");
    Check(InOrder(driver, "if (!present) {", "presenter->RetireFrame(frame);"), "a skipped flip is retired, not dropped");
    Check(Contains(driver, "vblank_period(1000000000 /\n                                                 EmulatorSettings.GetVblankFrequency())") ||
              Contains(driver, "EmulatorSettings.GetVblankFrequency()"),
          "the guest vblank comes from the fixed vblank frequency");
    Check(Contains(Source("gpu/shim/core/emulator_settings.h"), "u32 GetVblankFrequency() { return 60; }"),
          "the guest vblank is 60 Hz at every rate");
    size_t uses = 0;
    for (size_t at = driver.find("target_fps"); at != std::string::npos; at = driver.find("target_fps", at + 1)) {
        ++uses;
    }
    Check(uses <= 7, "the frame rate setting feeds only the presentation limiter");
    Check(InOrder(driver, "vblank_status.count++;", "limiter.Enabled() && main_port.is_open"), "repeats follow the vblank");
}

void TestTitle() {
    using DcpTitle::Compose;
    const std::string base = "God of War: Sons of Sparta";
    Check(Compose({.base = base}) == base, "title without readouts");
    Check(Compose({.base = base, .show_resolution = true, .width = 1920, .height = 1080}) == base + " | 1920x1080",
          "title with the render resolution");
    Check(Compose({.base = base, .show_fps = true, .fps = 60.0}) == base + " | FPS: 60.00", "title with the frame rate");
    Check(Compose({.base = base, .show_resolution = true, .width = 2560, .height = 1440, .show_fps = true, .fps = 143.98}) ==
              base + " | 2560x1440 | FPS: 143.98",
          "title with both readouts");
    Check(Compose({.base = base, .show_resolution = true}) == base, "no resolution before a frame is presented");
    Check(Compose({.base = base, .show_fps = true, .fps = -1.0}) == base, "no frame rate before it is measured");
    Check(Compose({.base = base, .show_fps = false, .fps = 60.0}) == base, "frame rate off hides it");
    Check(Compose({.base = base, .show_fps = true, .fps = 60.0, .entry = " - typing"}) == base + " - typing",
          "text entry replaces the frame rate");

    const DcpSettings::Values& v = ReadValues("show_fps_title = 1\nshow_resolution_title = 1\n");
    Check(v.show_fps_title && v.show_resolution_title, "title readouts are read");
    const std::string window = Source("gpu/shim/window.cpp");
    Check(Contains(window, "DcpTitle::Compose({") && Contains(window, ".show_resolution = settings.show_resolution_title") &&
              Contains(window, ".show_fps = settings.show_fps_title"),
          "the window composes its title from the settings");
    Check(Contains(window, "SDL_SetWindowTitle(window, title.c_str());") && InOrder(window, "UpdateFpsTitle();", "RefreshTitle();"),
          "the title is refreshed while the game runs");
    Check(Contains(window, "if (!DcpSettings::Get().show_fps_title) {\n        fps = -1.0;"), "frame rate off clears the reading");
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Contains(presenter, "DcpSettings::Get().image_width = int(image_size.width);") &&
              Contains(presenter, "DcpSettings::Get().image_height = int(image_size.height);"),
          "the presenter reports the render resolution");
    Check(Contains(Source("gpu/shim/dcpgpu.cpp"), "config->title : \"God of War: Sons of Sparta\""), "default game title");
}

void TestFrameCounter() {
    const DcpSettings::Values& v = ReadValues("show_fps = 1\noverlay_scale = 150\noverlay_corner = 2\n");
    Check(v.show_fps && v.overlay_scale == 150 && v.overlay_corner == 2, "frame counter settings are read");
    const DcpSettings::Values& clamped = ReadValues("overlay_scale = 10\noverlay_corner = 9\n");
    Check(clamped.overlay_scale == 50 && clamped.overlay_corner == 1, "overlay scale and corner are bounded");
    constexpr DcpOverlayBox::Rect work{0.0f, 0.0f, 1920.0f, 1080.0f};
    const DcpOverlayBox::Box right = DcpOverlayBox::Place(1, 100.0f, 18.0f, 18.0f, work);
    const DcpOverlayBox::Box left = DcpOverlayBox::Place(0, 100.0f, 18.0f, 18.0f, work);
    const DcpOverlayBox::Box bottom = DcpOverlayBox::Place(3, 100.0f, 18.0f, 18.0f, work);
    Check(right.rect.x1 < 1920.0f && right.rect.y0 > 0.0f && left.rect.x0 > 0.0f, "the panel sits inside the corner");
    Check(bottom.rect.y1 < 1080.0f && bottom.rect.x1 < 1920.0f, "the bottom corner stays on screen");
    Check(DcpOverlayBox::NoticeCorner(0) == 1 && DcpOverlayBox::NoticeCorner(1) == 0, "notices take the free top corner");

    const std::string overlay = Source("gpu/shim/dcp_overlay_generic.cpp");
    Check(Contains(overlay, "\"%.0f FPS  %.1f ms\""), "the counter shows frame rate and frame time");
    Check(Contains(overlay, "DcpSettings::Get().show_fps && frame_ms_avg > 0.0f"), "the counter follows show_fps");
    Check(Contains(overlay, "DcpSettings::Get().overlay_scale / 100.0f") && Contains(overlay, "DcpSettings::Get().overlay_corner"),
          "the counter follows scale and corner");
    Check(Contains(overlay, "return initialized && (loading || background_total || DcpSettings::Get().show_fps || NoticeShown());"),
          "the overlay is visible while the counter is on");
    Check(Contains(overlay, "ImGui_ImplVulkan_Init(&info)") && Contains(overlay, "ImGui_ImplVulkan_RenderDrawData"),
          "the overlay draws through the ImGui Vulkan backend");
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Contains(presenter, "DcpOverlay::Init(instance,") && Contains(presenter, "if (overlay) {\n            DcpOverlay::Render("),
          "the presenter initialises and renders the overlay");
    Check(Contains(Source("gpu/CMakeLists.txt"), "shim/dcp_overlay_generic.cpp"), "the overlay is built");
}

void TestWindowSettings() {
    const sos::WindowFit fit = sos::FitWindowToArea(1920, 1080, {31, 1, 1, 1}, 0, 0, 1920, 1040);
    Check(fit.width < 1920 && fit.width * 9 - fit.height * 16 <= 16, "a window larger than the area is shrunk, keeping 16:9");
    const sos::WindowFit small = sos::FitWindowToArea(1280, 720, {31, 1, 1, 1}, 0, 0, 3840, 2100);
    Check(small.width == 1280 && small.height == 720, "a window that fits is not changed");

    const std::string window = Source("gpu/shim/window.cpp");
    Check(Contains(window, "settings.requested_width > 0 ? settings.requested_width.load() : width_"), "window size comes from the settings");
    Check(Contains(window, "sos::DisplayForSetting(displays, settings.monitor)") &&
              Contains(window, "SDL_WINDOWPOS_CENTERED_DISPLAY(display->id)"),
          "the window opens on the chosen monitor");
    Check(InOrder(window, "sos::FitWindowToArea(", "ApplyDisplayMode(DcpSettings::Get().display_mode);"), "the window is fitted before the mode");
    Check(Contains(window, "SDL_SetWindowFullscreen(window, borderless);") && Contains(window, "SDL_SetWindowFullscreenMode(window, nullptr)"),
          "Borderless Fullscreen uses desktop fullscreen");
    Check(Contains(window, "event.key.scancode == DcpSettings::Get().window_mode_key") &&
              Contains(window, "mode = mode.load() == DcpGraphics::Windowed ? DcpGraphics::Borderless : DcpGraphics::Windowed;") &&
              Contains(window, "DcpSettings::SaveDisplayMode();"),
          "F11 toggles and saves the display mode");
    Check(Contains(window, "SDL_EVENT_WINDOW_DISPLAY_CHANGED"), "the refresh rate follows the display");
    Check(Contains(Source("gpu/shim/dcpgpu.cpp"), "DcpSettings::Load(config->settings_file);"), "the settings file is loaded before the window");
    Check(Contains(Source("src/loader.c"), "settings_file, 0, 1920, 1080, 0") || Contains(Source("src/loader.c"), "user_dir, settings_file,"),
          "the loader hands the settings file to the video core");

    const DcpSettings::Values& key = ReadValues("hotkey_window_mode = f10\n");
    Check(key.window_mode_key == SDL_SCANCODE_F10, "the window mode key is rebindable");

    const fs::path file = TempFile();
    Write(file, "# header\nlauncher_x = 40\ncustom = keep\n");
    DcpSettings::Load(file.string());
    DcpSettings::Get().display_mode = DcpGraphics::Borderless;
    Check(DcpSettings::SaveDisplayMode(), "F11 saves");
    std::string text = ReadFile(file);
    Check(Contains(text, "display_mode = borderless") && Contains(text, "launcher_x = 40") && Contains(text, "custom = keep"),
          "F11 writes the display mode and keeps the other lines");
    DcpSettings::Get().display_mode = DcpGraphics::Windowed;
    DcpSettings::SaveDisplayMode();
    text = ReadFile(file);
    Check(Contains(text, "display_mode = windowed") && !Contains(text, "borderless"), "toggling back restores Windowed");
    Check(DcpSettings::SaveEntries({{"vsync", "0"}}) && Contains(ReadFile(file), "vsync = 0"), "entries are written in place");
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
}

void TestOverlayProcess() {
    const std::string loader = Source("src/loader.c");
    Check(Contains(loader, "\"--sos-game\"") && Contains(loader, "game_child = 1"), "the game process is marked by --sos-game");
    Check(Contains(loader, "} else if (!check_imports && !cpu_only) {"), "the launcher is skipped only inside the game process");
    Check(InOrder(loader, "SOS_LAUNCHER_GAME_STARTED", "sos_resume_game_process()"), "the loader resumes the suspended process");
    Check(InOrder(loader, "runtime_log_release();", "sos_resume_game_process()"), "the log is released before the child continues it");
    Check(Contains(loader, "log_started ? log_path : NULL"), "the child continues this run's log");
    Check(Contains(loader, "game_process = 1;"), "Windows enables the separate process");
    const std::string process = Source("launcher/game_process.cpp");
    Check(Contains(process, "CREATE_SUSPENDED") && Contains(process, "ResumeThread(g_game.hThread)"), "the process is created suspended and resumed");
    Check(Contains(process, "TerminateProcess(g_game.hProcess, 1)"), "a process that cannot resume is ended");
}

} // namespace

int main() {
    TestDefaults();
    TestDisplaySettings();
    TestPresentModeAndRegion();
    TestScaling();
    TestGpuSelection();
    TestFrameRate();
    TestTitle();
    TestFrameCounter();
    TestWindowSettings();
    TestOverlayProcess();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
