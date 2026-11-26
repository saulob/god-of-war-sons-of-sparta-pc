// The launcher's Graphics card list, asked of Vulkan only after the window has drawn a few frames (Dragon's Crown Pro
// 4552ba3 and fefc78a). The probe creates Vulkan instances on the UI thread: before this, a driver that hung or crashed
// there left no window and no word in the log. A marker file exists only while the probe runs; a start that finds it
// skips the probe once (the last one did not finish) and removes it, so the start after that asks again. PLAY still runs
// its own graphics preflight. SDL-free, for headless tests.
#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace sos {

enum class GraphicsProbeState { kPending, kInterrupted, kProbed };

struct GraphicsCards {
    GraphicsProbeState state = GraphicsProbeState::kPending;
    std::vector<std::string> names;
};

inline constexpr char kGpuProbeMarker[] = "gpu_probe.pending";
inline constexpr int kProbeAfterFrames = 3;
inline constexpr double kProbeAfterSeconds = 0.5;

inline bool GraphicsProbeDue(int frames_shown, double seconds_shown) {
    return frames_shown >= kProbeAfterFrames && seconds_shown >= kProbeAfterSeconds;
}

inline GraphicsCards DetectGraphicsCards(const std::filesystem::path& marker, const std::function<void()>& before,
                                         const std::function<std::vector<std::string>()>& probe) {
    GraphicsCards cards;
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) {
        std::filesystem::remove(marker, ec);
        cards.state = GraphicsProbeState::kInterrupted;
        return cards;
    }
    {
        std::ofstream pending(marker, std::ios::binary | std::ios::trunc);
        pending << "graphics card detection started\n";
    }
    if (before) {
        before();
    }
    cards.names = probe ? probe() : std::vector<std::string>{};
    std::filesystem::remove(marker, ec);
    cards.state = GraphicsProbeState::kProbed;
    return cards;
}

// The English text of the Graphics card row when there is no card to choose; "" when there are cards.
inline const char* GraphicsCardsText(const GraphicsCards& cards) {
    if (!cards.names.empty()) {
        return "";
    }
    switch (cards.state) {
    case GraphicsProbeState::kPending: return "Detecting graphics cards...";
    case GraphicsProbeState::kInterrupted: return "Graphics card detection was skipped (it did not finish last time)";
    case GraphicsProbeState::kProbed: break;
    }
    return "No graphics card found";
}

} // namespace sos
