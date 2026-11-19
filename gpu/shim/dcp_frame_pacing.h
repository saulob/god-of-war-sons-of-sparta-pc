// SPDX-License-Identifier: GPL-2.0-or-later
// The Frame rate setting. The Unity player renders one new frame per guest vblank (vSyncCount 1 gives flip rate 0, and the
// game never sets Application.targetFrameRate), and its physics step is a fixed 1/60 s. Above 60 the guest vblank itself
// runs at the chosen rate, so the game really renders that many frames; 30 keeps the 60 Hz vblank and shows every second
// flip, so the game's speed is untouched. A frame is never shown twice to reach a rate.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace DcpFramePacing {

// The values the launcher stores; 0 is Unlimited.
inline constexpr std::array kFrameRateModes{30, 60, 90, 120, 144, 165, 240, 0};

// The guest vblank of Unlimited when no display refresh bounds it.
inline constexpr int kUnlimitedVblankHz = 480;
inline constexpr int kGameHz = 60;

constexpr bool IsFrameRateMode(int setting) {
    for (const int mode : kFrameRateModes) {
        if (mode == setting) {
            return true;
        }
    }
    return false;
}

constexpr bool IsUnlimited(int setting) {
    return setting == 0;
}

// The setting a dcp.ini value stands for; anything but a stored value exactly is the original 60, as in the launcher.
inline int ParseFrameRate(const std::string& value) {
    for (const int mode : kFrameRateModes) {
        if (value == std::to_string(mode)) {
            return mode;
        }
    }
    return kGameHz;
}

// The rate the setting asks for; 0 = uncapped.
constexpr int RenderFps(int setting) {
    if (IsUnlimited(setting)) {
        return 0;
    }
    return IsFrameRateMode(setting) ? setting : kGameHz;
}

// Above the game's own 60: the guest vblank is raised.
constexpr bool IsHighRate(int setting) {
    return IsUnlimited(setting) || RenderFps(setting) > kGameHz;
}

// The display's refresh rounded to the whole rate its modes are named by (59.94 -> 60, 143.856 -> 144).
constexpr int NominalHz(double hz) {
    return static_cast<int>(hz + 0.5);
}

constexpr bool VsyncBoundsRate(bool vsync, double display_hz) {
    return vsync && display_hz > 0.0;
}

// The rate frames can actually reach the screen at; 0 = uncapped. A VSync present queue never goes past the display.
constexpr int EffectiveRenderHz(int setting, bool vsync = false, double display_hz = 0.0) {
    if (!IsHighRate(setting) || !VsyncBoundsRate(vsync, display_hz)) {
        return RenderFps(setting);
    }
    const int display = NominalHz(display_hz);
    return IsUnlimited(setting) ? display : std::min(RenderFps(setting), display);
}

// The vblank rate the guest is given, which is also the number of new frames it can render a second: never below 60.
constexpr unsigned GuestVblankHz(int setting, bool vsync = false, double display_hz = 0.0) {
    if (!IsHighRate(setting)) {
        return kGameHz;
    }
    const int hz = EffectiveRenderHz(setting, vsync, display_hz);
    return static_cast<unsigned>(std::clamp(hz == 0 ? kUnlimitedVblankHz : hz, kGameHz, kUnlimitedVblankHz));
}

// The new frames a second the game can show: the 60 Hz flips halved at 30, else the guest vblank.
constexpr unsigned ShownFramesPerSecond(int setting, bool vsync = false, double display_hz = 0.0) {
    return RenderFps(setting) == 30 ? 30u : GuestVblankHz(setting, vsync, display_hz);
}

// 30 keeps the 60 Hz vblank and shows every second flip; all flips are still consumed. A game already flipping
// every second vblank (flip_rate 1 or more) is shown whole, so 30 never halves it again.
constexpr bool PresentsFlip(int setting, std::uint64_t flip_index, int flip_rate = 0) {
    return RenderFps(setting) != 30 || flip_rate > 0 || flip_index % 2 == 0;
}

inline std::string ModeName(int setting) {
    return setting == 0 ? "Unlimited" : std::to_string(setting);
}

} // namespace DcpFramePacing
