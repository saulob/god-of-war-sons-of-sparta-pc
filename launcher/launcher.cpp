#include "sos_launcher.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "trg/launcher.h"
#include "trg/platform.h"

#include "game_files.h"
#include "game_install.h"
#include "game_profile.h"
#include "host.h"
#include "localization/ui_text.h"
#include "options.h"
#include "settings.h"
#include "utf8.h"

extern "C" int dcp_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size);
extern "C" int dcp_gpu_names(char (*names)[256], int max);

namespace fs = std::filesystem;

namespace {

using sos::GameBuild;
using sos::GameCheck;
using sos::GameFileState;
using sos::GamePaths;
using sos::GameRoots;

constexpr char kSettingsNote[] = "Settings are saved when you press Play.";

std::string Join(const std::vector<std::string>& values, const char* separator) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += separator;
        }
        out += value;
    }
    return out;
}

std::string GpuText() {
    char names[16][256]{};
    const int count = dcp_gpu_names(names, 16);
    if (count <= 0) {
        return "No Vulkan device detected";
    }
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i) {
        list.emplace_back(names[i]);
    }
    return Join(list, " / ");
}

class LauncherApp {
public:
    LauncherApp(GamePaths paths, GameCheck check, trg::TextFileSettings& settings, const char* log_file,
                const char* logs_dir, SosGraphicsCheck graphics_check)
        : paths_(std::move(paths)), check_(std::move(check)), settings_(settings), log_file_(log_file ? log_file : ""),
          logs_dir_(logs_dir ? logs_dir : ""), graphics_check_(graphics_check) {
        RefreshBuilds();
    }

    bool Run(trg::Result& result) {
        sos::HostOptions options;
        options.title = sos::kGameTitle;
        if (!host_.Open(options)) {
            return false;
        }
        settings_.true_value = "1";
        settings_.false_value = "0";
        fonts_ = trg::LoadSystemFonts(18.0f);
        displays_ = sos::ListDisplays();
        gpu_text_ = GpuText();
        gpu_cards_.push_back({"automatic", "Automatic (first usable card)"});
        char gpu_names[16][256]{};
        const int gpu_count = dcp_gpu_names(gpu_names, 16);
        for (int i = 0; i < gpu_count; ++i) {
            gpu_cards_.push_back({gpu_names[i], gpu_names[i]});
        }
        launcher_ = std::make_unique<trg::Launcher>(Config());
        host_.BeginFrame();
        result = Frame();
        host_.EndFrame(trg::Theme::Midnight().bg);
        host_.Show();
        while (result == trg::Result::kNone) {
            if (!host_.PumpEvents()) {
                result = trg::Result::kQuit;
                break;
            }
            if (host_.Minimized()) {
                SDL_Delay(30);
                continue;
            }
            host_.BeginFrame();
            result = Frame();
            host_.EndFrame(trg::Theme::Midnight().bg);
        }
        host_.Close();
        return true;
    }

private:
    trg::LauncherConfig Config() {
        trg::LauncherConfig config;
        config.branding.title = sos::kGameTitle;
        config.branding.subtitle = "PC PORT   \xC2\xB7   LAUNCHER";
        config.branding.subtitle_segments = {{"github.com/saulob", sos::kPortRepository}};
        config.theme = trg::Theme::Ember();
        config.fonts = fonts_;
        config.settings = &settings_;
        config.start_page = 0;
        config.hints = "Enter  Play        Esc  Quit        Ctrl+S  Save";
        config.note = kSettingsNote;
        config.can_play = [this] { return CanPlay(); };
        config.on_save = [this] {
            settings_.Save();
            return true;
        };
        config.pages = Pages();
        return config;
    }

    trg::PlayCheck CanPlay() {
        trg::PlayCheck check;
        check.ok = check_.state == GameFileState::kValid;
        if (!check.ok) {
            check.reason = "Install the game first: the Play page explains what is missing.";
            check.page = 0;
            return check;
        }
        if (graphics_check_) {
            char cause[200] = "", advice[200] = "";
            if (!graphics_check_(cause, sizeof(cause), advice, sizeof(advice))) {
                check.ok = false;
                check.reason = std::string(cause) + (advice[0] ? std::string(" ") + advice : "");
            }
        }
        return check;
    }

