// Focused, headless checks of the launcher: page order, icons at the DPI sizes, header art placement, settings, game
// process arguments, the game identity shown and what the launcher must not contain. No window is opened.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "game_files.h"
#include "game_process.h"
#include "game_profile.h"
#include "launcher_theme.h"
#include "header_art.h"
#include "options.h"
#include "page_icons.h"
#include "settings.h"
#include "sos_keys.h"
#include "sos_launcher.h"
#include "trg/layout.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace sos;

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

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

const fs::path kRoot = SOS_SOURCE_DIR;

std::vector<fs::path> SourceFiles(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) {
            continue;
        }
        const std::string extension = it->path().extension().string();
        if (extension == ".cpp" || extension == ".h" || extension == ".c" || extension == ".md" || extension == ".txt" ||
            extension == ".cmake" || extension == ".ps1" || extension == ".svg") {
            files.push_back(it->path());
        }
    }
    return files;
}

std::string Relative(const fs::path& path) { return path.lexically_relative(kRoot).generic_string(); }

void TestPages() {
    const std::vector<std::string> expected = {"Play",     "Display",  "Graphics",   "Gameplay",
                                               "Controls", "Trophies", "Bug Report", "About"};
    std::vector<std::string> actual;
    for (const PageId page : kPageOrder) {
        actual.emplace_back(PageName(page));
    }
    Check(actual == expected, "page order is Play, Display, Graphics, Gameplay, Controls, Trophies, Bug Report, About");
    Check(std::count(actual.begin(), actual.end(), "Online") == 0, "no Online page");
    Check(std::count(actual.begin(), actual.end(), "Cheats") == 0, "Cheats page absent");
    Check(std::count(actual.begin(), actual.end(), "Bug Report") == 1, "Bug Report page present");
    Check(std::count(actual.begin(), actual.end(), "Trophies") == 1, "Trophies page present");
}

int IconPixels(float scale) { return static_cast<int>(21.0f * scale + 0.5f); }

void TestIcons() {
    const fs::path icons = kRoot / "assets" / "launcher" / "icons";
    std::set<std::string> referenced;
    for (const PageId page : kPageOrder) {
        const PageIconAsset* asset = FindPageIcon(page);
        Check(asset != nullptr, std::string("page icon asset exists for ") + PageName(page));
        if (!asset) {
            continue;
        }
        referenced.insert(asset->file);
        Check(fs::is_regular_file(icons / asset->file), std::string("icon file present: ") + asset->file);
        Check(asset->optical_scale > 0.5f && asset->optical_scale <= 1.0f, std::string("optical scale sane: ") + asset->file);
        for (const float scale : {1.0f, 1.5f, 2.25f}) {
            const int pixels = IconPixels(scale);
            const std::vector<uint8_t> rgba = RenderPageIcon(page, pixels);
            const std::string label = std::string(asset->file) + " at " + std::to_string(pixels) + " px";
            Check(rgba.size() == size_t(pixels) * size_t(pixels) * 4, "embedded icon renders: " + label);
            if (rgba.size() != size_t(pixels) * size_t(pixels) * 4) {
                continue;
            }
            int min_x = pixels, min_y = pixels, max_x = -1, max_y = -1;
            bool white = true;
            for (int y = 0; y < pixels; ++y) {
                for (int x = 0; x < pixels; ++x) {
                    const uint8_t* texel = &rgba[(size_t(y) * size_t(pixels) + size_t(x)) * 4];
                    white = white && texel[0] == 255 && texel[1] == 255 && texel[2] == 255;
                    if (texel[3] > 32) {
                        min_x = std::min(min_x, x);
                        max_x = std::max(max_x, x);
                        min_y = std::min(min_y, y);
                        max_y = std::max(max_y, y);
                    }
                }
            }
            Check(white, "icon is white with the artwork as alpha: " + label);
            Check(max_x >= min_x && max_y >= min_y, "icon has visible artwork: " + label);
            const int longest = std::max(max_x - min_x, max_y - min_y) + 1;
            Check(longest <= pixels && longest >= int(pixels * asset->optical_scale) - 3,
                  "icon fills its optical box: " + label);
            Check(std::abs((min_x + max_x + 1) - pixels) <= 3 && std::abs((min_y + max_y + 1) - pixels) <= 3,
                  "icon is centred: " + label);
        }
    }
    Check(!RasterizeIcon(nullptr, 0, 0.9f, 21).size(), "no icon from nothing");
    std::set<std::string> on_disk;
    for (const fs::directory_entry& entry : fs::directory_iterator(icons)) {
        if (entry.path().extension() == ".svg") {
            on_disk.insert(entry.path().filename().string());
        }
    }
    Check(on_disk == referenced, "icons folder holds exactly the page icons");
    Check(on_disk.count("online.svg") == 0, "no online.svg");
    Check(fs::is_regular_file(icons / "SOURCES.md"), "icons SOURCES.md kept");
    const std::string sources = ReadFile(icons / "SOURCES.md");
    for (const std::string& file : on_disk) {
        Check(Contains(sources, "`" + file + "`"), "SOURCES.md records " + file);
    }
    Check(Contains(sources, "CC Attribution License") && Contains(sources, "MIT License"),
          "SOURCES.md keeps the display.svg and bug-report.svg licenses");
    const std::string licenses = ReadFile(kRoot / "packaging" / "licenses" / "THIRD-PARTY-LICENSES.txt");
    for (const char* needle : {"nickylimyeanfen", "Vectopus", "NanoSVG", "miniz", "TRG Launcher", "Heart icon"}) {
        Check(Contains(Lower(licenses), Lower(needle)), std::string("license notice present: ") + needle);
    }
}

