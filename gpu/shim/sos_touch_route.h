// SPDX-License-Identifier: GPL-2.0-or-later
// Which SDL events belong to a touchscreen, for the Gamescope touch route (src/sos_touch_gamescope.h). Window thread.
#pragma once

#include <SDL3/SDL.h>

namespace SosTouchRoute {

// SDL repeats every touch as mouse input marked SDL_TOUCH_MOUSEID; while the route is on, the finger path has it.
inline bool TouchGeneratedMouse(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.which == SDL_TOUCH_MOUSEID;
    default:
        return false;
    }
}

inline bool IsFingerEvent(const SDL_Event& event) {
    return event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
           event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
}

// Only a screen the finger touches directly; trackpads and SDL's mouse- or pen-made touches stay out.
inline bool AcceptedTouch(SDL_TouchID touch, SDL_TouchDeviceType type) {
    return touch != SDL_MOUSE_TOUCHID && touch != SDL_PEN_TOUCHID && type == SDL_TOUCH_DEVICE_DIRECT;
}

// What the window does with one event while the route is `routed`: a finger goes to the touchpad, a touch-made mouse
// event is dropped for the game (the overlay still sees it), everything else is unchanged. Off the route nothing changes.
enum class Use { Unchanged, Finger, DropMouse };

inline Use Classify(const SDL_Event& event, bool routed) {
    if (!routed) {
        return Use::Unchanged;
    }
    if (IsFingerEvent(event)) {
        return Use::Finger;
    }
    return TouchGeneratedMouse(event) ? Use::DropMouse : Use::Unchanged;
}

} // namespace SosTouchRoute
