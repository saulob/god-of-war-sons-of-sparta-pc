// Headless tests of the input-aware cursor policy (gpu/shim/dcp_cursor.h): mouse/keyboard/controller transitions,
// stick and trigger deadzones with hysteresis, overlay visibility override and post-close restoration, focus changes,
// windowed/borderless parity, and SDL cursor call deduplication.
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
constexpr std::uint32_t kCross = 1u << 0;
constexpr std::uint32_t kCircle = 1u << 1;
constexpr std::uint32_t kDpadUp = 1u << 2;

int TriggerRaw(int units) {
    return units * 128;
}

struct Cursor {
    DcpCursor::State state;
    DcpCursor::ControllerActivityFilter pad_filter;
    DcpCursor::ControllerState pad_state;
    bool visible = true;
    int sdl_calls = 0;

    void Tick(bool focused = true, bool overlay = false, bool pointer = false, bool gameplay = true) {
        if (const auto change = state.Update(gameplay, focused, overlay, pointer)) {
            visible = *change;
            ++sdl_calls;
        }
    }

    void Move(float dx = 4.0f, float dy = 0.0f, std::uint32_t window = GameWindow) {
        state.MouseMoved(window, GameWindow, dx, dy);
    }

    void Click(std::uint32_t window = GameWindow) {
        state.MouseButtonPressed(window, GameWindow);
    }

    void Wheel(float dx = 0.0f, float dy = 1.0f, std::uint32_t window = GameWindow) {
        state.MouseWheelMoved(window, GameWindow, dx, dy);
    }

    void Key(bool repeat = false, bool host_shortcut = false) {
        if (DcpCursor::IsGameplayKey(repeat, host_shortcut)) {
            state.KeyboardActivity();
        }
    }

    bool Pad(const DcpCursor::ControllerState& next) {
        pad_state = next;
        const bool active = pad_filter.Update(pad_state);
        if (active) {
            state.ControllerActivity();
        }
        return active;
    }
};

void TestInitialState() {
    Cursor c;
    Check(c.state.Visible(), "initial state starts visible");
    Check(c.state.LastDevice() == DcpCursor::Device::Mouse, "initial last device is mouse");
    c.Tick();
    Check(c.visible && c.sdl_calls == 0, "first focused update keeps visible with zero SDL calls");
}

void TestMouseActivity() {
    Cursor c;
    c.Key();
    c.Tick();
    Check(!c.visible && c.sdl_calls == 1, "keyboard input hides cursor");

    c.Move(0.0f, 0.0f);
    c.Tick();
    Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Keyboard,
          "synthetic zero-delta mouse motion does not show cursor");

    c.Move(5.0f, -2.0f, OtherWindow);
    c.Tick();
    Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Keyboard,
          "mouse motion in another window does not alter game cursor state");

    c.Click(OtherWindow);
    c.Tick();
    Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Keyboard,
          "mouse button in another window does not alter game cursor state");

    c.Wheel(0.0f, 1.0f, OtherWindow);
    c.Tick();
    Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Keyboard,
          "mouse wheel in another window does not alter game cursor state");

    c.Wheel(0.0f, 0.0f);
    c.Tick();
    Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Keyboard,
          "zero-delta mouse wheel does not show cursor");

    c.Move(1.0f, 0.0f);
    c.Tick();
    Check(c.visible && c.sdl_calls == 2 && c.state.LastDevice() == DcpCursor::Device::Mouse,
          "meaningful mouse motion in game window shows cursor");

    c.Key();
    c.Tick();
    c.Click();
    c.Tick();
    Check(c.visible && c.state.LastDevice() == DcpCursor::Device::Mouse, "mouse button press shows cursor");

    c.Key();
    c.Tick();
    c.Wheel(0.0f, -1.0f);
    c.Tick();
    Check(c.visible && c.state.LastDevice() == DcpCursor::Device::Mouse, "mouse wheel shows cursor");

    const int before = c.sdl_calls;
    for (int i = 0; i < 10000; ++i) {
        c.Tick();
    }
    Check(c.visible && c.sdl_calls == before, "mouse stays visible indefinitely with no idle timeout");
}

