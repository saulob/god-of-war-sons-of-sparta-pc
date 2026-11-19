// SPDX-License-Identifier: GPL-2.0-or-later
// Graphics options as dcp.ini stores them, shared by the window and the renderer.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace DcpGraphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {"windowed", "borderless"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {"Windowed", "Borderless Fullscreen"};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Windowed);
}

/// Letterbox keeps the frame's aspect ratio in the window; Stretch fills the window with it.
enum Presentation : int { Letterbox = 0, Stretch = 1, PresentationCount };
inline constexpr std::array<std::string_view, PresentationCount> PresentationKeys = {"letterbox", "stretch"};

inline int ParsePresentation(std::string_view text) {
    return IndexOf(PresentationKeys, text, Letterbox);
}
/// RCAS strength in percent; invalid values fall back to 75.
inline int ParseRcasStrength(int value) {
    return value >= 0 && value <= 100 ? value : 75;
}
/// Forced anisotropic filtering: 0 is the game's own, else 2, 4, 8 or 16 times.
inline constexpr std::array<int, 5> Anisotropy = {0, 2, 4, 8, 16};
inline int ParseAnisotropy(int value) {
    return IndexOf(Anisotropy, value, -1) >= 0 ? value : 0;
}
/// render_resolution as the launcher stores it (launcher/resolution.c); anything else is native.
inline constexpr std::array<std::string_view, 6> RenderResolutionKeys = {"native", "480p", "720p", "1440p", "1800p", "4K"};
inline constexpr std::string_view NativeResolutionKey = "native";
inline std::string_view ParseRenderResolution(std::string_view text) {
    const int index = IndexOf(RenderResolutionKeys, text, -1);
    return index < 0 ? NativeResolutionKey : RenderResolutionKeys[std::size_t(index)];
}

struct AspectRatio {
    std::string_view key;
    int width;
    int height;
};
/// aspect_ratio as the launcher stores it; the first is the game's own shape.
inline constexpr std::array<AspectRatio, 6> AspectRatios = {
    {{"16:9", 16, 9}, {"16:10", 16, 10}, {"21:9", 21, 9}, {"32:9", 32, 9}, {"4:3", 4, 3}, {"5:4", 5, 4}}};
inline const AspectRatio& ParseAspectRatio(std::string_view text) {
    for (const AspectRatio& aspect : AspectRatios) {
        if (aspect.key == text) return aspect;
    }
    return AspectRatios[0];
}
/// The shapes True Ultrawide applies to.
constexpr bool WiderThanOriginal(const AspectRatio& aspect) {
    return aspect.width * 9 > aspect.height * 16;
}
/// The window width the launcher saves with an aspect ratio for a window height.
constexpr int WindowWidthFor(int height, const AspectRatio& aspect) {
    return (height * aspect.width * 2 + aspect.height) / (2 * aspect.height);
}
/// The usable device names among `names`, in enumeration order (an unusable device has an empty name).
inline std::vector<std::string> GpuChoices(const std::vector<std::string>& names) {
    std::vector<std::string> choices;
    for (const std::string& name : names) {
        if (!name.empty()) {
            choices.push_back(name);
        }
    }
    return choices;
}
/// Index of the device named `request` among `names` (an unusable device has an empty name); -1 when the
/// request is empty or names no device.
inline int FindGpuByName(const std::vector<std::string>& names, std::string_view request) {
    if (request.empty()) return -1;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty() && names[i] == request) return int(i);
    }
    return -1;
}
/// The device named `request`, else the first usable device; -1 only when no device is usable.
inline int ResolveGpuByName(const std::vector<std::string>& names, std::string_view request) {
    const int found = FindGpuByName(names, request);
    if (found >= 0) return found;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty()) return int(i);
    }
    return -1;
}
/// FSR RCAS attenuation for a strength: 0 is the sharpest, 2 the softest.
inline float RcasAttenuation(int strength) {
    return 2.0f * (1.0f - float(ParseRcasStrength(strength)) / 100.0f);
}
/// Whether the FSR pass runs: FSR 1 when the window is larger than the game's image, RCAS
/// alone when they are the same size. A smaller window is left to the regular scaling.
inline bool FsrPassRuns(bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool upscale = in_w < out_w && in_h < out_h;
    const bool same = in_w == out_w && in_h == out_h;
    return (fsr1 && upscale) || (rcas && same);
}

/// The ratio (2, 4, 8 or 16) forced on a game sampler; 0 leaves the sampler as the game made it. Depth, compute,
/// mipless and point-filtered samplers are never changed, and a sampler that already asks for the ratio or more is kept.
inline int ForcedAnisotropyRatio(int forced, bool excluded, bool has_mips, bool linear_min, bool already_aniso,
                                 float current_max) {
    if (forced <= 1 || excluded || !has_mips || !linear_min || (already_aniso && current_max >= float(forced))) {
        return 0;
    }
    return forced >= 16 ? 16 : forced >= 8 ? 8 : forced >= 4 ? 4 : 2;
}
/// What the Vulkan sampler gets: the requested ratio clamped to the device limit, or 1 without the feature or an
/// anisotropic filter.
inline float EffectiveMaxAnisotropy(bool feature, bool aniso_filter, float requested, float device_max) {
    if (!feature || !aniso_filter) {
        return 1.0f;
    }
    const float upper = device_max < 1.0f ? 1.0f : device_max;
    return requested < 1.0f ? 1.0f : requested > upper ? upper : requested;
}

/// Which host passes run on the game's image.
struct PostProcessPlan {
    bool smaa;
    bool fsr_pass; ///< the FSR pass runs (EASU, RCAS or both)
    bool rcas;     ///< sharpening is applied
};
inline PostProcessPlan PlanPostProcess(bool smaa, bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool pass = FsrPassRuns(fsr1, rcas, in_w, in_h, out_w, out_h);
    return {smaa, pass, pass && rcas};
}

/// Whether render targets made for (have_w, have_h) must be recreated for (want_w, want_h).
constexpr bool TargetsStale(unsigned have_w, unsigned have_h, unsigned want_w, unsigned want_h) {
    return have_w != want_w || have_h != want_h;
}

} // namespace DcpGraphics