void TestSponsorAsset() {
    const fs::path png = kRoot / "assets" / "launcher" / "sponsor-heart.png";
    const std::string data = ReadFile(png);
    Check(data.size() > 33 && data.compare(1, 3, "PNG") == 0, "sponsor-heart.png is a PNG");
    if (data.size() > 24) {
        const auto be32 = [&](size_t at) {
            return (unsigned(uint8_t(data[at])) << 24) | (unsigned(uint8_t(data[at + 1])) << 16) |
                   (unsigned(uint8_t(data[at + 2])) << 8) | unsigned(uint8_t(data[at + 3]));
        };
        Check(be32(16) == 64 && be32(20) == 64, "sponsor-heart.png is 64 x 64");
    }
    Check(fs::is_regular_file(kRoot / "assets" / "launcher" / "sponsor-heart.svg"), "sponsor-heart.svg is the kept source");
    Check(!fs::exists(kRoot / "assets" / "launcher" / "header-art.png"), "no header art shipped without approved Sons art");
    Check(Contains(ReadFile(kRoot / "launcher" / "game_profile.h"), "https://github.com/sponsors/saulob"),
          "sponsor URL configured");
}

bool Teal(int rgb) {
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    return b > r + 20 && g > r + 20;
}

void TestPalette() {
    using namespace sos::palette;
    const int primary[] = {kAccent, kAccentHot, kActionPressed, kActionEdge, kActionLink, kSponsor, kSponsorHot,
                           kSponsorActive, kSponsorLink, kOnAction, kBackground, kPanel, kFrame, kFrameHot, kFrameActive,
                           kPopup, kRowBorder, kPanelBorder, kText, kDim, kHeaderEdge, kHeaderGlow, kHeaderMid, kHeaderBottom};
    for (const int colour : primary) {
        Check(!Teal(colour), "no teal in the launcher palette");
        Check(((colour >> 16) & 255) >= (colour & 255), "palette colours are warm");
    }
    const auto channel = [](int colour, int shift) { return (colour >> shift) & 255; };
    Check(channel(kAccent, 16) > 200 && channel(kAccent, 8) > 150 && channel(kAccent, 0) < 120, "accent is gold");
    Check(kAccent == ((kAccentRed << 16) | (kAccentGreen << 8) | kAccentBlue), "accent comes from sos_accent.h");
    Check(channel(kActionEdge, 16) > 2 * channel(kActionEdge, 8) && channel(kActionEdge, 16) > 120, "action edge is red");
    Check(channel(kAccentHot, 16) > channel(kAccent, 16) && channel(kAccentHot, 8) > channel(kAccent, 8), "hover is a brighter gold");
    Check(channel(kActionPressed, 16) < channel(kAccent, 16) && channel(kActionPressed, 8) < channel(kAccent, 8), "pressed is darker");
    Check(channel(kBackground, 16) < 40 && channel(kBackground, 0) < 20, "background is very dark brown");
    Check(channel(kSponsor, 16) > 2 * channel(kSponsor, 8), "sponsor button is deep red");
    Check(channel(kOnAction, 16) < 80, "text on the gold action is dark brown");
}

unsigned Be32(const std::string& data, size_t at) {
    return (unsigned(uint8_t(data[at])) << 24) | (unsigned(uint8_t(data[at + 1])) << 16) |
           (unsigned(uint8_t(data[at + 2])) << 8) | unsigned(uint8_t(data[at + 3]));
}

unsigned Le(const std::string& data, size_t at, int bytes) {
    unsigned value = 0;
    for (int i = bytes - 1; i >= 0; --i) {
        value = (value << 8) | uint8_t(data[at + size_t(i)]);
    }
    return value;
}

void TestActionEdge() {
    using namespace sos::palette;
    Check(kActionEdgeWidth == 2.0f, "action edge is two logical pixels");
    Check(trg::layout::ActionEdgeThickness(kActionEdgeWidth, 1.0f) == 2.0f, "action edge is 2 px at 100%");
    Check(trg::layout::ActionEdgeThickness(kActionEdgeWidth, 1.5f) == 3.0f, "action edge is 3 px at 150%");
    const float large = trg::layout::ActionEdgeThickness(kActionEdgeWidth, 2.25f);
    Check(large >= 4.0f && large <= 5.0f, "action edge is 4 to 5 px at 225%");
    Check(trg::layout::ActionEdgeThickness(0.0f, 1.0f) == 1.0f, "action edge is never thinner than a pixel");
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        Check(trg::layout::ActionEdgeThickness(kActionEdgeWidth, scale) > trg::layout::LineThickness(scale),
              "action edge is thicker than an ordinary line");
        Check(trg::layout::ActionEdgeThickness(kActionEdgeWidth, scale) ==
                  std::floor(trg::layout::ActionEdgeThickness(kActionEdgeWidth, scale)),
              "action edge is whole pixels");
    }
    const std::string source = ReadFile(kRoot / "launcher" / "launcher.cpp");
    Check(Contains(source, "theme.action_edge_width = palette::kActionEdgeWidth;"), "theme sets the edge width centrally");
    const std::string patch = ReadFile(kRoot / "gpu" / "patches" / "trg-launcher" / "0020-action-edge-width.patch");
    for (const char* needle : {"+  float action_edge_width", "ActionEdgeThickness", "DrawActionEdge(dl, pos", "DrawActionEdge(dl, p,",
                               "ImGuiStyleVar_FrameBorderSize, layout::ActionEdgeThickness", "PushAccent(theme(), scale_)"}) {
        Check(Contains(patch, needle), std::string("TRG patch has ") + needle);
    }
    Check(Contains(ReadFile(kRoot / "launcher" / "launcher_theme.h"), "kActionEdge = 0xA82C26;"), "action edge keeps the dark red");
}