void TestKeyboardActivity() {
    Check(DcpCursor::IsGameplayKey(false, false), "fresh non-shortcut key counts as gameplay input");
    Check(!DcpCursor::IsGameplayKey(true, false), "key repeat does not count as gameplay input");
    Check(!DcpCursor::IsGameplayKey(false, true), "host shortcut does not count as gameplay input");
    Check(!DcpCursor::IsGameplayKey(true, true), "repeated host shortcut does not count as gameplay input");

    Cursor c;
    c.Key(true, false);
    c.Tick();
    Check(c.visible && c.sdl_calls == 0, "synthetic/repeat key event does not hide cursor");
    c.Key(false, true);
    c.Tick();
    Check(c.visible && c.sdl_calls == 0, "host shortcut key does not hide cursor");
    c.Key(false, false);
    c.Tick();
    Check(!c.visible && c.sdl_calls == 1 && c.state.LastDevice() == DcpCursor::Device::Keyboard,
          "gameplay key hides cursor");
}

void TestControllerButtons() {
    Cursor c;
    DcpCursor::ControllerState pad;
    pad.buttons = kCross;
    Check(c.Pad(pad), "fresh controller button press reports activity");
    c.Tick();
    Check(!c.visible && c.sdl_calls == 1, "controller button hides cursor");

    Check(!c.Pad(pad), "held controller button does not re-trigger activity");
    c.Move();
    c.Tick();
    Check(c.visible && c.sdl_calls == 2, "mouse movement shows cursor while button stays held");
    Check(!c.Pad(pad), "still-held button does not override mouse");
    c.Tick();
    Check(c.visible && c.sdl_calls == 2, "cursor stays visible while button remains held");

    pad.buttons |= kCircle;
    Check(c.Pad(pad), "second button press triggers controller activity");
    c.Tick();
    Check(!c.visible && c.sdl_calls == 3, "second button hides cursor");

    pad.buttons = 0;
    Check(!c.Pad(pad), "releasing buttons does not report controller activity");
    c.Tick();
    Check(!c.visible, "releasing buttons does not switch back to mouse");

    pad.buttons = kDpadUp;
    Check(c.Pad(pad), "d-pad press triggers controller activity");

    DcpCursor::ControllerActivityFilter a, b;
    DcpCursor::ControllerState sa, sb;
    sa.buttons = kCross;
    sb.buttons = kCircle;
    Check(a.Update(sa) && !a.Update(sa), "first controller filter tracks edges independently");
    Check(b.Update(sb) && !b.Update(sb), "second controller filter tracks edges independently");
}

void TestStickDeadzoneAndHysteresis() {
    Cursor c;
    DcpCursor::ControllerState pad;

    pad.left_x = DcpCursor::kStickDeadzone - 1;
    Check(!c.Pad(pad), "stick just below positive deadzone does not switch mode");
    pad.left_x = -(DcpCursor::kStickDeadzone - 1);
    Check(!c.Pad(pad), "stick just below negative deadzone does not switch mode");
    c.Tick();
    Check(c.visible && c.sdl_calls == 0, "sub-deadzone stick drift keeps cursor visible");

    pad.left_x = DcpCursor::kStickDeadzone;
    Check(c.Pad(pad), "stick crossing deadzone switches to controller");
    c.Tick();
    Check(!c.visible && c.sdl_calls == 1, "crossing stick deadzone hides cursor once");

    c.Move();
    c.Tick();
    Check(c.visible && c.sdl_calls == 2, "mouse shows cursor while stick is held");
    for (int v : {7999, 8001, 7998, 8002, DcpCursor::kStickDeadzone - 1, DcpCursor::kStickDeadzone,
                  DcpCursor::kStickReleaseDeadzone, DcpCursor::kStickDeadzone + 100}) {
        pad.left_x = v;
        Check(!c.Pad(pad), "threshold-edge stick noise inside hysteresis band does not switch mode");
    }
    c.Tick();
    Check(c.visible && c.sdl_calls == 2, "edge jitter leaves cursor visible without extra SDL calls");

    pad.left_x = 0;
    Check(!c.Pad(pad), "returning stick to center resets filter without firing controller activity");
    c.Tick();
    Check(c.visible && c.state.LastDevice() == DcpCursor::Device::Mouse,
          "returning stick through center does not change device");

    pad.left_x = -DcpCursor::kStickDeadzone;
    Check(c.Pad(pad), "new push across negative deadzone after return to center switches to controller");
    c.Tick();
    Check(!c.visible && c.sdl_calls == 3, "new meaningful stick push hides cursor");

    pad.left_x = 0;
    c.Pad(pad);
    c.Tick();
    Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Controller,
          "returning stick through center does not itself switch to mouse");

    pad.right_y = DcpCursor::kStickDeadzone;
    Check(c.Pad(pad), "right stick crossing deadzone reports controller activity");
    pad.right_y = 0;
    c.Pad(pad);
    for (int v : {0, 1200, -2500, 4000, -4000, 0}) {
        pad.left_y = v;
        pad.right_x = -v;
        Check(!c.Pad(pad), "axis noise around neutral does not report activity");
    }
}