    std::vector<trg::Page> Pages() {
        std::vector<trg::Page> pages;
        pages.push_back({"Play", "Install the game and start playing.", [this](trg::Ui& ui) { PlayPage(ui); },
                         [this] { return check_.state != GameFileState::kValid; }});
        pages.push_back({"Display", "How the game window and its display behave.", [this](trg::Ui& ui) { DisplayPage(ui); }});
        pages.push_back({"Graphics", "The renderer and the graphics card it uses.", [this](trg::Ui& ui) { GraphicsPage(ui); }});
        pages.push_back({"Gameplay", "Frame pacing, overlays and logging.", [this](trg::Ui& ui) { GameplayPage(ui); }});
        pages.push_back({"Controls", "The Xbox controller the game uses.", [this](trg::Ui& ui) { ControlsPage(ui); }});
        pages.push_back({"Bug Report", "Logs and diagnostics.", [this](trg::Ui& ui) { BugReportPage(ui); }});
        pages.push_back({"About", "God of War: Sons of Sparta PC.", [this](trg::Ui& ui) { AboutPage(ui); }});
        return pages;
    }

    void PlayPage(trg::Ui& ui) {
        PollInstall();
        if (check_.state == GameFileState::kValid && check_.build) {
            const GameBuild& build = *check_.build;
            ui.StatusCard(trg::Status::kReady, "Ready to play", build.label);
            ui.Info("Game", build.label);
            ui.Info("Region", build.region);
            ui.Info("Content ID", build.content_id);
            ui.Info("Files", sos::BuildMetadataText(build).c_str());
        } else {
            ui.StatusCard(trg::Status::kAttention, "Game files needed",
                          check_.detail.empty() ? "Install CUSA57767 1.00, then optionally the 1.01 update."
                                                : check_.detail.c_str());
            ui.Info("Supported", sos::SupportedBuildsText().c_str());
        }

        if (install_task_.running()) {
            ui.Spacer();
            ui.TaskProgress(install_task_);
            ui.Help("Installing copies the game next to this program. The source package is not changed.");
            return;
        }

        ui.Spacer();
        ui.Row("Base game", "The CUSA57767 1.00 package or an extracted game folder.");
        ui.RowControl();
        if (ImGui::Button("Install game...")) {
            BrowseAndInstall(sos::PackageKind::kAny, "Install game");
        }
        ImGui::SameLine();
        if (ImGui::Button("Use a folder...")) {
            BrowseFolderAndInstall(sos::PackageKind::kAny);
        }

        if (IsUpdateInstalled() || check_.build) {
            ui.Row("Update", "The CUSA57767 1.01 update package. Base 1.00 must be installed first.");
            ui.RowControl();
            if (ImGui::Button("Install update...")) {
                BrowseAndInstall(sos::PackageKind::kUpdate, "Install update");
            }
        }

        if (!result_text_.empty()) {
            ui.Spacer();
            ui.Paragraph(result_text_.c_str());
        }
        if (!check_.detail.empty() && check_.state != GameFileState::kValid) {
            ui.Help(check_.detail.c_str());
        }
    }

    void BrowseAndInstall(sos::PackageKind kind, const char* title) {
        const std::string file = trg::BrowseForFile(title, {{"PS4 package", "*.pkg"}});
        if (file.empty()) {
            return;
        }
        StartInstall(fs::path(sos::PathFromUtf8(file)), kind);
    }

    void BrowseFolderAndInstall(sos::PackageKind kind) {
        const std::string page = trg::BrowseForFolder("Select the game folder (it contains eboot.bin and sce_module)");
        if (page.empty()) {
            return;
        }
        StartInstall(fs::path(sos::PathFromUtf8(page)), kind);
    }

    void StartInstall(const fs::path& source, sos::PackageKind kind) {
        const GameRoots roots = sos::RootsOf(paths_);
        result_text_.clear();
        install_task_.Start(kind == sos::PackageKind::kUpdate ? "Installing the update" : "Installing the game",
                            [this, source, roots, kind](trg::Task& task) {
            sos::InstallHooks hooks;
            hooks.expected = kind;
            hooks.phase = [&task](const char* phase) { task.SetLabel(phase); };
            hooks.progress = [&task](uint64_t done, uint64_t total) {
                task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                return !task.cancelled();
            };
            install_result_ = sos::InstallGame(source, roots, hooks);
            if (!install_result_.ok()) {
                const sos::InstallMessage message = sos::DescribeInstall(install_result_);
                return message.title + ": " + message.text;
            }
            return std::string();
        });
    }

    void PollInstall() {
        std::string error;
        const trg::Task::State state = install_task_.Finish(&error);
        if (state == trg::Task::State::kIdle || state == trg::Task::State::kRunning) {
            return;
        }
        if (state == trg::Task::State::kDone) {
            const sos::InstallMessage message = sos::DescribeInstall(install_result_);
            result_text_ = message.title + ": " + message.text;
            RefreshGame();
        } else if (state == trg::Task::State::kCancelled) {
            result_text_ = "Installation cancelled. The installed game was not changed.";
        } else {
            result_text_ = error.empty() ? "Installation failed." : error;
        }
    }

