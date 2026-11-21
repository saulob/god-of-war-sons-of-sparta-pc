// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>

namespace DcpCursor {

enum class Device { Mouse, Keyboard, Controller };

class State {
public:
    std::optional<bool> Update(bool gameplay, bool focused, bool overlay, bool pointer = false) {
        if (!gameplay) {
            device_ = Device::Mouse;
        }
        const bool visible = !gameplay || !focused || overlay || (device_ == Device::Mouse && !pointer);
        if (visible == visible_) {
            return std::nullopt;
        }
        visible_ = visible;
        return visible;
    }

    void SetMouseControls(bool on) { mouse_controls_ = on; }

    void MouseMoved(std::uint32_t event_window, std::uint32_t game_window, float dx, float dy) {
        if (event_window != game_window || (dx == 0.0f && dy == 0.0f)) {
            return;
        }
        MouseActivity();
    }

    void MouseButtonPressed(std::uint32_t event_window, std::uint32_t game_window) {
        if (event_window != game_window) {
            return;
        }
        MouseActivity();
    }

    void MouseWheelMoved(std::uint32_t event_window, std::uint32_t game_window, float dx, float dy) {
        if (event_window != game_window || (dx == 0.0f && dy == 0.0f)) {
            return;
        }
        MouseActivity();
    }

    void KeyboardActivity() { device_ = Device::Keyboard; }
    void ControllerActivity() { device_ = Device::Controller; }

    Device LastDevice() const { return device_; }
    bool Visible() const { return visible_; }

private:
    void MouseActivity() {
        if (mouse_controls_) {
            device_ = Device::Mouse;
        }
    }

    bool mouse_controls_ = true;
    Device device_ = Device::Mouse;
    bool visible_ = true;
};

constexpr bool IsGameplayKey(bool repeat, bool host_shortcut) {
    return !repeat && !host_shortcut;
}

// Stick engage/release thresholds in SDL's raw -32768..32767 axis units; hysteresis prevents edge jitter.
inline constexpr int kStickDeadzone = 7849;
inline constexpr int kStickReleaseDeadzone = 5500;
// Trigger engage/release thresholds in 0..255 units (raw SDL 0..32767 / 128).
inline constexpr int kTriggerThreshold = 30;
inline constexpr int kTriggerReleaseThreshold = 15;

struct ControllerState {
    std::uint32_t buttons = 0;
    int left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    int left_trigger = 0, right_trigger = 0;
};

class ControllerActivityFilter {
public:
    bool Update(const ControllerState& state) {
        bool started = (state.buttons & ~buttons_) != 0;
        buttons_ = state.buttons;
        started |= StickCrossed(state.left_x, left_x_);
        started |= StickCrossed(state.left_y, left_y_);
        started |= StickCrossed(state.right_x, right_x_);
        started |= StickCrossed(state.right_y, right_y_);
        started |= TriggerCrossed(state.left_trigger, left_trigger_);
        started |= TriggerCrossed(state.right_trigger, right_trigger_);
        return started;
    }

private:
    static bool StickCrossed(int value, bool& was_active) {
        const int magnitude = std::abs(value);
        if (!was_active) {
            if (magnitude >= kStickDeadzone) {
                was_active = true;
                return true;
            }
            return false;
        }
        if (magnitude < kStickReleaseDeadzone) {
            was_active = false;
        }
        return false;
    }

    static bool TriggerCrossed(int value, bool& was_active) {
        const int level = value > 0 ? value / 128 : 0;
        if (!was_active) {
            if (level >= kTriggerThreshold) {
                was_active = true;
                return true;
            }
            return false;
        }
        if (level < kTriggerReleaseThreshold) {
            was_active = false;
        }
        return false;
    }

    std::uint32_t buttons_ = 0;
    bool left_x_ = false, left_y_ = false, right_x_ = false, right_y_ = false;
    bool left_trigger_ = false, right_trigger_ = false;
};

} // namespace DcpCursor
