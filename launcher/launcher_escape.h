#pragma once

#include <cstring>
#include <string>

#include "launcher_quit.h"

namespace sos {

// One Escape press per frame, claimed by at most one owner. The launcher never closes on Escape, so a quit request that
// coincides with a press is dropped whoever claimed it.
class EscapeArbiter {
public:
    void BeginFrame(bool pressed) {
        pressed_ = pressed;
        taken_ = false;
    }

    bool Pressed() const { return pressed_; }
    bool Taken() const { return taken_; }

    bool Take() {
        if (!pressed_ || taken_) {
            return false;
        }
        taken_ = true;
        return true;
    }

    bool QuitAllowed() const { return LauncherQuitAllowed(pressed_); }

private:
    bool pressed_ = false;
    bool taken_ = false;
};

inline EscapeArbiter& LauncherEscape() {
    static EscapeArbiter arbiter;
    return arbiter;
}

// Dear ImGui reverts a text field to its text from when it was activated on Escape. The text typed before the press is
// kept: `backup` follows the buffer and is written back when Escape ended an edit. True when it restored the text.
inline bool KeepTextOnEscape(char* buffer, size_t capacity, std::string& backup, bool escape_pressed, bool was_active) {
    if (escape_pressed && was_active) {
        if (backup == buffer) {
            return false;
        }
        std::strncpy(buffer, backup.c_str(), capacity - 1);
        buffer[capacity - 1] = '\0';
        return true;
    }
    if (backup != buffer) {
        backup = buffer;
    }
    return false;
}

} // namespace sos