void TestVersion() {
    const std::string cmake = ReadFile(kRoot / "CMakeLists.txt");
    const size_t at = cmake.find("project(sons_of_sparta_pc VERSION ");
    Check(at != std::string::npos, "project version is declared");
    const std::string declared = at == std::string::npos ? "" : cmake.substr(at + 34, 5);
    Check(declared == "0.1.0", "project version is 0.1.0");
    Check(std::string(SOS_PORT_VERSION) == declared, "launcher version comes from the project version");
    Check(Contains(cmake, "SOS_VERSION_STRING"), "Windows resource version is generated from the project version");
    Check(Contains(ReadFile(kRoot / "launcher" / "CMakeLists.txt"), "SOS_PORT_VERSION=\"${PROJECT_VERSION}\""),
          "launcher and updater definition comes from the project version");
    Check(Contains(ReadFile(kRoot / "launcher" / "update_config.cpp"), "SOS_PORT_VERSION"), "updater reports the port version");
    const std::string literal = "0.2.0";
    for (const char* folder : {"launcher", "src", "assets", "packaging", "tests", "tools"}) {
        for (const fs::path& path : SourceFiles(kRoot / folder)) {
            const std::string name = Relative(path);
            if (name.find("third_party/") != std::string::npos || name == "tests/launcher_tests.cpp") {
                continue;
            }
            Check(!Contains(ReadFile(path), literal), name + " has no 0.2.0");
        }
    }
    Check(!Contains(cmake, literal), "CMakeLists.txt has no 0.2.0");
}

void TestApplicationIcon() {
    const fs::path icons = kRoot / "packaging" / "icons";
    const std::string ico = ReadFile(icons / "app-icon.ico");
    Check(ico.size() > 22 && Le(ico, 0, 2) == 0 && Le(ico, 2, 2) == 1, "app-icon.ico is an icon file");
    const unsigned frames = ico.size() > 6 ? Le(ico, 4, 2) : 0;
    Check(frames == 7, "app-icon.ico has 7 sizes");
    const unsigned expected[] = {16, 24, 32, 48, 64, 128, 256};
    for (unsigned i = 0; i < frames && i < 7 && ico.size() >= 6 + 16 * (i + 1); ++i) {
        const size_t entry = 6 + 16 * i;
        const unsigned width = uint8_t(ico[entry]) ? uint8_t(ico[entry]) : 256;
        Check(width == expected[i], "ico frame size " + std::to_string(expected[i]));
        Check(Le(ico, entry + 6, 2) == 32, "ico frame is 32-bit " + std::to_string(expected[i]));
        const size_t offset = Le(ico, entry + 12, 4), length = Le(ico, entry + 8, 4);
        Check(offset + length <= ico.size(), "ico frame inside the file " + std::to_string(expected[i]));
    }
    for (const unsigned size : {16u, 24u, 32u, 48u, 64u, 128u, 256u, 512u}) {
        const std::string png = ReadFile(icons / ("app-icon-" + std::to_string(size) + ".png"));
        Check(png.size() > 33 && png.compare(1, 3, "PNG") == 0 && Be32(png, 16) == size && Be32(png, 20) == size &&
                  uint8_t(png[24]) == 8 && uint8_t(png[25]) == 6,
              "app-icon-" + std::to_string(size) + ".png is 8-bit RGBA");
    }
    const std::string svg = Lower(ReadFile(icons / "app-icon.svg"));
    Check(Contains(svg, "#dea959") && Contains(svg, "#a82c26"), "icon source uses the launcher accent and action edge");
    Check(!Contains(svg, "#19b7a5") && !Contains(svg, "#f05a3c"), "icon source has no other project's colours");
    const std::string resource = ReadFile(kRoot / "src" / "sons_of_sparta.rc");
    for (const char* needle : {"1 ICON \"../packaging/icons/app-icon.ico\"", "VALUE \"CompanyName\", \"saulob\"",
                               "VALUE \"FileDescription\", \"God of War: Sons of Sparta PC\"",
                               "VALUE \"InternalName\", \"sons_of_sparta\"",
                               "VALUE \"OriginalFilename\", \"sons_of_sparta.exe\"",
                               "VALUE \"ProductName\", \"God of War: Sons of Sparta PC\"",
                               "VALUE \"Comments\", \"https://github.com/saulob/god-of-war-sons-of-sparta-pc\""}) {
        Check(Contains(resource, needle), std::string("resource has ") + needle);
    }
    const std::string cmake = ReadFile(kRoot / "CMakeLists.txt");
    Check(Contains(cmake, "src/sons_of_sparta.rc") && Contains(cmake, "enable_language(RC)") &&
              Contains(cmake, "packaging/icons/app-icon.ico"),
          "CMake embeds the icon resource");
    const std::string licenses = ReadFile(kRoot / "packaging" / "licenses" / "THIRD-PARTY-LICENSES.txt");
    Check(Contains(licenses, "Application icon: Game Console Ecommerce") && Contains(licenses, "svgrepo.com/svg/425126"),
          "application icon provenance and license");
}

