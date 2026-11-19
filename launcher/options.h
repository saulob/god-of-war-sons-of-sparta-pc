// What the Display page offers and how it labels it.
#pragma once

#include <string>
#include <vector>

#include "sos_displays.h"

namespace sos {

struct WindowSize {
    int width = 0;
    int height = 0;
    bool operator==(const WindowSize&) const = default;
};

inline constexpr int kWindowHeights[] = {720, 900, 1080, 1440, 2160};
inline constexpr WindowSize kGamePictureSize{1920, 1080};

std::vector<WindowSize> WindowSizes();

enum class PageId { kPlay, kDisplay, kGraphics, kGameplay, kControls, kBugReport, kAbout };
inline constexpr PageId kPageOrder[] = {PageId::kPlay,      PageId::kDisplay, PageId::kGraphics,
                                        PageId::kGameplay,  PageId::kControls, PageId::kBugReport,
                                        PageId::kAbout};
const char* PageName(PageId page);

WindowSize FitGameWindow(WindowSize requested, const Display& display);
bool WindowSizeFits(WindowSize size, const Display& display);
std::string WindowSizeLabel(WindowSize size);
std::string MonitorLabel(int monitor, const std::vector<Display>& displays);

} // namespace sos
