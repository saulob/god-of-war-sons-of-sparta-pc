// Headless checks of the game cursor policy: the five second idle hide in fullscreen, focus and fullscreen transitions,
// mouse events of another window, and that the SDL cursor is touched only when the visibility changes. The clock is
// injected; nothing sleeps, and no window or SDL is used.
#include <cstdint>
#include <cstdio>
#include <string>

#include "dcp_cursor.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

constexpr std::uint32_t GameWindow = 7;
constexpr std::uint32_t OtherWindow = 9;

// What WindowSDL::UpdateCursor does with the state: one SDL call per reported change.
struct Cursor {
    DcpCursor::State state;
    bool visible = true;
    int sdl_calls = 0;

    void Tick(std::uint64_t now, bool fullscreen, bool focused = true, bool overlay = false, bool pointer = false) {
        if (const auto change = state.Update(true, focused, overlay, pointer, fullscreen, now)) {
            visible = *change;
            ++sdl_calls;
        }
    }
    void Move(std::uint64_t now, std::uint32_t window = GameWindow) { state.MouseMoved(window, GameWindow, now); }
};

void TestIdleHide() {
    Cursor c;
    c.Tick(0, true);
    Check(c.visible && c.sdl_calls == 0, "fullscreen at t0: cursor visible, no SDL call");
    c.Tick(4999, true);
    Check(c.visible, "fullscreen at 4999 ms: visible");
    c.Tick(5000, true);
    Check(!c.visible && c.sdl_calls == 1, "fullscreen at 5000 ms: hidden, one SDL call");

    c.Move(5200);
    c.Tick(5200, true);
    Check(c.visible && c.sdl_calls == 2, "mouse movement shows the cursor at once");
    c.Tick(10199, true);
    Check(c.visible, "5 s minus 1 ms after the movement: still visible");
    c.Tick(10200, true);
    Check(!c.visible && c.sdl_calls == 3, "5 s after the movement: hidden again");

    Cursor late;
    late.Tick(600000, true);
    late.Tick(604999, true);
    Check(late.visible, "a window already fullscreen late in the session starts a fresh delay, not an expired one");
    late.Tick(605000, true);
    Check(!late.visible, "and hides 5 s after the first update");

    Cursor steady;
    for (std::uint64_t t = 0; t < 60000; t += 1000) {
        steady.Tick(t, true);
        if (t % 3000 == 0) {
            steady.Move(t);
        }
    }
    Check(steady.visible, "movement every 3 s keeps the cursor visible");

    Cursor back;
    back.Tick(1000, true);
    back.Tick(500, true);
    Check(back.visible, "a clock that steps back never hides the cursor");
}

void TestWindowed() {
    Cursor c;
    for (std::uint64_t t = 0; t <= 120000; t += 250) {
        c.Tick(t, false);
    }
    Check(c.visible && c.sdl_calls == 0, "windowed stays visible for two minutes without movement, with no SDL call");

    Cursor toWindowed;
    toWindowed.Tick(0, true);
    toWindowed.Tick(5000, true);
    Check(!toWindowed.visible, "fullscreen idle: hidden");
    toWindowed.Tick(5001, false);
    Check(toWindowed.visible, "fullscreen to windowed shows the cursor at once");
    toWindowed.Tick(60000, false);
    Check(toWindowed.visible, "and windowed keeps it visible");
    toWindowed.Tick(60001, true);
    Check(toWindowed.visible, "windowed to fullscreen: visible, no stale hidden state");
    toWindowed.Tick(64999, true);
    Check(toWindowed.visible, "a fresh delay runs after entering fullscreen: 4999 ms later still visible");
    toWindowed.Tick(65001, true);
    Check(!toWindowed.visible, "and hides after it");
}