void TestHeaderArt() {
    const HeaderArtRect rect = PlaceHeaderArt(0.0f, 1000.0f, 130.4f, kHeaderArtAspect);
    Check(rect.max_x == 1000.0f && rect.min_y == 0.0f && rect.max_y == 131.0f, "header art is flush top and right, whole pixels");
    Check(rect.max_x - rect.min_x == float(int(rect.max_x - rect.min_x)), "header art width is whole pixels");
    float covered = 0.0f;
    const std::vector<HeaderArtStrip> strips = SplitHeaderArt(rect);
    for (const HeaderArtStrip& strip : strips) {
        covered += strip.max_x - strip.min_x;
        Check(strip.max_x - strip.min_x <= float(kHeaderArtStripWidth), "header art strip within the renderer limit");
    }
    Check(covered == rect.max_x - rect.min_x, "header art strips cover the art");
    const std::vector<uint8_t> source(4 * 4 * 4, 255);
    Check(ScaleArtRgba(source.data(), 4, 4, 2, 2).size() == 2 * 2 * 4, "art scales to the shown size");
    Check(ScaleArtRgba(nullptr, 4, 4, 2, 2).empty(), "bad art input gives nothing");
}

fs::path TempFolder() {
    const char* parent = "E:\\ClaudeTemp\\opencode";
    fs::path base = fs::is_directory(parent) ? fs::path(parent) : fs::temp_directory_path();
    fs::path folder = base / ("sos_launcher_test_" + std::to_string(std::rand()) + std::to_string(std::rand()));
    fs::create_directories(folder);
    return folder;
}

