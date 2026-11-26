// Launcher input telemetry and the Gamescope gamepad policy, without SDL, so they can be tested headless.
//
// A Steam Deck report (SOS-VLHSWP) showed a launcher that ran normally and a session that ended with no PLAY: nothing
// said whether a tap, the Deck's buttons (Steam Input's virtual pad) or a key ever reached the window. Each kind of input
// event and each focus change is logged a few times, and the last input decides the "input=" of the PLAY line.
#pragma once

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace sos {

enum class InputKind { kNone, kMouse, kTouch, kKeyboard, kGamepad };

inline const char* InputKindName(InputKind kind) {
    switch (kind) {
    case InputKind::kNone: return "unknown";
    case InputKind::kMouse: return "mouse";
    case InputKind::kTouch: return "touchscreen";
    case InputKind::kKeyboard: return "keyboard";
    case InputKind::kGamepad: return "gamepad";
    }
    return "unknown";
}

class InputTelemetry {
public:
    static constexpr int kLinesPerEvent = 3;

    // The line to log for this event, or nothing once the event was logged kLinesPerEvent times. The kind (when not
    // kNone) becomes the last input.
    std::optional<std::string> Note(std::string_view event, std::string_view detail = {}, InputKind kind = InputKind::kNone) {
        if (kind != InputKind::kNone) {
            last_ = kind;
        }
        int& count = counts_[std::string(event)];
        if (++count > kLinesPerEvent) {
            return std::nullopt;
        }
        std::string line = "Launcher input: event=" + std::string(event);
        if (!detail.empty()) {
            line += " ";
            line += detail;
        }
        line += " count=" + std::to_string(count);
        if (count == kLinesPerEvent) {
            line += " (further ones not logged)";
        }
        return line;
    }

    InputKind last() const { return last_; }
    int count(std::string_view event) const {
        const auto found = counts_.find(std::string(event));
        return found == counts_.end() ? 0 : found->second;
    }

private:
    std::map<std::string, int> counts_;
    InputKind last_ = InputKind::kNone;
};

// Under Gamescope the launcher's X window may not hold keyboard focus, and SDL drops joystick and gamepad buttons while
// no window has it, so the Deck's Start and A (Steam Input's virtual pad) could never reach the launcher. The hint is set
// only there; SDL_Quit at launcher close resets it, so the game window keeps its own setting.
inline bool AllowBackgroundGamepadEvents(const char* gamescope_wayland_display, const char* current_desktop) {
    if (gamescope_wayland_display && *gamescope_wayland_display) {
        return true;
    }
    return current_desktop && std::strstr(current_desktop, "gamescope") != nullptr;
}

// The footer hint names every way to press PLAY, including a controller's Start (TRG maps it to PLAY).
inline constexpr const char kLauncherHints[] = "Enter / Start  Play     Alt+F4  Quit     Ctrl+S  Save";

} // namespace sos