void TestTriggerDeadzoneAndHysteresis() {
    Cursor c;
    DcpCursor::ControllerState pad;

    pad.left_trigger = -100;
    Check(!c.Pad(pad), "negative trigger value is ignored");
    pad.left_trigger = TriggerRaw(DcpCursor::kTriggerThreshold) - 1;
    Check(!c.Pad(pad), "resting trigger noise below threshold does not switch mode");
    c.Tick();
    Check(c.visible && c.sdl_calls == 0, "trigger noise keeps cursor visible");

    pad.left_trigger = TriggerRaw(DcpCursor::kTriggerThreshold);
    Check(c.Pad(pad), "trigger crossing threshold switches to controller");
    c.Tick();
    Check(!c.visible && c.sdl_calls == 1, "meaningful trigger hides cursor");

    c.Move();
    c.Tick();
    Check(c.visible && c.sdl_calls == 2, "mouse shows cursor while trigger is held");
    for (int units : {DcpCursor::kTriggerThreshold - 1, DcpCursor::kTriggerThreshold,
                      DcpCursor::kTriggerReleaseThreshold, DcpCursor::kTriggerThreshold + 5}) {
        pad.left_trigger = TriggerRaw(units);
        Check(!c.Pad(pad), "trigger jitter inside hysteresis band does not re-trigger");
    }
    c.Tick();
    Check(c.visible && c.sdl_calls == 2, "trigger jitter leaves cursor visible");

    pad.left_trigger = TriggerRaw(DcpCursor::kTriggerReleaseThreshold - 1);
    Check(!c.Pad(pad), "releasing trigger below release threshold re-arms filter");
    pad.left_trigger = TriggerRaw(DcpCursor::kTriggerThreshold);
    Check(c.Pad(pad), "next meaningful trigger pull switches to controller");

    pad.left_trigger = 0;
    c.Pad(pad);
    pad.right_trigger = TriggerRaw(DcpCursor::kTriggerThreshold);
    Check(c.Pad(pad), "right trigger crossing threshold reports controller activity");
}

void TestOverlayBehavior() {
    Cursor fromMouse;
    fromMouse.Move();
    fromMouse.Tick(true, false);
    Check(fromMouse.visible, "overlay from mouse: starts visible");
    fromMouse.Tick(true, true);
    Check(fromMouse.visible, "overlay open from mouse: stays visible");
    fromMouse.Tick(true, false);
    Check(fromMouse.visible, "overlay close restores mouse-visible state");

    Cursor fromKeyboard;
    fromKeyboard.Key();
    fromKeyboard.Tick(true, false);
    Check(!fromKeyboard.visible, "overlay from keyboard: hidden before open");
    fromKeyboard.Tick(true, true);
    Check(fromKeyboard.visible, "overlay open forces visible from keyboard state");
    fromKeyboard.Key();
    fromKeyboard.Tick(true, true);
    Check(fromKeyboard.visible, "overlay keyboard navigation does not defeat overlay visibility override");
    fromKeyboard.Tick(true, false);
    Check(!fromKeyboard.visible, "overlay close restores keyboard-hidden state");

    Cursor fromController;
    DcpCursor::ControllerState pad;
    pad.buttons = kCross;
    fromController.Pad(pad);
    fromController.Tick(true, false);
    Check(!fromController.visible, "overlay from controller: hidden before open");
    fromController.Tick(true, true);
    Check(fromController.visible, "overlay open forces visible from controller state");
    fromController.Tick(true, false);
    Check(!fromController.visible, "overlay close restores controller-hidden state");
}