void TestSettings() {
    const fs::path folder = TempFolder();
    const fs::path file = folder / "user" / "sons_of_sparta.ini";
    Check(SettingsPath(folder / "user") == file, "settings file is user\\sons_of_sparta.ini");
    {
        LauncherSettings settings(file);
        Check(settings.GetBool(kShowLauncherKey, true), "Show this launcher defaults to on");
        Check(settings.Get(kFrameRateKey, "60") == "60", "frame rate defaults to 60");
        Check(!settings.GetBool(kUnlockHermesModeKey, false), "Unlock Hermes Mode defaults to off");
        settings.Set(kDisplayModeKey, "borderless");
        settings.SetBool(kVSyncKey, false);
        settings.SetInt(kSharpenStrengthKey, 40);
        settings.SetBool(kShowLauncherKey, false);
        settings.Set(kLauncherXKey, "10");
        Check(settings.Save(), "settings save");
    }
    Check(sos_hermes_unlock_from_file(Utf8(file).c_str()) == 0, "Hermes Mode early unlock defaults off when the key is absent");
    const std::string text = ReadFile(file);
    Check(Contains(text, "display_mode = borderless") || Contains(text, "display_mode=borderless"), "display mode written");
    Check(Contains(text, "vsync = 0") || Contains(text, "vsync=0"), "booleans are written as 0/1");
    Check(Contains(text, "God of War: Sons of Sparta user settings"), "settings file header names the game");
    {
        std::ofstream(file, std::ios::app) << "custom_line = keep\n# a comment\n";
    }
    {
        LauncherSettings settings(file);
        Check(settings.Get(kDisplayModeKey, "windowed") == "borderless", "saved display mode is read back");
        Check(!settings.GetBool(kShowLauncherKey, true), "saved Show this launcher is read back");
        settings.Set(kFrameRateKey, "144");
        Check(settings.SaveNow({{kLauncherYKey, "20"}}), "placement saves at once");
        Check(settings.Get(kFrameRateKey, "60") == "144", "other edits stay pending after SaveNow");
        settings.ResetToDefaults();
        Check(settings.Get(kDisplayModeKey, "windowed") == "windowed", "reset puts display mode back");
        Check(settings.Get(kFrameRateKey, "60") == "60", "reset puts frame rate back");
        Check(settings.Get(kUnlockHermesModeKey, "") == "0", "reset puts Hermes Mode early unlock back off");
        Check(settings.Get(kLauncherXKey) == "10" && settings.Get(kLauncherYKey) == "20", "reset keeps the launcher placement");
        Check(settings.Save(), "reset settings save");
    }
    const std::string after = ReadFile(file);
    Check(Contains(after, "custom_line = keep") || Contains(after, "custom_line=keep"), "unmanaged lines are kept");
    Check(Contains(after, "# a comment"), "comments are kept");
    {
        std::ofstream(file, std::ios::app) << "frame_rate = 61\nanisotropy = 3\ndisplay_mode = sideways\n";
    }
    {
        const LauncherSettings settings(file);
        Check(settings.Get(kFrameRateKey, "60") == "60", "invalid frame rate falls back");
        Check(settings.Get(kAnisotropyKey, "0") == "0", "invalid texture filtering falls back");
        Check(settings.Get(kDisplayModeKey, "windowed") == "windowed", "invalid display mode falls back");
    }
    for (const char* rate : {"30", "60", "90", "120", "144", "165", "240", "0"}) {
        Check(IsValidSetting(kFrameRateKey, rate), std::string("frame rate valid: ") + rate);
    }
    for (const char* filtering : {"0", "2", "4", "8", "16"}) {
        Check(IsValidSetting(kAnisotropyKey, filtering), std::string("texture filtering valid: ") + filtering);
    }
    Check(!IsValidSetting(kFrameRateKey, "75"), "frame rate 75 is not offered");

    std::set<std::string> managed;
    std::string hermes_default;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed.insert(std::string(setting.key));
        if (setting.key == kUnlockHermesModeKey) hermes_default = setting.default_value;
    }
    Check(hermes_default == "0", "the managed Hermes Mode default is off");
    for (const char* key : {kShowLauncherKey, kDisplayModeKey, kWindowWidthKey, kWindowHeightKey, kMonitorKey, kVSyncKey,
                            kPresentationKey, kUpscaleKey, kSharpenKey, kSharpenStrengthKey, kGraphicsCardKey,
                            kAntiAliasingKey, kAnisotropyKey, kFrameRateKey, kShowFpsTitleKey, kShowResolutionTitleKey,
                            kShowFrameCounterKey, kSkipIntroKey, kUnlockHermesModeKey, kDetailedLoggingKey,
                            kAllowOverlayProgramsKey, kLauncherXKey,
                            kLauncherYKey, kLauncherWidthKey, kLauncherHeightKey, kLauncherMaximizedKey,
                            kLauncherFullscreenKey}) {
        Check(managed.count(key) == 1, std::string("managed setting: ") + key);
    }
    for (const SosKeyInfo& action : sos_key_actions) {
        Check(managed.count(action.ini_key) == 1, std::string("managed key binding: ") + action.ini_key);
    }
    Check(managed.count("asian_menu_buttons") == 0 && managed.count("local_coop") == 0, "no Asian Menu Buttons or co-op setting");
    for (const std::string& key : managed) {
        Check(key.find("online") == std::string::npos && key.find("lobby") == std::string::npos, "no online setting: " + key);
    }

    {
        LauncherSettings settings(file);
        const SosKeyAction action = SOS_KEY_CROSS;
        SetKey(settings, action, SDL_SCANCODE_J);
        Check(SavedKey(settings, action) == SDL_SCANCODE_J, "a rebound key is saved");
        SetKey(settings, SOS_KEY_CIRCLE, SDL_SCANCODE_J);
        Check(SavedKey(settings, SOS_KEY_CIRCLE) == SDL_SCANCODE_J && SavedKey(settings, action) == SDL_SCANCODE_J,
              "a key can serve two controls");
        SetKey(settings, SOS_KEY_SQUARE, SDL_SCANCODE_ESCAPE);
        Check(SavedKey(settings, SOS_KEY_SQUARE) == sos_key_actions[SOS_KEY_SQUARE].default_scancode,
              "Escape cannot be bound");
        ResetKeys(settings);
        Check(SavedKey(settings, action) == sos_key_actions[action].default_scancode, "keys reset to their defaults");
        Check(settings.Get(kDisplayModeKey, "windowed") == "windowed", "key reset touches nothing else");
        Check(KeyDisplayName(SDL_SCANCODE_RETURN) == "Enter", "Enter is shown as Enter");
        Check(KeyDisplayName(SDL_SCANCODE_ESCAPE).empty(), "Escape has no key name");
        Check(sos_key_actions[SOS_KEY_HOTKEY_WINDOW_MODE].default_scancode == SDL_SCANCODE_F11, "window mode hotkey is F11");
    }

    Check(sos_detailed_logging(Utf8(file).c_str()) == 1, "detailed logging defaults on");
    {
        LauncherSettings settings(file);
        settings.SetBool(kDetailedLoggingKey, false);
        settings.Save();
    }
    Check(sos_detailed_logging(Utf8(file).c_str()) == 0, "detailed logging off is read by the loader");
    Check(sos_detailed_logging(nullptr) == 1, "detailed logging without a file is on");
    Check(sos_hermes_unlock_from_file(Utf8(file).c_str()) == 0, "saved Hermes Mode off is read by the loader");
    {
        LauncherSettings settings(file);
        settings.SetBool(kUnlockHermesModeKey, false);
        settings.Save();
    }
    Check(sos_hermes_unlock_from_file(Utf8(file).c_str()) == 0, "Hermes Mode early unlock off is read by the loader");
    {
        const LauncherSettings settings(file);
        Check(!settings.GetBool(kUnlockHermesModeKey, false), "saved Hermes Mode off is read back by the launcher");
    }
    {
        LauncherSettings settings(file);
        settings.SetBool(kUnlockHermesModeKey, true);
        settings.Save();
    }
    Check(sos_hermes_unlock_from_file(Utf8(file).c_str()) == 1, "Hermes Mode early unlock on is read by the loader");
    {
        const LauncherSettings settings(file);
        Check(settings.GetBool(kUnlockHermesModeKey, false), "saved Hermes Mode on is read back by the launcher");
    }
    Check(sos_hermes_unlock_from_file(nullptr) == 0, "Hermes Mode early unlock without a file is off");

    Check(LauncherWanted(false, false, false, true), "an unready game always shows the launcher");
    Check(LauncherWanted(true, true, false, false), "Show this launcher on shows it");
    Check(!LauncherWanted(true, false, false, false), "Show this launcher off starts the game directly");
    Check(LauncherWanted(true, false, true, false), "Shift brings the launcher back");
    Check(!LauncherWanted(true, true, false, true), "skip once skips a ready game");

    Check(FrameRateLabel(0) == "Unlimited" && FrameRateLabel(60) == "60 FPS (Original)" && FrameRateLabel(144) == "144 FPS",
          "frame rate labels");

    std::error_code ec;
    fs::remove_all(folder, ec);
}

