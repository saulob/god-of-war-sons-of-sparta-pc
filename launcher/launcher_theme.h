// The launcher's colours as 0xRRGGBB. Gold, parchment and brown come from the game's cover (sce_sys/icon0.png, pic0.png);
// the red is its deep red (#661B18) lifted to read as a border on the dark background.
#pragma once

#include "sos_accent.h"

namespace sos::palette {

inline constexpr int kAccent = (kAccentRed << 16) | (kAccentGreen << 8) | kAccentBlue;
inline constexpr int kAccentHot = 0xF0C878;
inline constexpr int kActionPressed = 0xA67B3C;
inline constexpr int kActionEdge = 0xA82C26;
inline constexpr float kActionEdgeWidth = 2.0f;
inline constexpr int kActionLink = 0xEDCB82;
inline constexpr int kOnAction = 0x2C1A16;

inline constexpr int kBackground = 0x120A07;
inline constexpr int kPanel = 0x1B100C;
inline constexpr int kFrame = 0x2C1A16;
inline constexpr int kFrameHot = 0x3A241A;
inline constexpr int kFrameActive = 0x4A2B1E;
inline constexpr int kPopup = 0x21140F;
inline constexpr int kRowBorder = 0x3A2519;
inline constexpr int kPanelBorder = 0x4A2E20;
inline constexpr int kText = 0xF3E6CF;
inline constexpr int kDim = 0xB39B80;

inline constexpr int kWarning = 0x782E1A;
inline constexpr int kWarningDot = 0xFF734D;

inline constexpr int kSponsor = 0x8E2823;
inline constexpr int kSponsorHot = 0xA83630;
inline constexpr int kSponsorActive = 0x6E1F1B;
inline constexpr int kSponsorLink = 0xE7C27A;

inline constexpr int kHeaderEdge = 0x0C0705;
inline constexpr int kHeaderGlow = 0x4A2A14;
inline constexpr int kHeaderMid = 0x2E1A0E;
inline constexpr int kHeaderBottom = 0x070403;

} // namespace sos::palette