void TestEventOrderSequences() {
    // Sequence A: mouse move -> controller button -> mouse move => visible, hidden, visible
    {
        Cursor c;
        c.Move();
        c.Tick();
        Check(c.visible, "Sequence A step 1 (mouse move): visible");
        DcpCursor::ControllerState pad;
        pad.buttons = kCross;
        c.Pad(pad);
        c.Tick();
        Check(!c.visible, "Sequence A step 2 (controller button): hidden");
        c.Move();
        c.Tick();
        Check(c.visible, "Sequence A step 3 (mouse move): visible");
    }

    // Sequence B: controller input -> overlay open -> mouse move -> overlay close => hidden, visible, visible, visible
    {
        Cursor c;
        DcpCursor::ControllerState pad;
        pad.buttons = kCross;
        c.Pad(pad);
        c.Tick(true, false);
        Check(!c.visible, "Sequence B step 1 (controller input): hidden");
        c.Tick(true, true);
        Check(c.visible, "Sequence B step 2 (overlay open): visible");
        c.Move();
        c.Tick(true, true);
        Check(c.visible, "Sequence B step 3 (mouse move in overlay): visible");
        c.Tick(true, false);
        Check(c.visible && c.state.LastDevice() == DcpCursor::Device::Mouse,
              "Sequence B step 4 (overlay close): stays visible");
    }

    // Sequence C: mouse move -> overlay open -> controller input -> overlay close => visible, visible, visible, hidden
    {
        Cursor c;
        c.Move();
        c.Tick(true, false);
        Check(c.visible, "Sequence C step 1 (mouse move): visible");
        c.Tick(true, true);
        Check(c.visible, "Sequence C step 2 (overlay open): visible");
        DcpCursor::ControllerState pad;
        pad.buttons = kCross;
        c.Pad(pad);
        c.Tick(true, true);
        Check(c.visible, "Sequence C step 3 (controller input in overlay): visible while overlay open");
        c.Tick(true, false);
        Check(!c.visible && c.state.LastDevice() == DcpCursor::Device::Controller,
              "Sequence C step 4 (overlay close): hidden after close");
    }

    // Sequence D: mouse move -> small stick drift -> small trigger drift => remains visible
    {
        Cursor c;
        c.Move();
        c.Tick();
        DcpCursor::ControllerState pad;
        pad.left_x = 1800;
        pad.left_y = -2400;
        pad.right_x = 3000;
        c.Pad(pad);
        c.Tick();
        pad.left_trigger = TriggerRaw(5);
        pad.right_trigger = TriggerRaw(DcpCursor::kTriggerThreshold - 1);
        c.Pad(pad);
        c.Tick();
        Check(c.visible && c.sdl_calls == 0 && c.state.LastDevice() == DcpCursor::Device::Mouse,
              "Sequence D (mouse + stick drift + trigger drift): cursor remains visible");
    }

    // Sequence E: mouse move -> meaningful stick input => hidden exactly once
    {
        Cursor c;
        c.Move();
        c.Tick();
        DcpCursor::ControllerState pad;
        pad.left_x = DcpCursor::kStickDeadzone + 1000;
        c.Pad(pad);
        c.Tick();
        for (int i = 0; i < 50; ++i) {
            pad.left_x = DcpCursor::kStickDeadzone + 1000 + (i % 500);
            c.Pad(pad);
            c.Tick();
        }
        Check(!c.visible && c.sdl_calls == 1, "Sequence E (mouse -> meaningful stick): hidden exactly once");
    }
}

void TestFocusBehavior() {
    Cursor c;
    DcpCursor::ControllerState pad;
    pad.buttons = kCross;
    c.Pad(pad);
    c.Tick(true, false);
    Check(!c.visible && c.sdl_calls == 1, "focus test: controller hides cursor");

    c.Tick(false, false);
    Check(c.visible && c.sdl_calls == 2 && c.state.LastDevice() == DcpCursor::Device::Controller,
          "focus loss shows cursor without synthesizing mouse input");

    c.Tick(true, false);
    Check(!c.visible && c.sdl_calls == 3 && c.state.LastDevice() == DcpCursor::Device::Controller,
          "focus regain deterministically restores controller-hidden state");

    c.Tick(false, true);
    Check(c.visible, "focus loss with overlay keeps cursor visible");
    c.Tick(true, true);
    Check(c.visible, "focus regain with overlay keeps cursor visible");
    c.Tick(true, false);
    Check(!c.visible, "closing overlay after focus regain restores controller-hidden state");

    Cursor mouseFocus;
    mouseFocus.Move();
    mouseFocus.Tick(true, false);
    mouseFocus.Tick(false, false);
    mouseFocus.Tick(true, false);
    Check(mouseFocus.visible && mouseFocus.sdl_calls == 0,
          "focus loss and regain in mouse mode keep cursor visible with zero SDL calls");
}