void TestOptions() {
    const std::vector<WindowSize> sizes = WindowSizesFor(sos_aspect_ratios[0]);
    Check(sizes.size() == 5 && sizes.front() == WindowSize{1280, 720} && sizes[2] == kGamePictureSize &&
              sizes.back() == WindowSize{3840, 2160},
          "window sizes are the 16:9 ladder");
    const WindowSize wide = WindowSizeOf(1080, sos_aspect_ratios[sos_aspect_index("21:9")]);
    Check(wide.height == 1080 && wide.width == 2520, "window sizes follow the aspect ratio");
    Check(!ResolutionLabel(sos_resolutions[0]).empty() && IsValidSetting(kRenderResolutionKey, "4K") &&
              !IsValidSetting(kRenderResolutionKey, "480p") && IsValidSetting(kAspectRatioKey, "21:9") &&
              !IsValidSetting(kAspectRatioKey, "3:1"),
          "render resolution and aspect ratio values");
    Check(Contains(WindowSizeLabel(kGamePictureSize), "(the game's own size)"), "the game's own size is marked");
    Check(!Contains(WindowSizeLabel(WindowSize{1280, 720}), "own size"), "other sizes are not marked");
    Display small;
    small.bounds = SDL_Rect{0, 0, 1366, 768};
    small.usable = small.bounds;
    Check(WindowSizeFits(WindowSize{1280, 720}, small) && !WindowSizeFits(WindowSize{3840, 2160}, small),
          "window sizes that do not fit the display are not offered");
    Check(MonitorLabel(3, {small}).find("not connected") != std::string::npos, "missing monitor says so");
    Check(MonitorLabel(1, {small}).find("1366") != std::string::npos, "monitor label has its size");
}

void TestGameProcess() {
    Check(!SeparateGameProcessWanted(false, true) && SeparateGameProcessWanted(true, true) &&
              !SeparateGameProcessWanted(true, false),
          "overlay programs start a separate process only when available and on");
    const std::vector<std::string> arguments =
        GameProcessArguments({"--launcher", "--sos-skip-launcher", "--user", "u", "--log", "old.log", "--cpu-only"},
                             "new.log");
    const std::vector<std::string> expected = {"--user", "u", "--cpu-only", "--sos-game", "--log", "new.log"};
    Check(arguments == expected, "game process arguments drop launcher and log choices, then continue the log");
    Check(GameProcessArguments({}, "").size() == 1 && GameProcessArguments({}, "")[0] == kGameProcessArgument,
          "game process without a log is just the marker");
#ifdef _WIN32
    const std::wstring line = WindowsCommandLine(L"C:\\Program Files\\sons_of_sparta.exe", {"--log", "a b.log"});
    Check(line == L"\"C:\\Program Files\\sons_of_sparta.exe\" --log \"a b.log\"", "Windows command line quoting");
#endif
}

void TestProfile() {
    const std::span<const GameBuild> builds = SupportedBuilds();
    Check(builds.size() == 2, "two supported builds");
    for (const GameBuild& build : builds) {
        Check(std::string(build.title_id) == "CUSA57767", "supported build is a Sons title");
    }
    Check(BuildDisplayName(builds[0]) == "CUSA57767 1.00 \xC2\xB7 USA", "base build display name");
    Check(BuildMetadataText(builds[0]) == "APP_VER 1.00 \xC2\xB7 VERSION 01.00", "base build metadata text");
    Check(SupportedBuildsText() == "CUSA57767 1.00 \xC2\xB7 USA and CUSA57767 1.01 \xC2\xB7 USA", "supported builds text");
    Check(std::string(kGameTitle) == "God of War: Sons of Sparta", "game title");
    Check(std::string(kPortRepository) == "github.com/saulob/god-of-war-sons-of-sparta-pc", "port repository");
    Check(std::string(kAuthorUrl) == "https://github.com/saulob", "author URL");
    Check(std::string(kSponsorUrl) == "https://github.com/sponsors/saulob", "sponsor URL");
    const std::string summary = SettingsSummary(LauncherSettings(fs::path()), &builds[0]);
    Check(Contains(summary, "Settings: God of War: Sons of Sparta PC v") && Contains(summary, "CUSA57767 1.00"),
          "run log settings line names the game and build");
    Check(!Contains(Lower(summary), "online"), "run log settings has no online line");
}