    void DisplayPage(trg::Ui& ui) {
        const int monitor = std::clamp(settings_.GetInt(sos::kMonitorKey, 1), 1,
                                       std::max(1, static_cast<int>(displays_.size())));
        std::vector<trg::Option> monitors;
        for (int i = 1; i <= static_cast<int>(displays_.size()); ++i) {
            monitors.push_back({std::to_string(i), sos::MonitorLabel(i, displays_)});
        }
        if (monitors.empty()) {
            monitors.push_back({"1", "Display 1"});
        }
        ui.Combo("Monitor", "The display the game opens on.", sos::kMonitorKey, "1", monitors);

        const std::vector<sos::WindowSize> sizes = sos::WindowSizes();
        std::vector<trg::Option> windows;
        for (const sos::WindowSize& size : sizes) {
            windows.push_back({std::to_string(size.width) + "x" + std::to_string(size.height),
                               sos::WindowSizeLabel(size)});
        }
        const int width = settings_.GetInt(sos::kWindowWidthKey, sos::kDefaultWindowWidth);
        const int height = settings_.GetInt(sos::kWindowHeightKey, sos::kDefaultWindowHeight);
        int current = 2;
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (sizes[i].width == width && sizes[i].height == height) {
                current = static_cast<int>(i);
            }
        }
        ui.Row("Window size", "The game window in pixels.");
        ui.RowControl();
        if (ImGui::BeginCombo("##window_size", windows[static_cast<size_t>(current)].label.c_str())) {
            for (size_t i = 0; i < windows.size(); ++i) {
                if (ImGui::Selectable(windows[i].label.c_str(), static_cast<int>(i) == current)) {
                    settings_.SetInt(sos::kWindowWidthKey, sizes[i].width);
                    settings_.SetInt(sos::kWindowHeightKey, sizes[i].height);
                }
            }
            ImGui::EndCombo();
        }

        ui.Choice("Window mode", "Windowed or desktop-sized borderless fullscreen.", sos::kDisplayModeKey, "windowed",
                  {{"windowed", "Windowed"}, {"borderless", "Borderless"}}, 2);
        ui.Toggle("VSync", "Waits for the display; off allows tearing.", sos::kVSyncKey, true);
        ui.Choice("Presentation", "How the rendered frame fills a window of another shape.", sos::kPresentationKey,
                  "letterbox", {{"letterbox", "Letterbox"}, {"stretch", "Stretch"}}, 2);
        ui.Toggle("Frame rate in the title bar", "Shows the presented frames per second.", sos::kShowFpsTitleKey, false);
        ui.Toggle("Resolution in the title bar", "Shows the render resolution.", sos::kShowResolutionTitleKey, false);
    }

    void GraphicsPage(trg::Ui& ui) {
        ui.Info("Renderer", "Vulkan");
        ui.Info("Detected graphics card", gpu_text_.c_str());
        ui.Combo("Graphics card", "Selects which card renders the game.", sos::kGraphicsCardKey, "automatic", gpu_cards_);
        ui.Paragraph("Vulkan is the only verified renderer for this build.");
    }

    void GameplayPage(trg::Ui& ui) {
        std::vector<trg::Option> rates;
        for (const int rate : {30, 60, 90, 120, 144, 165, 240}) {
            rates.push_back({std::to_string(rate), std::to_string(rate) + " Hz"});
        }
        rates.push_back({"0", "Unlimited"});
        ui.Combo("Frame rate", "Presentation pacing; the game's own 60 Hz tick is unchanged.", sos::kFrameRateKey, "60",
                 rates);
        ui.Toggle("Detailed log", "Keeps normal session logs in build\\logs.", sos::kDetailedLoggingKey, true);
        ui.Paragraph("No game-specific frame rate patch is applied.");
    }

    void ControlsPage(trg::Ui& ui) {
        int count = 0;
        SDL_JoystickID* pads = SDL_GetGamepads(&count);
        std::string name = "Not connected";
        if (pads) {
            for (int i = 0; i < count; ++i) {
                const char* pad = SDL_GetGamepadNameForID(pads[i]);
                if (pad) {
                    name = pad;
                    break;
                }
            }
            SDL_free(pads);
        }
        ui.Info("Controller", name.c_str());
        ui.Info("Layout", "Xbox controller (SDL3)");
        ui.Info("Keyboard", "Secondary; the game uses the controller layout");
        ui.Paragraph("Rebinding is not implemented yet. The game reads the standard PS4 pad layout.");
    }

    void BugReportPage(trg::Ui& ui) {
        ui.Info("Log", log_file_.empty() ? "(not started)" : log_file_.c_str());
        ui.Info("Logs folder", logs_dir_.c_str());
        ui.Info("Build", check_.build ? check_.build->label : "Not installed");
        ui.Row("Logs folder", "Opens the folder holding this run's log and crash reports.");
        ui.RowControl();
        if (ImGui::Button("Open logs folder")) {
            if (!logs_dir_.empty()) {
                trg::OpenInFileManager(logs_dir_);
            }
        }
        ui.Row("Diagnostics", "Copies a short summary for a bug report.");
        ui.RowControl();
        if (ImGui::Button("Copy summary")) {
            std::string text = std::string(sos::kGameTitle) + " " + (check_.build ? check_.build->label : "not installed") +
                               "\n" + sos::kPortRepository + "\n" + log_file_;
            ImGui::SetClipboardText(text.c_str());
        }
        if (!result_text_.empty()) {
            ui.Spacer();
            ui.Paragraph(result_text_.c_str());
        }
    }

    void AboutPage(trg::Ui& ui) {
        ui.Heading("God of War: Sons of Sparta PC");
        ui.Paragraph("Unofficial native PC port of the PS4 game.");
        ui.Info("Supported", sos::SupportedBuildsText().c_str());
        ui.Paragraph("This port bundles no Sony assets. The game files come from the player's own package.");
        ui.Paragraph("Not affiliated with Sony Interactive Entertainment or Santa Monica Studio.");
        ui.Link("Project", sos::kPortRepository);
        ui.Link("Issues", sos::kIssuesUrl);
    }

    trg::Result Frame() {
        return launcher_->Frame();
    }

    void RefreshBuilds() {
        const GameRoots roots = sos::RootsOf(paths_);
        update_installed_ = !sos::InstalledUpdatesOf(roots, "CUSA57767").empty();
    }

    void RefreshGame() {
        const GameRoots roots = sos::RootsOf(paths_);
        paths_ = sos::ActiveBuildPaths(roots);
        check_ = sos::CheckGameFiles(paths_);
        RefreshBuilds();
    }

    bool IsUpdateInstalled() const { return update_installed_; }

    GamePaths paths_;
    GameCheck check_;
    trg::TextFileSettings& settings_;
    std::string log_file_;
    std::string logs_dir_;
    std::vector<sos::Display> displays_;
    std::string gpu_text_;
    std::vector<trg::Option> gpu_cards_;
    trg::Fonts fonts_;
    sos::LauncherHost host_;
    std::unique_ptr<trg::Launcher> launcher_;
    trg::Task install_task_;
    sos::InstallResult install_result_;
    std::string result_text_;
    bool update_installed_ = false;
    SosGraphicsCheck graphics_check_ = nullptr;
};

} // namespace

