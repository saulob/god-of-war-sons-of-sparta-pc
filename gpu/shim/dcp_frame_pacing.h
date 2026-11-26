// SPDX-License-Identifier: GPL-2.0-or-later
// The Frame rate setting. The Unity player renders one new frame per guest vblank (vSyncCount 1 gives flip rate 0, and the
// game never sets Application.targetFrameRate), and its physics step is a fixed 1/60 s. Above 60 the guest vblank itself
// runs at the chosen rate, so the game really renders that many frames; 30 keeps the 60 Hz vblank and shows every second
// flip, so the game's speed is untouched. A frame is never shown twice to reach a rate.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

// VSync, or a compositor such as Gamescope that shows one image per refresh whatever the present mode.
constexpr bool DisplayPacesPresents(bool vsync, bool compositor_paced) {
    return vsync || compositor_paced;
}

constexpr bool VsyncBoundsRate(bool display_paced, double display_hz) {
    return display_paced && display_hz > 0.0;
}

// The rate frames can actually reach the screen at; 0 = uncapped. When the display paces presents (VSync, Gamescope)
// it is the ceiling.
constexpr int EffectiveRenderHz(int setting, bool display_paced = false, double display_hz = 0.0) {
    if (!IsHighRate(setting) || !VsyncBoundsRate(display_paced, display_hz)) {
        return RenderFps(setting);
    }
    const int display = NominalHz(display_hz);
    return IsUnlimited(setting) ? display : std::min(RenderFps(setting), display);
}

// The vblank rate the guest is given, which is also the number of new frames it can render a second: never below 60.
constexpr unsigned GuestVblankHz(int setting, bool display_paced = false, double display_hz = 0.0) {
    if (!IsHighRate(setting)) {
        return kGameHz;
    }
    const int hz = EffectiveRenderHz(setting, display_paced, display_hz);
    return static_cast<unsigned>(std::clamp(hz == 0 ? kUnlimitedVblankHz : hz, kGameHz, kUnlimitedVblankHz));
}

// The new frames a second the game can show: the 60 Hz flips halved at 30, else the guest vblank.
constexpr unsigned ShownFramesPerSecond(int setting, bool display_paced = false, double display_hz = 0.0) {
    return RenderFps(setting) == 30 ? 30u : GuestVblankHz(setting, display_paced, display_hz);
}

// 30 keeps the 60 Hz vblank and shows every second flip; all flips are still consumed. A game already flipping
// every second vblank (flip_rate 1 or more) is shown whole, so 30 never halves it again.
constexpr bool PresentsFlip(int setting, std::uint64_t flip_index, int flip_rate = 0) {
    return RenderFps(setting) != 30 || flip_rate > 0 || flip_index % 2 == 0;
}

inline std::string ModeName(int setting) {
    return setting == 0 ? "Unlimited" : std::to_string(setting);
}

// ---- The display refresh changing at runtime (a Steam Deck switched between 60 and 90 Hz, the window moved to
// another monitor). The Frame rate is read once at startup and VSync and Gamescope are fixed for the session, so only
// the display refresh can move the guest vblank. The window thread debounces the refresh it sees; a value that stays
// kRefreshSettleMs is published with the vblank it gives, and the present thread reads that with one relaxed load per
// vblank and retimes its period from the next vblank on. The game's clock (sceKernelGetProcessTimeCounter, the host's
// monotonic clock) and its fixed 1/60 s physics step do not depend on the vblank, so the game's speed never changes;
// only the number of new frames a second does, exactly as when the same rate is chosen at startup.

inline constexpr std::uint64_t kRefreshSettleMs = 500;
// Lines the present thread logs for refresh changes; a display that keeps flapping is not logged forever.
inline constexpr unsigned kRefreshLogLines = 16;

// The startup vblank policy the driver publishes for the window thread; 0 until the driver started.
constexpr std::uint64_t PackPolicy(int setting, bool display_paced, unsigned display_hz) {
    return std::uint64_t{1} << 63 | std::uint64_t{display_paced} << 62 |
           std::uint64_t(static_cast<unsigned>(setting) & 0xffffu) << 32 | (display_hz & 0xffffu);
}
// A display refresh (nominal Hz) and the guest vblank it gives; 0 until a change was published.
constexpr std::uint64_t PackLive(unsigned display_hz, unsigned vblank_hz) {
    return std::uint64_t{1} << 63 | std::uint64_t(display_hz & 0xffffu) << 32 | (vblank_hz & 0xffffu);
}
constexpr unsigned LiveDisplayHz(std::uint64_t live) {
    return static_cast<unsigned>(live >> 32) & 0xffffu;
}
constexpr unsigned LiveVblankHz(std::uint64_t live) {
    return static_cast<unsigned>(live) & 0xffffu;
}

struct LiveVblank {
    std::atomic<std::uint64_t> policy{0}; ///< PackPolicy, written once by the driver
    std::atomic<std::uint64_t> target{0}; ///< PackLive, written by the window thread on a settled change
};
inline LiveVblank g_live_vblank;

// Window thread only: follows the display refresh and returns a PackLive value once a different nominal rate has held
// for kRefreshSettleMs, else 0. A rate that goes back before it settles, or an unknown (0) refresh, changes nothing.
class RefreshFollower {
public:
    bool Started() const {
        return started_;
    }

    void Start(std::uint64_t policy) {
        setting_ = static_cast<int>((policy >> 32) & 0xffffu);
        display_paced_ = ((policy >> 62) & 1u) != 0;
        applied_ = static_cast<int>(policy & 0xffffu);
        pending_ = 0;
        started_ = true;
    }

    std::uint64_t Observe(double display_hz, std::uint64_t now_ms) {
        if (!started_) {
            return 0;
        }
        const int nominal = display_hz > 0.0 ? NominalHz(display_hz) : 0;
        if (nominal <= 0 || nominal == applied_) {
            pending_ = 0;
            return 0;
        }
        if (nominal != pending_) {
            pending_ = nominal;
            since_ms_ = now_ms;
            return 0;
        }
        if (now_ms - since_ms_ < kRefreshSettleMs) {
            return 0;
        }
        applied_ = nominal;
        pending_ = 0;
        return PackLive(static_cast<unsigned>(nominal), GuestVblankHz(setting_, display_paced_, nominal));
    }

private:
    bool started_ = false;
    int setting_ = kGameHz;
    bool display_paced_ = false;
    int applied_ = 0; ///< the nominal refresh the published vblank was computed for
    int pending_ = 0; ///< a different refresh waiting to settle; 0 = none
    std::uint64_t since_ms_ = 0;
};

// Present thread: the guest vblank a published change asks for, or 0 when the vblank stays as it is.
constexpr unsigned RetimeHz(unsigned current_hz, std::uint64_t live) {
    const unsigned to = LiveVblankHz(live);
    return live != 0 && to >= static_cast<unsigned>(kGameHz) && to != current_hz ? to : 0u;
}

// Present thread: the one line for an applied change, bounded to kRefreshLogLines and a closing note.
inline bool RefreshChangeLine(unsigned& lines, unsigned display_from, unsigned display_to, unsigned vblank_from,
                              unsigned vblank_to, char* line, std::size_t size) {
    if (lines > kRefreshLogLines) {
        return false;
    }
    if (lines++ == kRefreshLogLines) {
        std::snprintf(line, size, "VideoOut: further display refresh changes are not logged");
        return true;
    }
    std::snprintf(line, size, "VideoOut: display refresh changed %u -> %u Hz, guest vblank %u -> %u Hz", display_from,
                  display_to, vblank_from, vblank_to);
    return true;
}

} // namespace DcpFramePacing
