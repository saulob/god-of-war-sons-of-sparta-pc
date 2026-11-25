#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <vector>

#include "game_files.h"
#include "game_process.h"
#include "sos_keys.h"
#include "sos_launcher.h"
#include "sos_resolution.h"
#include "utf8.h"

#ifndef SOS_PORT_VERSION
#define SOS_PORT_VERSION "unknown"
#endif

namespace sos {
namespace {

constexpr ManagedSetting kBaseSettings[] = {
    {kShowLauncherKey, "1"},
    {kControllerPromptsKey, kDefaultControllerPrompts},
    {kDisplayModeKey, "windowed"},
    {kWindowWidthKey, "1920"},
    {kWindowHeightKey, "1080"},
    {kMonitorKey, "1"},
    {kVSyncKey, "1"},
    {kPresentationKey, kDefaultPresentation},
    {kUpscaleKey, "0"},
    {kSharpenKey, "1"},
    {kSharpenStrengthKey, "75"},
    {kRenderResolutionKey, kNativeResolution},
    {kAspectRatioKey, kDefaultAspectRatio},
    {kGraphicsCardKey, ""},
    {kAntiAliasingKey, "0"},
    {kAnisotropyKey, "0"},
    {kFrameRateKey, "60"},
    {kShowFpsTitleKey, "0"},
    {kShowResolutionTitleKey, "0"},
    {kShowFrameCounterKey, "0"},
    {kSkipIntroKey, "0"},
    {kUnlockHermesModeKey, "0"},
    {kUnlockThePitKey, "0"},
    {kDetailedLoggingKey, "1"},
    {kAllowOverlayProgramsKey, "0"},
    {kLauncherXKey, ""},
    {kLauncherYKey, ""},
    {kLauncherWidthKey, ""},
    {kLauncherHeightKey, ""},
    {kLauncherMaximizedKey, "0"},
    {kLauncherFullscreenKey, "0"},
};

constexpr std::string_view kPlacementKeys[] = {kLauncherXKey,         kLauncherYKey,         kLauncherWidthKey,
                                                kLauncherHeightKey,    kLauncherMaximizedKey, kLauncherFullscreenKey};

constexpr char kFileHeader[] =
    "# God of War: Sons of Sparta user settings (written by the launcher; F11 saves the display mode).\n"
    "# Remove a line to use the default.\n";

} // namespace

std::span<const ManagedSetting> ManagedSettings() {
    static const std::vector<ManagedSetting> settings = [] {
        static std::deque<std::string> default_names;
        std::vector<ManagedSetting> all(std::begin(kBaseSettings), std::end(kBaseSettings));
        for (const SosKeyInfo& action : sos_key_actions) {
            char name[32];
            default_names.emplace_back(sos_key_name(action.default_scancode, name, sizeof(name)));
            all.push_back({action.ini_key, default_names.back()});
        }
        return all;
    }();
    return settings;
}

bool IsValidSetting(std::string_view key, std::string_view value) {
    const auto one_of = [&](std::initializer_list<std::string_view> values) {
        return std::find(values.begin(), values.end(), value) != values.end();
    };
    if (std::any_of(std::begin(sos_key_actions), std::end(sos_key_actions),
                    [&](const SosKeyInfo& action) { return key == action.ini_key; })) {
        return sos_key_from_name(std::string(value).c_str()) != 0;
    }
    if (key == kControllerPromptsKey) {
        return sos_prompt_style_parse(std::string(value).c_str(), nullptr) != 0;
    }
    if (key == kDisplayModeKey) {
        return one_of({"windowed", "borderless"});
    }
    if (key == kRenderResolutionKey) {
        return std::any_of(std::begin(sos_resolutions), std::end(sos_resolutions),
                           [&](const SosResolution& resolution) { return value == resolution.key; });
    }
    if (key == kAspectRatioKey) {
        return std::any_of(std::begin(sos_aspect_ratios), std::end(sos_aspect_ratios),
                           [&](const SosAspectRatio& aspect) { return value == aspect.key; });
    }
    if (key == kAnisotropyKey) {
        return one_of({"0", "2", "4", "8", "16"});
    }
    if (key == kFrameRateKey) {
        return one_of({"30", "60", "90", "120", "144", "165", "240", "0"});
    }
    if (key == kPresentationKey) {
        return std::any_of(std::begin(kPresentationChoices), std::end(kPresentationChoices),
                           [&](const PresentationChoice& choice) { return value == choice.key; });
    }
    return true;
}

bool IsLauncherPlacementSetting(std::string_view key) {
    for (const std::string_view placement_key : kPlacementKeys) {
        if (placement_key == key) {
            return true;
        }
    }
    return false;
}

LauncherSettings::LauncherSettings(std::filesystem::path file) : file_(std::move(file)) {
    true_value = "1";
    false_value = "0";
    const trg::TextFileSettings stored(Utf8(file_));
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (stored.Has(setting.key)) {
            saved_[std::string(setting.key)] = stored.Get(setting.key);
        }
    }
    pending_ = saved_;
}