extern "C" int sos_gpu_names(char (*names)[256], int max) { return dcp_gpu_names(names, max); }
extern "C" int sos_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size) {
    return dcp_check_vulkan(cause, cause_size, advice, advice_size);
}

extern "C" SosLauncherResult sos_launcher_run(const SosLauncherConfig* config) {
    const SosPathRequest request{nullptr, nullptr, config ? config->app0 : nullptr, config ? config->user_dir : nullptr,
                                 config ? config->logs_dir : nullptr};
    SosPaths resolved{};
    if (!sos_resolve_paths(&request, &resolved)) {
        std::fprintf(stderr, "Launcher: %s\n", resolved.error);
        return SOS_LAUNCHER_BLOCKED;
    }
    const GamePaths paths =
        config && config->app0 && config->image_dir
            ? GamePaths{sos::PathFromUtf8(config->app0), sos::PathFromUtf8(config->image_dir)}
            : GamePaths{sos::PathFromUtf8(resolved.app0), sos::PathFromUtf8(resolved.image_dir)};
    const GameCheck check = sos::CheckGameFiles(paths);
    const bool ready = check.state == GameFileState::kValid;
    const fs::path settings_file = config && config->settings_file ? sos::PathFromUtf8(config->settings_file)
                                                                 : sos::SettingsPath(sos::PathFromUtf8(resolved.user_dir));
    trg::TextFileSettings settings(sos::Utf8(settings_file));
    sos::ConfigureSettings(settings);
    const bool show = trg::ShouldShowLauncher(settings, sos::kShowLauncherKey, ready,
                                              config && config->force_show != 0) ||
                      (config && config->skip_once == 0 && !ready);
    if (!show) {
        return ready ? SOS_LAUNCHER_PLAY : SOS_LAUNCHER_BLOCKED;
    }
    LauncherApp app(paths, check, settings, config ? config->log_file : nullptr, resolved.logs_dir,
                    config && config->check_graphics ? config->check_graphics : sos_check_vulkan);
    trg::Result result = trg::Result::kNone;
    if (!app.Run(result)) {
        std::fprintf(stderr, "Launcher: could not open its window\n");
        return ready ? SOS_LAUNCHER_PLAY : SOS_LAUNCHER_BLOCKED;
    }
    return result == trg::Result::kPlay ? SOS_LAUNCHER_PLAY : SOS_LAUNCHER_QUIT;
}
