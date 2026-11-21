#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher_frame_loop.h"
#include "placement.h"

namespace sos {

enum class StartupStage {
    kSdlReady,
    kDisplaySelected,
    kWindowCreated,
    kRendererReady,
    kUiReady,
    kPagesReady,
    kFirstFramePresented,
    kShown,
    kLoopActive,
    kCount
};

inline constexpr size_t kStartupStageCount = static_cast<size_t>(StartupStage::kCount);
inline constexpr int64_t kSlowStageNs = 2 * kNsPerSecond;

inline const char* StartupStageName(StartupStage stage) {
    constexpr const char* kNames[] = {"sdl", "display", "window", "renderer", "ui", "pages", "first_present", "shown", "loop"};
    return kNames[static_cast<size_t>(stage)];
}

// Literals for the crash report's last startup stage.
inline const char* StartupStagePhase(StartupStage stage) {
    constexpr const char* kPhases[] = {"launcher sdl ready",  "launcher display selected",       "launcher window created",
                                       "launcher renderer ready", "launcher ui ready",           "launcher pages ready",
                                       "launcher first present", "launcher shown",               "launcher loop active"};
    return kPhases[static_cast<size_t>(stage)];
}

struct StageReached {
    StartupStage stage = StartupStage::kSdlReady;
    int64_t took_ns = 0;
    bool slow = false;
};

// Each stage is recorded once, in order, with the time since the previous recorded stage.
class StartupTimeline {
public:
    explicit StartupTimeline(int64_t begin_ns = 0) : begin_(begin_ns), last_(begin_ns) {}

    std::optional<StageReached> Reach(StartupStage stage, int64_t now_ns) {
        const size_t index = static_cast<size_t>(stage);
        if (index >= kStartupStageCount || took_[index] >= 0 || (any_ && index < last_stage_)) {
            return std::nullopt;
        }
        const int64_t took = std::max<int64_t>(now_ns - last_, 0);
        took_[index] = took;
        last_ = now_ns;
        last_stage_ = index;
        any_ = true;
        return StageReached{stage, took, took >= kSlowStageNs};
    }

    bool Reached(StartupStage stage) const { return took_[static_cast<size_t>(stage)] >= 0; }
    std::optional<StartupStage> Last() const {
        return any_ ? std::optional<StartupStage>(static_cast<StartupStage>(last_stage_)) : std::nullopt;
    }
    int64_t TotalNs() const { return std::max<int64_t>(last_ - begin_, 0); }

    std::string SlowLine(const StageReached& reached, std::string_view renderer, bool minimized) const {
        char line[192];
        std::snprintf(line, sizeof(line), "Launcher startup slow: stage=%s took=%lldms renderer=%.32s minimized=%d",
                      StartupStageName(reached.stage), static_cast<long long>(reached.took_ns / kNsPerMs),
                      std::string(renderer).c_str(), minimized ? 1 : 0);
        return line;
    }

    std::string SummaryLine(std::string_view renderer) const {
        std::string line = "Launcher startup: total=" + std::to_string(TotalNs() / kNsPerMs) + "ms renderer=" +
                           std::string(renderer).substr(0, 32);
        for (size_t i = 0; i < kStartupStageCount; ++i) {
            if (took_[i] >= 0) {
                line += std::string(" ") + StartupStageName(static_cast<StartupStage>(i)) + "=" + std::to_string(took_[i] / kNsPerMs);
            }
        }
        return line;
    }

private:
    int64_t begin_;
    int64_t last_;
    size_t last_stage_ = 0;
    bool any_ = false;
    std::array<int64_t, kStartupStageCount> took_ = [] {
        std::array<int64_t, kStartupStageCount> values{};
        values.fill(-1);
        return values;
    }();
};

// The window is shown exactly once, and only after a renderer exists and a first frame was presented to the hidden window.
class ShowGate {
public:
    void Opened(bool hidden_mode) { hidden_mode_ = hidden_mode; }
    void RendererReady() { renderer_ = true; }
    void FramePresented() { presented_ = renderer_; }

    bool TryShow() {
        if (hidden_mode_ || shown_ || !renderer_ || !presented_) {
            return false;
        }
        shown_ = true;
        return true;
    }

    bool shown() const { return shown_; }

private:
    bool hidden_mode_ = false;
    bool renderer_ = false;
    bool presented_ = false;
    bool shown_ = false;
};

struct DisplayFacts {
    std::string name;
    Rect bounds;
    Rect usable;
    float scale = 1.0f;
    bool primary = false;
};

enum class SavedPlacement { kNone, kRestored, kStale };

struct PlacementFacts {
    Rect window;
    int pixel_width = 0;
    int pixel_height = 0;
    float pixel_density = 0.0f;
    float content_scale = 0.0f;
    bool maximized = false;
    bool fullscreen = false;
    SavedPlacement saved = SavedPlacement::kNone;
};

// 1-based like the Monitor setting; 0 when the window lies on none of them. The display holding the window's centre wins,
// else the one it overlaps most.
inline int DisplayIndexOf(const Rect& window, const std::vector<DisplayFacts>& displays) {
    const int cx = (window.left + window.right) / 2, cy = (window.top + window.bottom) / 2;
    int best = 0;
    long long best_area = 0;
    for (size_t i = 0; i < displays.size(); ++i) {
        const Rect& area = displays[i].bounds;
        if (cx >= area.left && cx < area.right && cy >= area.top && cy < area.bottom) {
            return static_cast<int>(i) + 1;
        }
        const long long width = std::min(window.right, area.right) - std::max(window.left, area.left);
        const long long height = std::min(window.bottom, area.bottom) - std::max(window.top, area.top);
        if (width > 0 && height > 0 && width * height > best_area) {
            best_area = width * height;
            best = static_cast<int>(i) + 1;
        }
    }
    return best;
}

inline std::string PlacementLine(std::string_view stage, const PlacementFacts& facts, const std::vector<DisplayFacts>& displays) {
    char display[200];
    const int index = DisplayIndexOf(facts.window, displays);
    if (index > 0) {
        const DisplayFacts& d = displays[static_cast<size_t>(index) - 1];
        std::string name;
        for (const char c : d.name.substr(0, 32)) {
            name += (c >= 32 && c < 127 && c != '"') ? c : '_';
        }
        std::snprintf(display, sizeof(display), "display=%d/%d name=\"%s\" bounds=%d,%d,%dx%d usable=%d,%d,%dx%d scale=%.2f",
                      index, static_cast<int>(displays.size()), name.c_str(), d.bounds.left, d.bounds.top, d.bounds.width(),
                      d.bounds.height(), d.usable.left, d.usable.top, d.usable.width(), d.usable.height(), d.scale);
    } else {
        std::snprintf(display, sizeof(display), "display=none/%d", static_cast<int>(displays.size()));
    }
    const char* saved = facts.saved == SavedPlacement::kRestored ? "restored" : facts.saved == SavedPlacement::kStale ? "stale" : "none";
    char line[512];
    std::snprintf(line, sizeof(line),
                  "Launcher placement: stage=%.12s x=%d y=%d size=%dx%d %s pixels=%dx%d density=%.2f content_scale=%.2f "
                  "maximized=%d fullscreen=%d saved=%s",
                  std::string(stage).c_str(), facts.window.left, facts.window.top, facts.window.width(), facts.window.height(),
                  display, facts.pixel_width, facts.pixel_height, facts.pixel_density, facts.content_scale,
                  facts.maximized ? 1 : 0, facts.fullscreen ? 1 : 0, saved);
    return line;
}

} // namespace sos