void TestLauncherSource() {
    const std::string source = ReadFile(kRoot / "launcher" / "launcher.cpp");
    const auto has = [&](std::string_view needle, const std::string& what) { Check(Contains(source, needle), what); };
    has("c.hints = \"Enter  Play     Alt+F4  Quit     Ctrl+S  Save\";", "footer hints are exact");
    has("c.note = \"Play applies the changes. Save keeps them without starting.\";", "footer note is exact");
    has("c.sponsor = {\"Sponsor on GitHub\", kSponsorUrl, sponsor_icon_};", "footer Sponsor on GitHub configured");
    has("c.save_on_play = false;", "Save and Play are separate");
    has("WithAction(\n        kAccent, Hex(palette::kActionEdge), Hex(palette::kActionLink))", "Midnight theme with palette actions");
    has("theme.sponsor = Hex(palette::kSponsor);", "sponsor button follows the palette");
    has("c.branding.fade_into_page = false;", "header does not fade into the page");
    has("{\"PC PORT   \\xC2\\xB7   \", \"\"}", "subtitle: PC PORT");
    has("{\"v\" SOS_PORT_VERSION \"   \\xC2\\xB7   \", \"\"}", "subtitle: version");
    has("{\"BY SAULOB\", kAuthorUrl}", "subtitle: author link");
    Check(!Contains(source, "SAULO B"), "BY SAULO B is absent");
    has("c.branding.title = \"GOD OF WAR: SONS OF SPARTA\";", "header title");
    has("DisplayFramebufferScale", "header art follows the framebuffer scale");
    has("page_icons_.try_emplace({page, pixels})", "icon textures are cached per size");
    has("PageIconTexture(page, pixels)", "every page gets its icon");
    has("trg::SetTranslator(&TranslateLauncherText)", "translator installed");
    has("PlacementTracker", "window placement tracked");
    has("SavePlacement(placement_.Flush())", "window placement saved on exit");
    has("ShiftHeldAtStart() || config->force_show != 0", "Shift and --launcher bring the launcher back");
    has("ui.Toggle(\"Show this launcher\", kShowLauncherHelp, kShowLauncherKey, true, \"Off\", \"On\")", "Show this launcher row");

    for (const char* label : {"\"Display mode\"", "\"Window size\"", "\"Monitor\"", "\"Aspect Ratio\"", "\"Graphics card\"", "\"VSync\"",
                              "\"Presentation\"", "\"Scaling and sharpening\"", "\"Upscaling\"", "\"Sharpening\"",
                              "\"Sharpening strength\""}) {
        has(label, std::string("Display page row ") + label);
    }
    has("{\"windowed\", \"Windowed\"}, {\"borderless\", \"Borderless Fullscreen\"}", "Display modes");
    has("\"Off\", \"FSR 1\"", "Upscaling off/FSR 1");
    for (const char* label : {"\"Render resolution\"", "\"Anti-aliasing\"", "\"Texture filtering\""}) {
        has(label, std::string("Graphics page row ") + label);
    }
    has("\"Off\", \"SMAA\"", "Anti-aliasing off/SMAA");
    has("{{\"0\", \"Game\"}, {\"2\", \"2x\"}, {\"4\", \"4x\"}, {\"8\", \"8x\"}, {\"16\", \"16x\"}}", "Texture filtering choices");
    for (const char* label : {"\"Frame rate\"", "\"Frame rate in title\"", "\"Render resolution in title\"", "\"Frame counter\"",
                              "\"Skip Intro\"", "\"Unlock Hermes Mode\"", "\"Allow overlay programs\""}) {
        has(label, std::string("Gameplay page row ") + label);
    }
    has("for (const int fps : {30, 60, 90, 120, 144, 165, 240, 0})", "frame rate choices");
    has("ui.Toggle(\"Frame counter\", kFrameCounterHelp, kShowFrameCounterKey, false, \"Hidden\", \"Shown\")", "Frame counter toggle");
    has("build_ && build_->skip_intro", "Skip Intro only where the build supports it");
    has("ui.Toggle(\"Unlock Hermes Mode\", kUnlockHermesModeHelp, kUnlockHermesModeKey, false, \"Off\", \"On\")",
        "Unlock Hermes Mode toggle defaults off");
    const size_t skip_intro = source.find("ui.Toggle(\"Skip Intro\"");
    const size_t hermes = source.find("ui.Toggle(\"Unlock Hermes Mode\"");
    const size_t overlay = source.find("ui.Toggle(\"Allow overlay programs\"");
    Check(skip_intro != std::string::npos && skip_intro < hermes && hermes < overlay, "Hermes follows Skip Intro in Gameplay");
    Check(!Contains(source, "Asian Menu Buttons") && !Contains(source, "Local co-op") && !Contains(source, "kAsianMenuButtons"),
          "Controls has no Asian Menu Buttons or Local co-op");
    Check(!Contains(source, "Not available") && !Contains(source, "Unavailable"), "no unavailable rows");
    for (const char* label : {"\"Keyboard defaults\"", "\"Keyboard controls\"", "\"Port hotkeys\"", "\"Reset to defaults\"",
                              "ui.KeyBindRow", "\"Click a key, then press the new one. Esc cancels. A key can be used for more than one control.\"",
                              "\"Controller\"", "\"Keyboard\""}) {
        has(label, std::string("Controls page ") + label);
    }
    for (const char* label : {"ui.SponsorCard", "\"Game files\"", "\"Saves and settings\"", "\"Settings file\"", "\"Show settings file\"",
                              "\"Reset settings\"", "\"Reset all settings\"", "\"Diagnostics\"", "\"Detailed logging\"",
                              "\"Logs folder\"", "{\"Port version\"", "{\"Game\"", "{\"Renderer\"", "{\"Platform\"",
                              "\"Libraries\"", "SDL %d.%d.%d", "Dear ImGui %s", "TRG Launcher %s", "ui.Link(kPortRepository, kPortUrl)"}) {
        has(label, std::string("About page ") + label);
    }
    for (const char* label : {"\"Reinstall game...\"", "\"Install game...\"", "\"Install update...\"", "\"Remove update\"",
                              "ui.StatusCard", "ui.TaskProgress(install_)", "\"Ready to play\"", "BuildMetadataText(*build_)"}) {
        has(label, std::string("Play page ") + label);
    }
    Check(!Contains(source, "PageId::kCheats") && !Contains(source, "\"Cheats\""), "Cheats page and label are absent");
    Check(!Contains(ReadFile(kRoot / "launcher" / "options.h"), "kCheats") &&
              !Contains(ReadFile(kRoot / "launcher" / "page_icons.cpp"), "kCheats") &&
              !Contains(ReadFile(kRoot / "launcher" / "page_icons.cpp"), "cheats.svg"),
          "Cheats page id and icon are absent");
    has("bug_report_page_->Draw(ui, facts)", "Bug Report page draws the bug report page");
    has("trophy_page_->Draw(ui)", "Trophies page draws the trophy page");
    has("trg::update::UpdateController", "updater is part of the launcher");
    has("DrawUpdateBanner", "update banner is drawn");
    Check(Contains(source, "kInstallTitle[] = \"Select your Sons of Sparta base game package\""), "install chooser title");

    int pages = 0;
    for (size_t at = source.find("case PageId::k"); at != std::string::npos; at = source.find("case PageId::k", at + 1)) {
        ++pages;
    }
    Check(pages >= 8, "every page is built");
}