void TestWindowedBorderlessAndLauncher() {
    // Windowed and Borderless share the same input-aware policy; mouse stays visible forever in both.
    Cursor windowed;
    windowed.Tick();
    for (int i = 0; i < 5000; ++i) {
        windowed.Tick();
    }
    Check(windowed.visible && windowed.sdl_calls == 0, "windowed: mouse stays visible with no SDL calls");
    windowed.Key();
    windowed.Tick();
    Check(!windowed.visible, "windowed: keyboard hides cursor while focused");
    windowed.Tick(false, false);
    Check(windowed.visible, "windowed: focus loss restores cursor");
    windowed.Tick(true, false);
    windowed.Move();
    windowed.Tick();
    Check(windowed.visible, "windowed: mouse restores cursor");

    Cursor borderless;
    DcpCursor::ControllerState pad;
    pad.buttons = kCross;
    borderless.Pad(pad);
    borderless.Tick();
    Check(!borderless.visible, "borderless/fullscreen: controller hides cursor");
    borderless.Move();
    borderless.Tick();
    Check(borderless.visible, "borderless/fullscreen: mouse restores cursor");

    Cursor launcher;
    launcher.Key();
    launcher.Tick(true, false, false, false);
    Check(launcher.visible && launcher.sdl_calls == 0 && launcher.state.LastDevice() == DcpCursor::Device::Mouse,
          "non-gameplay (launcher): cursor always stays visible and resets device to mouse");
    launcher.Tick(true, false, true, false);
    Check(launcher.visible && launcher.sdl_calls == 0,
          "non-gameplay (launcher): cursor stays visible even if pointer flag is set");

    Cursor pointer;
    pointer.Tick(true, false, true, true);
    Check(!pointer.visible, "pointer capture hides host cursor during gameplay");
    pointer.Tick(true, true, true, true);
    Check(pointer.visible, "overlay overrides pointer capture");

    Cursor noMouseControls;
    noMouseControls.state.SetMouseControls(false);
    noMouseControls.Key();
    noMouseControls.Tick();
    noMouseControls.Move();
    noMouseControls.Tick();
    Check(!noMouseControls.visible, "Mouse Controls off prevents mouse from taking gameplay device");
}

void TestApiDeduplication() {
    Cursor c;
    for (int i = 0; i < 2000; ++i) {
        c.Move(2.0f, 1.0f);
        c.Tick();
    }
    Check(c.visible && c.sdl_calls == 0, "repeated mouse events while already visible make zero SDL calls");

    DcpCursor::ControllerState pad;
    pad.buttons = kCross;
    c.Pad(pad);
    c.Tick();
    Check(!c.visible && c.sdl_calls == 1, "first controller transition makes exactly one SDL hide call");

    for (int i = 0; i < 2000; ++i) {
        pad.left_x = (i % 2 == 0) ? 20000 : 25000;
        c.Pad(pad);
        c.Tick();
    }
    Check(!c.visible && c.sdl_calls == 1, "repeated controller events while already hidden make zero extra SDL calls");

    for (int i = 0; i < 2000; ++i) {
        c.Move(3.0f, 0.0f);
        c.Tick();
    }
    Check(c.visible && c.sdl_calls == 2, "transition back to visible makes exactly one SDL show call");
}

} // namespace

int main() {
    TestInitialState();
    TestMouseActivity();
    TestKeyboardActivity();
    TestControllerButtons();
    TestStickDeadzoneAndHysteresis();
    TestTriggerDeadzoneAndHysteresis();
    TestOverlayBehavior();
    TestEventOrderSequences();
    TestFocusBehavior();
    TestWindowedBorderlessAndLauncher();
    TestApiDeduplication();
    std::printf("Cursor policy: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
