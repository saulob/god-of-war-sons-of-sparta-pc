// The launcher's view of user\sons_of_sparta.ini, the file the video core reads at start.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "trg/settings.h"

namespace sos {

using Values = std::map<std::string, std::string, std::less<>>;

inline constexpr char kShowLauncherKey[] = "show_launcher";
inline constexpr char kDisplayModeKey[] = "display_mode";
inline constexpr char kWindowWidthKey[] = "window_width";
inline constexpr char kWindowHeightKey[] = "window_height";
inline constexpr char kMonitorKey[] = "monitor";
inline constexpr char kVSyncKey[] = "vsync";
inline constexpr char kPresentationKey[] = "presentation";
inline constexpr char kGraphicsCardKey[] = "graphics_card";
inline constexpr char kRendererKey[] = "renderer";
inline constexpr char kFrameRateKey[] = "frame_rate";
inline constexpr char kShowFpsTitleKey[] = "show_fps_title";
inline constexpr char kShowResolutionTitleKey[] = "show_resolution_title";
inline constexpr char kDetailedLoggingKey[] = "detailed_logging";
inline constexpr char kLauncherXKey[] = "launcher_x";
inline constexpr char kLauncherYKey[] = "launcher_y";
inline constexpr char kLauncherWidthKey[] = "launcher_width";
inline constexpr char kLauncherHeightKey[] = "launcher_height";
inline constexpr char kLauncherMaximizedKey[] = "launcher_maximized";
inline constexpr char kLauncherFullscreenKey[] = "launcher_fullscreen";

inline constexpr int kDefaultWindowWidth = 1920;
inline constexpr int kDefaultWindowHeight = 1080;

// The settings file the video core reads: <user_dir>/sons_of_sparta.ini.
std::filesystem::path SettingsPath(const std::filesystem::path& user_dir);

// Settings are written as 1/0, which the video core's parser accepts.
void ConfigureSettings(trg::TextFileSettings& settings);

} // namespace sos
