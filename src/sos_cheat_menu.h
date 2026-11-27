// The F10 cheat menu's model: its rows in order, what each row says, and how the selection moves past the headings.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

#include "runtime_cheats.h"
#include "sos_cheats.h"
#include "sos_menu_input.h"

namespace sos::cheats {

inline constexpr char kMenuTitle[] = "CHEATS";
inline constexpr char kMenuCloseHint[] = "F10, Esc or B Close";
inline constexpr char kMenuNote[] = "This session only, not saved";

enum class RowKind { Heading, Toggle, Multiplier, Action };

struct MenuRow {
    RowKind kind;
    const char* label;
    Toggle toggle;
    Multiplier multiplier;
    Action action;
};

constexpr MenuRow HeadingRow(const char* text) { return {RowKind::Heading, text, {}, {}, {}}; }
constexpr MenuRow ToggleRow(Toggle toggle) { return {RowKind::Toggle, Info(toggle).label, toggle, {}, {}}; }
constexpr MenuRow MultiplierRow(Multiplier multiplier) {
    return {RowKind::Multiplier, Info(multiplier).label, {}, multiplier, {}};
}
constexpr MenuRow ActionRow(Action action) { return {RowKind::Action, Info(action).label, {}, {}, action}; }

inline constexpr std::array<MenuRow, 12> kMenuRows = {
    HeadingRow("PLAYER"),
    ToggleRow(Toggle::GodMode),
    ToggleRow(Toggle::InfiniteSpirit),
    ToggleRow(Toggle::InfiniteMagic),
    HeadingRow("COMBAT"),
    MultiplierRow(Multiplier::Damage),
    HeadingRow("MOVEMENT"),
    MultiplierRow(Multiplier::MovementSpeed),
    MultiplierRow(Multiplier::JumpHeight),
    HeadingRow("ITEMS"),
    ActionRow(Action::AddBloodOrbs),
    ActionRow(Action::AddUpgradeMaterials),
};

constexpr bool Selectable(const MenuRow& row) { return row.kind != RowKind::Heading; }

// The first selectable row at or after `from` in direction `step`, or `from` itself when there is none.
constexpr int SelectableFrom(int from, int step) {
    const int count = static_cast<int>(kMenuRows.size());
    for (int at = from; at >= 0 && at < count; at += step) {
        if (Selectable(kMenuRows[static_cast<size_t>(at)])) {
            return at;
        }
    }
    return from;
}

// The selected row: always a selectable one, stopping at the ends. A keyboard or controller step asks for it to be
// scrolled into view; a click does not move the scroll.
struct MenuList {
    int selected = SelectableFrom(0, 1);
    bool reveal = true;

    void Open() {
        selected = SelectableFrom(0, 1);
        reveal = true;
    }

    void Step(int step) {
        const int next = SelectableFrom(selected + step, step);
        if (next >= 0 && next < static_cast<int>(kMenuRows.size()) && Selectable(kMenuRows[static_cast<size_t>(next)])) {
            selected = next;
        }
        reveal = true;
    }

    bool TakeReveal() {
        const bool wanted = reveal;
        reveal = false;
        return wanted;
    }

    void Click(int row) {
        if (row >= 0 && row < static_cast<int>(kMenuRows.size()) && Selectable(kMenuRows[static_cast<size_t>(row)])) {
            selected = row;
        }
    }
};

// What a row shows on its right, from the runtime state of its cheat.
enum class RowState { Unavailable, Off, On, Pending, Failed };

constexpr const char* StateText(RowState state, const char* on_text) {
    switch (state) {
    case RowState::Unavailable: return kUnavailable;
    case RowState::Off: return "Off";
    case RowState::On: return on_text;
    case RowState::Pending: return kPending;
    case RowState::Failed: return kFailed;
    }
    return kUnavailable;
}

struct Box {
    float x0, y0, x1, y1;
};

// The panel's top: below the frame counter when it is shown in the panel's column at the top of the screen, so the two
// never overlap, else the normal margin. Both follow the same font size and scale, so the gap does too.
constexpr float MenuTop(float margin, bool fps_shown, Box fps, float panel_x0, float panel_x1, float screen_height) {
    if (!fps_shown || fps.y0 > screen_height * 0.5f || fps.x1 <= panel_x0 || fps.x0 >= panel_x1) {
        return margin;
    }
    const float below = fps.y1 + margin * 0.5f;
    return below > margin ? below : margin;
}

// The menu buttons of every connected controller, from SDL gamepad events: D-pad up and down, the left stick pushed
// past half way, South to activate and East to close. A press released before the next poll still counts once.
class PadState {
public:
    static constexpr int16_t kStickThreshold = 16000;

    void Button(uint32_t pad, uint32_t bit, bool down) {
        Pad& p = pads_[pad];
        p.buttons = down ? (p.buttons | bit) : (p.buttons & ~bit);
        if (down) {
            latched_ |= bit;
        }
    }

    void StickY(uint32_t pad, int16_t value) {
        Pad& p = pads_[pad];
        p.stick = value < -kStickThreshold ? menu::kPadUp : value > kStickThreshold ? menu::kPadDown : 0;
        latched_ |= p.stick;
    }

    // A controller unplugged while a button was down never reports its release.
    void Remove(uint32_t pad) { pads_.erase(pad); }

    uint32_t Held() const {
        uint32_t held = 0;
        for (const auto& [id, p] : pads_) {
            held |= p.buttons | p.stick;
        }
        return held;
    }

    // Once per frame: what is held now plus what was pressed since the last call.
    uint32_t Take() {
        const uint32_t buttons = Held() | latched_;
        latched_ = 0;
        return buttons;
    }

private:
    struct Pad {
        uint32_t buttons = 0, stick = 0;
    };
    std::map<uint32_t, Pad> pads_;
    uint32_t latched_ = 0;
};

// F10: a held key repeats and only the first press counts; the menu never opens over a loading screen.
using menu::CloseGate;
using menu::NavStep;
using menu::PadNav;

// A toggle flips, a multiplier moves to its next value and an action runs once, for this session only.
inline void ActivateRow(const MenuRow& row) {
    switch (row.kind) {
    case RowKind::Toggle: runtime_cheat_toggle(int(row.toggle)); break;
    case RowKind::Multiplier: runtime_cheat_cycle(int(row.multiplier)); break;
    case RowKind::Action: runtime_cheat_action(int(row.action)); break;
    case RowKind::Heading: break;
    }
}

// One frame of the controller: the keyboard and the mouse move the same selection.
inline void ApplyPadStep(const NavStep& step, MenuList& list, CloseGate& close) {
    if (close.requested()) {
        return;
    }
    if (step.move) {
        list.Step(step.move);
    }
    if (step.activate) {
        ActivateRow(kMenuRows[static_cast<size_t>(list.selected)]);
    }
    if (step.close) {
        close.Request();
    }
}
using menu::ToggleAction;
using menu::TogglePress;

} // namespace sos::cheats
