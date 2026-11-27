// The input rules the in-game menus (Cheats, Options) share: when the game gets its input back after a menu closes,
// the L3 + R3 chord, and stepping through rows, pages and choices. Logic only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace sos::menu {

// A button that is never reported released (a controller unplugged as the menu closes) cannot block the game forever.
inline constexpr double kReleaseTimeoutSeconds = 1.0;

// Whether the menu may release the game's input: nothing it received is still held, or the wait ran out.
inline bool InputReleased(bool anything_held, double waited_seconds, double timeout_seconds) {
    return !anything_held || waited_seconds >= timeout_seconds;
}

// Closing the menu (B, Escape, its key or chord) starts a wait, and the game gets its input back only after what was
// pressed in the menu is released, so the closing press never also reaches the game.
class CloseGate {
public:
    void Request() { requested_ = true; }
    bool requested() const { return requested_; }

    // Once per frame. True when the menu may go and the game may read its input again.
    bool Step(bool anything_held, double now) {
        if (!requested_) {
            return false;
        }
        if (since_ < 0.0) {
            since_ = now;
        }
        return InputReleased(anything_held, now - since_, kReleaseTimeoutSeconds);
    }

private:
    bool requested_ = false;
    double since_ = -1.0;
};

// True once per press of every button in `mask` together. After a press, all of them must be let go before the next
// one counts, so a held chord never repeats and a half-release does not re-arm it.
class Chord {
public:
    explicit Chord(uint32_t mask) : mask_(mask) {}

    bool Poll(uint32_t buttons) {
        const uint32_t held = buttons & mask_;
        const bool pressed = armed_ && held == mask_;
        if (pressed) {
            armed_ = false;
        } else if (held == 0) {
            armed_ = true;
        }
        return pressed;
    }

private:
    uint32_t mask_;
    bool armed_ = true;
};

// A menu's controller buttons, whatever controller they come from.
inline constexpr uint32_t kPadUp = 1, kPadDown = 2, kPadActivate = 4, kPadClose = 8;
inline constexpr uint32_t kPadMenuMask = kPadUp | kPadDown | kPadActivate | kPadClose;
inline constexpr double kPadRepeatDelay = 0.4;
inline constexpr double kPadRepeatInterval = 0.12;

struct NavStep {
    int move = 0; // -1 up, 1 down
    bool activate = false;
    bool close = false;
};

// Steps from the controller's buttons, polled once per frame. A button held when the menu opened is ignored until it is
// released; a press acts once, and holding up or down repeats after a delay at a steady rate.
class PadNav {
public:
    NavStep Poll(uint32_t buttons, double now) {
        if (!started_) {
            started_ = true;
            ignored_ = buttons & kPadMenuMask;
        }
        ignored_ &= buttons;
        const uint32_t current = buttons & kPadMenuMask & ~ignored_;
        const uint32_t pressed = current & ~previous_;
        previous_ = current;
        NavStep step;
        step.move = (pressed & kPadUp) ? -1 : (pressed & kPadDown) ? 1 : 0;
        if (step.move) {
            repeat_at_ = now + kPadRepeatDelay;
        } else if ((current & (kPadUp | kPadDown)) && now >= repeat_at_) {
            step.move = (current & kPadUp) ? -1 : 1;
            repeat_at_ = now + kPadRepeatInterval;
        }
        step.activate = (pressed & kPadActivate) != 0;
        step.close = (pressed & kPadClose) != 0;
        return step;
    }

    // The menu buttons pressed since it opened and not yet released.
    uint32_t Held() const { return previous_; }

private:
    bool started_ = false;
    uint32_t ignored_ = 0, previous_ = 0;
    double repeat_at_ = 0.0;
};

enum class ToggleAction { None, Open, Close };

// A menu's key or chord: a held key repeats and only the first press counts. `busy` is another menu or a game dialog
// on screen, which the menu never opens over.
constexpr ToggleAction TogglePress(bool repeat, bool open, bool busy) {
    if (repeat) {
        return ToggleAction::None;
    }
    if (open) {
        return ToggleAction::Close;
    }
    return busy ? ToggleAction::None : ToggleAction::Open;
}

// The index one step away, wrapping around.
constexpr int WrapIndex(int index, int count, int step) {
    return count <= 0 ? 0 : ((index + step) % count + count) % count;
}

// One step from `current` (-1 or 1), stopping at the ends. A current value that is not a choice moves to the first.
constexpr int StepChoice(int current, int count, int step) {
    if (count <= 0) {
        return -1;
    }
    if (current < 0 || current >= count) {
        return 0;
    }
    return std::clamp(current + step, 0, count - 1);
}

// The next choice, wrapping around, so a toggle flips.
constexpr int NextChoiceWrapping(int current, int count) {
    if (count <= 0) {
        return -1;
    }
    return current < 0 || current + 1 >= count ? 0 : current + 1;
}

// The first visible row of a list of `count` rows, `visible` of them shown from `first`, that keeps `selected` shown.
constexpr int ScrollToShow(int selected, int first, int visible, int count) {
    if (visible <= 0 || count <= visible) {
        return 0;
    }
    if (selected < first) {
        first = selected;
    } else if (selected >= first + visible) {
        first = selected - visible + 1;
    }
    return std::clamp(first, 0, count - visible);
}

} // namespace sos::menu