void TestFocus() {
    Cursor c;
    c.Tick(0, true);
    c.Tick(5000, true);
    Check(!c.visible, "hidden by the idle delay");
    c.Tick(5100, true, false);
    Check(c.visible, "focus loss shows the cursor at once");
    for (std::uint64_t t = 6000; t <= 9000; t += 1000) {
        c.Tick(t, true, false);
    }
    Check(c.visible, "it stays visible while unfocused, however long");
    c.Tick(9000, true, true);
    Check(c.visible, "focus returns: visible");
    c.Tick(13999, true);
    Check(c.visible, "focus returned 4999 ms ago: still visible");
    c.Tick(14000, true);
    Check(!c.visible, "focus returned 5000 ms ago: hidden, a fresh delay");

    Cursor overlay;
    overlay.Tick(0, true);
    overlay.Tick(4000, true, true, true);
    overlay.Tick(8000, true, true, true);
    Check(overlay.visible, "an open overlay keeps the cursor visible");
    overlay.Tick(8001, true);
    overlay.Tick(12000, true);
    Check(overlay.visible, "closing the overlay starts a fresh delay");
    overlay.Tick(13001, true);
    Check(!overlay.visible, "and the cursor hides after it");
}

void TestWindows() {
    Cursor c;
    c.Tick(0, true);
    c.Move(3000, OtherWindow);
    c.Tick(5000, true);
    Check(!c.visible, "motion in another window does not restart the timer");

    Cursor keyboard;
    keyboard.Tick(0, true);
    keyboard.state.KeyboardActivity();
    keyboard.Tick(1, true);
    Check(!keyboard.visible, "keyboard input hides the cursor, as before");
    keyboard.Move(2, OtherWindow);
    keyboard.Tick(3, true);
    Check(!keyboard.visible && keyboard.state.LastDevice() == DcpCursor::Device::Keyboard,
          "motion in another window does not make the mouse the last device");
    keyboard.Move(4);
    keyboard.Tick(4, true);
    Check(keyboard.visible && keyboard.state.LastDevice() == DcpCursor::Device::Mouse, "motion in the game window does");
}

void TestSdlCalls() {
    Cursor c;
    for (std::uint64_t t = 0; t < 4000; ++t) {
        c.Tick(t, true);
    }
    Check(c.sdl_calls == 0, "4000 unchanged updates before the delay: no SDL call");
    for (std::uint64_t t = 4000; t < 20000; ++t) {
        c.Tick(t, true);
    }
    Check(c.sdl_calls == 1 && !c.visible, "16000 more unchanged updates: exactly one SDL call, for the hide");
    for (std::uint64_t t = 20000; t < 24000; ++t) {
        c.Tick(t, false);
    }
    Check(c.sdl_calls == 2 && c.visible, "4000 windowed updates: exactly one more, for the show");
}

void TestExistingPolicy() {
    Cursor c;
    c.state.KeyboardActivity();
    c.Tick(0, false);
    Check(!c.visible, "windowed: keyboard as the last device hides the cursor, unchanged");
    c.Move(1);
    c.Tick(1, false);
    Check(c.visible, "windowed: the mouse brings it back");
    c.state.ControllerActivity();
    c.Tick(2, false);
    Check(!c.visible, "windowed: a controller as the last device hides it, unchanged");

    Cursor pointer;
    pointer.Tick(0, true, true, false, true);
    Check(!pointer.visible, "Pointer Control hides the host cursor");

    Cursor launcher;
    launcher.state.KeyboardActivity();
    const auto launcherChange = launcher.state.Update(false, true, false, false, true, 99999);
    Check(!launcherChange && launcher.visible, "outside the game window (launcher) the cursor stays visible");

    Cursor noMouseControls;
    noMouseControls.state.SetMouseControls(false);
    noMouseControls.state.KeyboardActivity();
    noMouseControls.Tick(0, true);
    noMouseControls.Move(1);
    noMouseControls.Tick(1, true);
    Check(!noMouseControls.visible, "with Mouse Controls off, motion is not a gameplay device");
    Cursor idleNoControls;
    idleNoControls.state.SetMouseControls(false);
    idleNoControls.Tick(0, true);
    idleNoControls.Move(4000);
    idleNoControls.Tick(4000, true);
    idleNoControls.Tick(8999, true);
    Check(idleNoControls.visible, "with Mouse Controls off, motion still restarts the idle delay");
    idleNoControls.Tick(9000, true);
    Check(!idleNoControls.visible, "and the delay then runs out");
}

} // namespace

int main() {
    TestIdleHide();
    TestWindowed();
    TestFocus();
    TestWindows();
    TestSdlCalls();
    TestExistingPolicy();
    std::printf("Cursor policy: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