void TestDocumentation() {
    const std::string readme = ReadFile(kRoot / "README.md");
    Check(Contains(readme, "## Hermes Mode"), "README has a Hermes Mode section");
    Check(Contains(readme, "Unlock Hermes Mode") && Contains(readme, "speedrun mode"),
          "README explains the Hermes Mode option and the original mode");
    Check(Contains(readme, "disabled by default"), "README states the Hermes Mode option is off by default");
    Check(Contains(readme, "cannot be changed afterward"), "README states the save's mode is fixed");
}

void TestNoOnlineBackend() {
    for (const char* file : {"online_client.cpp", "online_client.h", "online_page.cpp", "online_page.h", "online_view.cpp",
                             "online_view.h", "online_settings.cpp", "online_settings.h", "online_status_script.c",
                             "controller_prompts.cpp", "exit_game_script.c", "menu_buttons.c", "text_fix.c",
                             "frame_rate.c", "resolution.c"}) {
        Check(!fs::exists(kRoot / "launcher" / file), std::string("launcher has no ") + file);
    }
    const std::vector<std::string> forbidden = {"online_page", "online_client", "online_view", "OnlinePage", "kOnline",
                                                "online.svg", "Connection test", "PlayNeedsTest", "lobby", "Lobby",
                                                "IncidentPage", "MyReports", "workers.dev", "Worker URL"};
    for (const fs::path& path : SourceFiles(kRoot / "launcher")) {
        const std::string name = Relative(path);
        if (name.find("launcher/third_party/") == 0 || name.find("launcher/updater/third_party/") == 0) {
            continue;
        }
        const std::string text = ReadFile(path);
        for (const std::string& word : forbidden) {
            Check(!Contains(text, word), name + " has no " + word);
        }
    }
}

void TestNoAlienationLeftovers() {
    const std::string a = "alien", b = "ation";
    const std::vector<std::string> words = {a + b, "aln_", "cusa04430", "cusa00062", "alienation-pc", "ALN_", "AlnPng"};
    std::vector<fs::path> files;
    for (const char* folder : {"launcher", "assets", "packaging", "tests", "src/trophies"}) {
        for (const fs::path& path : SourceFiles(kRoot / folder)) {
            files.push_back(path);
        }
    }
    for (const char* script : {"tools/test_launcher.ps1", "tools/test_updater.ps1", "tools/test_trophies.ps1"}) {
        files.push_back(kRoot / script);
    }
    for (const fs::path& path : files) {
        const std::string name = Relative(path);
        if (name == "tests/launcher_tests.cpp" || name.find("third_party/") != std::string::npos) {
            continue;
        }
        const std::string text = Lower(ReadFile(path));
        for (const std::string& word : words) {
            Check(!Contains(text, Lower(word)), name + " has no " + word);
        }
    }
    const std::string patches = ReadFile(kRoot / "gpu" / "patches" / "trg-launcher" / "0016-download-watchdog.patch");
    Check(!Contains(patches, "ALNHTTP") && !Contains(patches, "DCPHTTP"), "TRG download patch has no other project's marker");
}

void TestTrgPatchSet() {
    const fs::path folder = kRoot / "gpu" / "patches" / "trg-launcher";
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        if (entry.path().extension() == ".patch") {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    Check(names.size() == 21, "TRG launcher has its 21 patches");
    for (size_t i = 0; i < names.size(); ++i) {
        char prefix[8];
        std::snprintf(prefix, sizeof(prefix), "%04zu-", i + 1);
        Check(names[i].rfind(prefix, 0) == 0, "TRG patch numbering is continuous: " + names[i]);
    }
    Check(names.size() > 20 && names[19] == "0020-action-edge-width.patch", "TRG action edge width patch is twentieth");
    Check(!names.empty() && names.back() == "0021-download-hardening.patch", "TRG download hardening patch is last");
    const std::string hardening = ReadFile(folder / names.back());
    for (const char* needle : {"enum class HttpFailure", "IsSensitiveHeader", "ResolveRedirect", "retry_after_ms",
                               "kConnectTimeout", "kReceiveTimeout", "kTotalTimeout", "kHttpsDowngrade", "kSizeLimit"}) {
        Check(Contains(hardening, needle), std::string("TRG download hardening patch has ") + needle);
    }
    Check(!Contains(hardening, "ALNHTTP") && !Contains(hardening, "DCPHTTP"), "TRG download hardening patch has no other project's marker");
    const std::string deps = ReadFile(kRoot / "gpu" / "deps.txt");
    Check(Contains(deps, "b384a28c712c9320eea8f9d50639dec49981b6c1"), "TRG Launcher 1.1.0 pin");
}

} // namespace

int main() {
    TestPages();
    TestIcons();
    TestSponsorAsset();
    TestPalette();
    TestActionEdge();
    TestVersion();
    TestApplicationIcon();
    TestHeaderArt();
    TestSettings();
    TestOptions();
    TestGameProcess();
    TestProfile();
    TestLauncherSource();
    TestDocumentation();
    TestNoOnlineBackend();
    TestNoAlienationLeftovers();
    TestTrgPatchSet();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