std::string LauncherSettings::Get(std::string_view key, std::string_view def) const {
    const auto it = pending_.find(key);
    if (it == pending_.end() || it->second.empty() || !IsValidSetting(key, it->second)) {
        return std::string(def);
    }
    return it->second;
}

void LauncherSettings::Set(std::string_view key, std::string value) {
    pending_[std::string(key)] = std::move(value);
}

bool LauncherSettings::Save() {
    Values changes;
    for (const auto& [key, value] : pending_) {
        const auto saved = saved_.find(key);
        if (saved == saved_.end() || saved->second != value) {
            changes.emplace(key, value);
        }
    }
    if (changes.empty()) {
        return true;
    }
    if (!WriteFile(changes)) {
        return false;
    }
    for (const auto& [key, value] : changes) {
        saved_[key] = value;
    }
    return true;
}

bool LauncherSettings::SaveNow(const Values& values) {
    if (!WriteFile(values)) {
        return false;
    }
    for (const auto& [key, value] : values) {
        saved_[key] = value;
        pending_[key] = value;
    }
    return true;
}

void LauncherSettings::ResetToDefaults() {
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (!IsLauncherPlacementSetting(setting.key)) {
            pending_[std::string(setting.key)] = std::string(setting.default_value);
        }
    }
}

bool LauncherSettings::WriteFile(const Values& changes) {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (!std::filesystem::exists(file_, ec)) {
        std::ofstream(file_) << kFileHeader;
    }
    trg::TextFileSettings file(Utf8(file_));
    for (const auto& [key, value] : changes) {
        file.Set(key, value);
    }
    return file.Save();
}

std::string FrameRateLabel(int fps) {
    if (fps == 0) {
        return "Unlimited";
    }
    if (fps == 60) {
        return "60 FPS (Original)";
    }
    return std::to_string(fps) + " FPS";
}

bool LauncherWanted(bool game_ready, bool show_setting, bool shift_held, bool skip_once) {
    if (!game_ready) {
        return true;
    }
    return !skip_once && (shift_held || show_setting);
}

std::filesystem::path SettingsPath(const std::filesystem::path& user_dir) {
    return user_dir / "sons_of_sparta.ini";
}

namespace {

const char* OnOff(bool on) { return on ? "on" : "off"; }

#ifdef _WIN32
constexpr char kPlatform[] = "Windows x64";
#else
constexpr char kPlatform[] = "Linux x86-64";
#endif

} // namespace

std::string SettingsSummary(const LauncherSettings& settings, const GameBuild* build) {
    std::string out;
    const auto line = [&out](const std::string& text) { out += "Settings: " + text + "\n"; };
    line(std::string("God of War: Sons of Sparta PC v" SOS_PORT_VERSION ", ") + kPlatform + ", game build " +
         (build ? build->label : "not a supported build"));

    const std::string mode = settings.Get(kDisplayModeKey, "windowed");
    line(std::string("Window ") + (mode == "borderless" ? "Borderless Fullscreen" : "Windowed") + ", frame rate " +
         FrameRateLabel(std::atoi(settings.Get(kFrameRateKey, "60").c_str())) + ", frame counter " +
         OnOff(settings.GetBool(kShowFrameCounterKey, false)) + ", frame rate in title " +
         OnOff(settings.GetBool(kShowFpsTitleKey, false)) + ", render resolution in title " +
         OnOff(settings.GetBool(kShowResolutionTitleKey, false)));

    int prompts = SOS_PROMPTS_AUTOMATIC;
    sos_prompt_style_parse(settings.Get(kControllerPromptsKey, kDefaultControllerPrompts).c_str(), &prompts);
    line(std::string("Controller prompts ") + sos_prompt_style_label(prompts));
    line(std::string("Skip Intro ") + OnOff(settings.GetBool(kSkipIntroKey, false)));
    line(std::string("Unlock Hermes Mode ") + OnOff(settings.GetBool(kUnlockHermesModeKey, false)));
    line(std::string("Unlock The Pit Early ") + OnOff(settings.GetBool(kUnlockThePitKey, false)));
    line(std::string("Overlay programs ") +
         (kSeparateGameProcessAvailable ? OnOff(settings.GetBool(kAllowOverlayProgramsKey, false)) : "unsupported"));
    return out;
}

} // namespace sos

extern "C" int sos_detailed_logging(const char* settings_file) {
    using namespace sos;
    if (!settings_file || !*settings_file) {
        return 1;
    }
    const LauncherSettings settings(PathFromUtf8(settings_file));
    return settings.GetBool(kDetailedLoggingKey, true) ? 1 : 0;
}

extern "C" void sos_log_settings_summary(const char* settings_file, const char* image_dir) {
    using namespace sos;
    const LauncherSettings settings(PathFromUtf8(settings_file ? settings_file : ""));
    std::fputs(SettingsSummary(settings, BuildOfImageDir(image_dir)).c_str(), stdout);
}
