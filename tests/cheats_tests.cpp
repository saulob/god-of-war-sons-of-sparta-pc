// Cheats, headless: the model (defaults, saved values, session overrides), the rules each hook applies, the F10 menu
// model, the detour engine and the real handlers executed on synthetic code that holds the game's own site
// instructions, the item actions, and the site tables of CUSA57767 1.00 and 1.01 on their real boot images.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "build_images.h"
#include "runtime_cheats.h"
#include "sos_cheat_menu.h"
#include "sos_cheat_patch.h"
#include "sos_cheats.h"

namespace fs = std::filesystem;
namespace cheats = sos::cheats;

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

uint64_t Fnv(const uint8_t* data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

// ---- model ------------------------------------------------------------------------------------------------------------

void TestModel() {
    const cheats::Saved none = cheats::ParseSaved("");
    for (const cheats::ToggleInfo& info : cheats::kToggles) {
        Check(!none.toggles[cheats::Index(info.toggle)], std::string(info.label) + " is Off by default");
        Check(std::string_view(info.key).starts_with("cheat_"), std::string(info.label) + " key is a cheat_ key");
    }
    for (const cheats::MultiplierInfo& info : cheats::kMultipliers) {
        Check(none.multipliers[cheats::Index(info.multiplier)] == cheats::kOff, std::string(info.label) + " is Off by default");
        Check(info.choices[0].value == cheats::kOff && std::string(info.choices[0].label) == "Off",
              std::string(info.label) + " lists Off first");
    }
    std::set<std::string> keys;
    for (const auto& t : cheats::kToggles) keys.insert(t.key);
    for (const auto& m : cheats::kMultipliers) keys.insert(m.key);
    Check(keys.size() == cheats::kToggleCount + cheats::kMultiplierCount, "every saved key is distinct");
    for (const auto& a : cheats::kActions) {
        Check(!keys.contains(a.label) && std::string(a.label).find("cheat_") == std::string::npos,
              std::string(a.label) + " is an action, not a saved setting");
    }

    const auto values = [](cheats::Multiplier m) {
        std::vector<int> out;
        for (const cheats::Choice& c : cheats::Info(m).choices) out.push_back(c.value);
        return out;
    };
    Check(values(cheats::Multiplier::Damage) == std::vector<int>{1, 2, 4, 6}, "Damage Multiplier is Off, 2x, 4x, 6x");
    Check(values(cheats::Multiplier::MovementSpeed) == std::vector<int>{1, 2}, "Movement Speed is Off, 2x");
    Check(values(cheats::Multiplier::JumpHeight) == std::vector<int>{1, 2, 4, 6}, "Jump Height is Off, 2x, 4x, 6x");

    const cheats::Saved saved = cheats::ParseSaved(
        "# header\ncheat_god_mode = 1\ncheat_infinite_magic=true\ncheat_damage_multiplier=6\ncheat_jump_height=3\n"
        "cheat_movement_speed=2 # comment\ncheat_infinite_spirit=0\r\n");
    Check(saved.toggles[cheats::Index(cheats::Toggle::GodMode)] && saved.toggles[cheats::Index(cheats::Toggle::InfiniteMagic)] &&
              !saved.toggles[cheats::Index(cheats::Toggle::InfiniteSpirit)],
          "saved toggles are read");
    Check(saved.multipliers[cheats::Index(cheats::Multiplier::Damage)] == 6, "saved 6x damage is read");
    Check(saved.multipliers[cheats::Index(cheats::Multiplier::MovementSpeed)] == 2, "saved 2x movement is read with a comment");
    Check(saved.multipliers[cheats::Index(cheats::Multiplier::JumpHeight)] == cheats::kOff, "an unlisted jump factor is Off");
    Check(cheats::ParseSaved("cheat_damage_multiplier=abc\ncheat_god_mode=yes").multipliers[0] == cheats::kOff &&
              !cheats::ParseSaved("cheat_god_mode=yes").toggles[0],
          "malformed values are Off");

    cheats::Session session;
    Check(session.Effective(cheats::Toggle::GodMode, saved), "no override follows the saved On");
    session.Set(cheats::Toggle::GodMode, saved, false);
    Check(!session.Effective(cheats::Toggle::GodMode, saved), "a session override takes priority over the saved value");
    Check(saved.toggles[0], "an override leaves the saved value alone");
    session.Set(cheats::Toggle::GodMode, saved, true);
    Check(!session.Overridden(), "changing back to the saved value follows the launcher again");
    session.Set(cheats::Multiplier::Damage, saved, 2);
    Check(session.Effective(cheats::Multiplier::Damage, saved) == 2 && session.Overridden(), "a multiplier override applies");
    session.Reset();
    Check(session.Effective(cheats::Multiplier::Damage, saved) == 6 && !session.Overridden(), "a new session clears the overrides");

    Check(cheats::NextChoice(cheats::Multiplier::Damage, 1) == 2 && cheats::NextChoice(cheats::Multiplier::Damage, 2) == 4 &&
              cheats::NextChoice(cheats::Multiplier::Damage, 4) == 6 && cheats::NextChoice(cheats::Multiplier::Damage, 6) == 1,
          "Damage Multiplier cycles Off, 2x, 4x, 6x, Off");
    Check(std::string(cheats::ChoiceLabel(cheats::Multiplier::JumpHeight, 6)) == "6x", "6x is labelled 6x");
}

// ---- rules ------------------------------------------------------------------------------------------------------------

void TestRules() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const cheats::Vitals hit{60.0f, 10.0f, -30.0f};
    const cheats::Vitals held = cheats::HeldHealth(true, true, 100.0f, hit);
    Check(held.hp == 100.0f && held.grey == 0.0f && held.amount == 0.0f, "God Mode: the player's damage takes nothing, HP full");
    const cheats::Vitals enemy = cheats::HeldHealth(true, false, 100.0f, hit);
    Check(enemy.hp == 60.0f && enemy.grey == 10.0f && enemy.amount == -30.0f, "God Mode: an enemy is not affected");
    const cheats::Vitals off = cheats::HeldHealth(false, true, 100.0f, hit);
    Check(off.hp == 60.0f && off.amount == -30.0f, "God Mode Off: original behaviour");
    Check(cheats::HeldHealth(true, true, 100.0f, {60.0f, 0.0f, 15.0f}).amount == 15.0f, "God Mode: healing still goes through");
    Check(cheats::HeldHealth(true, true, 100.0f, {60.0f, 0.0f, nan}).amount == 0.0f, "God Mode: a NaN change takes nothing");
    const cheats::Vitals grey = cheats::HeldGreyHealth(true, true, 100.0f, {80.0f, 5.0f, 20.0f});
    Check(grey.hp == 100.0f && grey.grey == 0.0f && grey.amount == 0.0f, "God Mode: grey health cannot shrink the bar");
    Check(cheats::HeldGreyHealth(true, true, 100.0f, {80.0f, 5.0f, -5.0f}).amount == -5.0f, "God Mode: grey recovery goes through");
    Check(cheats::HeldGreyHealth(true, false, 100.0f, {80.0f, 5.0f, 20.0f}).amount == 20.0f, "God Mode: an enemy's grey health");

    const cheats::Meter spent = cheats::HeldSpirit(true, true, 50.0f, {20.0f, -10.0f});
    Check(spent.current == 50.0f && spent.delta == 0.0f, "Spirit: consumption blocked, meter full");
    Check(cheats::HeldSpirit(true, true, 50.0f, {20.0f, 5.0f}).delta == 5.0f, "Spirit: a gain is allowed");
    Check(cheats::HeldSpirit(true, false, 50.0f, {20.0f, -10.0f}).delta == -10.0f, "Spirit: only the player");
    Check(cheats::HeldSpirit(false, true, 50.0f, {20.0f, -10.0f}).current == 20.0f, "Spirit Off: original behaviour");

    Check(cheats::HeldMana(true, true, 40.0f, 3.0f) == 40.0f, "Magic: a cost leaves it at the maximum");
    Check(cheats::HeldMana(true, true, 40.0f, 40.0f) == 40.0f, "Magic: a gain stops at the maximum (clamp)");
    Check(cheats::HeldMana(true, false, 40.0f, 3.0f) == 3.0f, "Magic: only the player");
    Check(cheats::HeldMana(false, true, 40.0f, 3.0f) == 3.0f, "Magic Off: original behaviour");

    Check(!cheats::MultipliesHit(1, true, false, false, 10.0f), "Damage Off: not multiplied");
    for (int factor : {2, 4, 6}) {
        Check(cheats::MultipliesHit(factor, true, false, false, 10.0f) && cheats::MultipliedDamage(factor, 10.0f) == 10.0f * factor,
              "Damage " + std::to_string(factor) + "x: player to enemy is multiplied");
    }
    Check(!cheats::MultipliesHit(2, false, true, false, 10.0f), "Damage: enemy to player is not multiplied");
    Check(!cheats::MultipliesHit(2, true, true, false, 10.0f), "Damage: the player's own hit on a player is not multiplied");
    Check(!cheats::MultipliesHit(2, false, false, false, 10.0f), "Damage: environment or self damage is not multiplied");
    Check(!cheats::MultipliesHit(2, true, false, false, 0.0f), "Damage: zero is not multiplied");
    Check(!cheats::MultipliesHit(2, true, false, false, nan), "Damage: NaN is not multiplied");
    Check(!cheats::MultipliesHit(2, true, false, false, inf), "Damage: infinity is not multiplied");
    Check(!cheats::MultipliesHit(6, true, false, false, std::numeric_limits<float>::max()), "Damage: a product that overflows is not multiplied");
    Check(!cheats::MultipliesHit(2, true, false, true, 10.0f), "Damage: unmodifiable damage is not multiplied");

    const cheats::Vector2 step{0.5f, -0.25f};
    const cheats::Vector2 fast = cheats::InputMovement(2, true, step);
    Check(fast.x == 1.0f && fast.y == -0.5f, "Movement 2x: the input movement doubles");
    Check(cheats::InputMovement(2, false, step).x == 0.5f, "Movement: only the player");
    Check(cheats::InputMovement(1, true, step).x == 0.5f, "Movement Off: original behaviour");

    for (int factor : {2, 4, 6}) {
        const cheats::Vector2 up = cheats::JumpDirection(factor, true, {0.3f, 1.0f});
        Check(std::fabs(up.y - std::sqrt(float(factor))) < 1e-6f && up.x == 0.3f,
              "Jump " + std::to_string(factor) + "x scales the upward speed by its square root");
        Check(std::fabs(up.y * up.y - float(factor)) < 1e-4f, "Jump " + std::to_string(factor) + "x gives that height factor");
    }
    Check(cheats::JumpDirection(4, true, {0.3f, -1.0f}).y == -1.0f, "Jump: a downward impulse is unchanged");
    Check(cheats::JumpDirection(4, false, {0.3f, 1.0f}).y == 1.0f, "Jump: only the player");
    Check(cheats::JumpDirection(1, true, {0.3f, 1.0f}).y == 1.0f, "Jump Off: original behaviour");

    Check(cheats::ActionAmount(cheats::Action::AddBloodOrbs) == 1000, "Add 1000 Blood Orbs adds 1000");
    Check(cheats::ActionAmount(cheats::Action::AddUpgradeMaterials) == 10, "Add 10 Upgrade Materials adds 10");
    Check(cheats::ActionItems(cheats::Action::AddBloodOrbs).size() == 1 &&
              std::string(cheats::ActionItems(cheats::Action::AddBloodOrbs)[0]) == "loot_orb_red",
          "Blood Orbs are loot_orb_red");
    Check(cheats::ActionItems(cheats::Action::AddUpgradeMaterials).size() == 9, "nine upgrade materials");
    bool boss = false;
    for (const char* id : cheats::ActionItems(cheats::Action::AddUpgradeMaterials)) boss |= cheats::IsBossTrophy(id);
    Check(!boss, "no boss trophy is an upgrade material");
    Check(cheats::IsBossTrophy("loot_boss_cyclops"), "loot_boss_* is a boss trophy");
    Check(cheats::AdditionFits(0, 1000) && !cheats::AdditionFits(std::numeric_limits<int64_t>::max() - 10, 1000),
          "an addition never wraps");
}

// ---- menu -------------------------------------------------------------------------------------------------------------

void TestMenu() {
    std::vector<std::string> labels;
    for (const auto& row : cheats::kMenuRows) labels.emplace_back(row.label);
    const std::vector<std::string> expected = {"PLAYER",         "God Mode",          "Infinite Spartan Spirit", "Infinite Magic",
                                               "COMBAT",         "Damage Multiplier", "MOVEMENT",                "Movement Speed",
                                               "Jump Height",    "ITEMS",             "Add 1000 Blood Orbs",     "Add 10 Upgrade Materials"};
    Check(labels == expected, "F10 menu rows in order with their labels: ITEMS follows Jump Height");
    Check(cheats::kMenuRows[10].kind == cheats::RowKind::Action && cheats::kMenuRows[11].kind == cheats::RowKind::Action,
          "item rows are one-shot actions");
    cheats::MenuList list;
    Check(list.selected == 1, "the selection opens on God Mode, not a heading");
    list.Step(-1);
    Check(list.selected == 1, "up from the first row stays");
    for (int i = 0; i < 3; ++i) list.Step(1);
    Check(list.selected == 5, "down skips the COMBAT heading");
    for (int i = 0; i < 20; ++i) list.Step(1);
    Check(list.selected == 11, "down stops at the last row");
    list.Click(9);
    Check(list.selected == 11, "a heading cannot be selected");
    list.Click(7);
    Check(list.selected == 7, "a click selects the row the keyboard and controller then move");
    list.TakeReveal();
    list.Click(5);
    Check(!list.TakeReveal(), "a click does not scroll the list");
    list.Step(1);
    Check(list.TakeReveal() && !list.TakeReveal(), "a keyboard or controller step scrolls its row into view once");
    Check(cheats::TogglePress(false, false, false) == cheats::ToggleAction::Open, "F10 opens");
    Check(cheats::TogglePress(false, true, false) == cheats::ToggleAction::Close, "F10 closes");
    Check(cheats::TogglePress(true, false, false) == cheats::ToggleAction::None, "a held F10 does nothing");
    Check(cheats::TogglePress(false, false, true) == cheats::ToggleAction::None, "F10 does not open over a loading screen");
    cheats::CloseGate gate;
    gate.Request();
    Check(!gate.Step(true, 0.0) && !gate.Step(true, 0.5), "closing waits while a menu key is held");
    Check(gate.Step(false, 0.6), "the game gets its input back once released");
    cheats::CloseGate stuck;
    stuck.Request();
    stuck.Step(true, 10.0);
    Check(stuck.Step(true, 11.1), "a key never released stops blocking after a second");
}

// The frame counter panel as the overlay places it (dcp_overlay_box.h): one text size in from the top right corner.
cheats::Box FpsPanel(float size, float screen_width, float text_width) {
    const float pad_x = size * 0.6f, pad_y = size * 0.3f;
    const float x1 = screen_width - size, x0 = x1 - text_width - 2 * pad_x;
    return {x0, size, x1, size + size + 2 * pad_y};
}

void TestPlacement() {
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float em = 18.0f * scale, margin = em * 0.8f, width = em * 20.0f, screen_w = 2560.0f * scale / 1.5f;
        const float right = screen_w - margin;
        const cheats::Box fps = FpsPanel(em, screen_w, em * 7.0f);
        const std::string at = " at scale " + std::to_string(scale);
        Check(cheats::MenuTop(margin, false, fps, right - width, right, 1440.0f) == margin, "FPS hidden: the menu keeps the top margin" + at);
        const float top = cheats::MenuTop(margin, true, fps, right - width, right, 1440.0f);
        Check(top > fps.y1, "FPS shown: the menu starts below the counter, never over it" + at);
        Check(top - fps.y1 >= margin * 0.5f - 0.001f && top - fps.y1 <= margin, "a gap of half a margin between counter and menu" + at);
        Check(top == fps.y1 + margin * 0.5f, "the gap follows the font size" + at);
    }
    const cheats::Box left = FpsPanel(18.0f, 600.0f, 100.0f);
    Check(cheats::MenuTop(14.4f, true, left, 1500.0f, 1900.0f, 1080.0f) == 14.4f, "a counter outside the menu's column moves nothing");
    Check(cheats::MenuTop(14.4f, true, {1600.0f, 1000.0f, 1800.0f, 1040.0f}, 1500.0f, 1900.0f, 1080.0f) == 14.4f,
          "a counter at the bottom moves nothing");
    Check(cheats::MenuTop(14.4f, true, {1600.0f, 18.0f, 1800.0f, 50.0f}, 1500.0f, 1900.0f, 720.0f) == 50.0f + 7.2f,
          "the menu follows a counter of another size and resolution");
}

void TestController() {
    namespace m = sos::menu;
    cheats::PadState pads;
    cheats::PadNav nav;
    double now = 0.0;
    const auto poll = [&](double dt = 0.016) {
        now += dt;
        return nav.Poll(pads.Take(), now);
    };
    poll();
    pads.Button(1, m::kPadDown, true);
    Check(poll().move == 1, "D-pad Down moves down at once");
    Check(poll().move == 0, "a held D-pad does not move every frame");
    Check(poll(0.4).move == 1, "a held D-pad repeats after its delay");
    int repeats = 0;
    for (int i = 0; i < 30; ++i) repeats += poll(0.016).move != 0;
    Check(repeats >= 3 && repeats <= 5, "the repeat is steady, about every 0.12 s (" + std::to_string(repeats) + " in 0.48 s)");
    pads.Button(1, m::kPadDown, false);
    poll();
    pads.Button(1, m::kPadUp, true);
    Check(poll().move == -1, "D-pad Up moves up");
    pads.Button(1, m::kPadUp, false);
    poll();
    pads.StickY(1, -30000);
    Check(poll().move == -1, "Left stick Up moves up");
    pads.StickY(1, 0);
    poll();
    pads.StickY(1, 30000);
    Check(poll().move == 1, "Left stick Down moves down");
    pads.StickY(1, 12000);
    Check(poll().move == 0, "a stick inside half way does not move");
    pads.StickY(1, 0);
    poll();
    pads.Button(2, m::kPadActivate, true);
    int activations = 0;
    for (int i = 0; i < 60; ++i) activations += poll().activate;
    Check(activations == 1, "a held A/Cross activates exactly once");
    pads.Button(2, m::kPadActivate, false);
    poll();
    pads.Button(2, m::kPadActivate, true);
    pads.Button(2, m::kPadActivate, false);
    Check(poll().activate, "a tap released between two frames still activates once");
    Check(!poll().activate, "and only once");
    pads.Button(2, m::kPadClose, true);
    Check(poll().close, "B/Circle closes");
    Check(nav.Held() & m::kPadClose, "the close gate sees B still held");
    pads.Remove(2);
    poll();
    Check(nav.Held() == 0 && pads.Held() == 0, "a controller unplugged while B is held releases the close gate");

    cheats::PadState opened;
    opened.Button(3, m::kPadActivate, true);
    cheats::PadNav fresh;
    Check(!fresh.Poll(opened.Take(), 1.0).activate, "a button held when the menu opens does nothing");
    Check(!fresh.Poll(opened.Take(), 1.1).activate, "not even while it stays held");
    opened.Button(3, m::kPadActivate, false);
    fresh.Poll(opened.Take(), 1.2);
    opened.Button(3, m::kPadActivate, true);
    Check(fresh.Poll(opened.Take(), 1.3).activate, "after its release it works");
}

// The controller drives the real runtime through the menu model the overlay uses.
void TestControllerRows() {
    namespace m = sos::menu;
    cheats::MenuList list;
    cheats::CloseGate close;
    cheats::PadState pads;
    cheats::PadNav nav;
    double now = 0.0;
    const auto frame = [&](int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            now += 0.016;
            cheats::ApplyPadStep(nav.Poll(pads.Take(), now), list, close);
        }
    };
    const auto press = [&](uint32_t bit, int held_frames = 1) {
        pads.Button(0, bit, true);
        frame(held_frames);
        pads.Button(0, bit, false);
        frame();
    };
    frame();
    Check(list.selected == 1, "the controller starts on God Mode");
    press(m::kPadActivate, 40);
    Check(runtime_cheat_toggle_state(int(cheats::Toggle::GodMode)) == SOS_CHEAT_ON, "A turns a toggle On exactly once, even held");
    press(m::kPadActivate);
    Check(runtime_cheat_toggle_state(int(cheats::Toggle::GodMode)) == SOS_CHEAT_OFF, "A again turns it Off");
    for (int i = 0; i < 3; ++i) press(m::kPadDown);
    Check(list.selected == 5, "D-pad Down reaches Damage Multiplier past the heading");
    const int damage[] = {2, 4, 6, 1};
    for (int value : damage) {
        press(m::kPadActivate, 30);
        Check(runtime_cheat_multiplier_value(int(cheats::Multiplier::Damage)) == value,
              "A moves Damage Multiplier one value to " + std::to_string(value));
    }
    press(m::kPadDown);
    for (int value : {2, 1}) {
        press(m::kPadActivate, 30);
        Check(runtime_cheat_multiplier_value(int(cheats::Multiplier::MovementSpeed)) == value,
              "A moves Movement Speed one value to " + std::to_string(value));
    }
    press(m::kPadDown);
    for (int value : {2, 4, 6, 1}) {
        press(m::kPadActivate, 30);
        Check(runtime_cheat_multiplier_value(int(cheats::Multiplier::JumpHeight)) == value,
              "A moves Jump Height one value to " + std::to_string(value));
    }
    pads.StickY(0, 32000);
    frame();
    pads.StickY(0, 0);
    frame();
    Check(list.selected == 10, "Left stick Down reaches Add 1000 Blood Orbs past the ITEMS heading");
    press(m::kPadActivate, 40);
    Check(runtime_cheat_action_state(int(cheats::Action::AddBloodOrbs)) == SOS_CHEAT_PENDING, "A requests the action");
    press(m::kPadActivate);
    Check(std::string(runtime_cheat_action_notice(int(cheats::Action::AddBloodOrbs)) ? runtime_cheat_action_notice(int(cheats::Action::AddBloodOrbs)) : "") == "Busy",
          "a held A requested it once; the second press says Busy");
    pads.Button(0, m::kPadClose, true);
    frame();
    Check(close.requested(), "B requests the close");
    Check(!close.Step(nav.Held() != 0, now), "the menu keeps the input while B is held");
    const int selected = list.selected;
    pads.Button(0, m::kPadDown, true);
    frame(10);
    Check(list.selected == selected, "nothing moves once closing");
    pads.Button(0, m::kPadDown, false);
    pads.Button(0, m::kPadClose, false);
    frame();
    Check(close.Step(nav.Held() != 0, now), "the game gets its input back once every menu button is released");
}
// ---- synthetic code ---------------------------------------------------------------------------------------------------

using VitalsFn = void(SOS_CHEAT_SYSV*)(void*, float);
using DamageFn = void(SOS_CHEAT_SYSV*)(void*, void*, void*);
using VectorFn = SosVector2(SOS_CHEAT_SYSV*)(void*, SosVector2);
using ObjectFn = void(SOS_CHEAT_SYSV*)(void*);
using RegsFn = void(SOS_CHEAT_SYSV*)(void*, void*, float);

constexpr uint32_t kImageSize = 0x4000;
constexpr uint32_t kGodHp = 0x1000, kGodGrey = 0x1100, kSpiritFn = 0x1200, kMagicFn = 0x1300, kDamageFn = 0x1400,
                   kMovementFn = 0x1500, kIdentity = 0x1580, kStringNew = 0x1590, kJumpFn = 0x1600, kWalletFn = 0x1700,
                   kRegsFn = 0x1800;

struct Code {
    uint32_t at;
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> Nops(size_t n) { return std::vector<uint8_t>(n, 0x90); }

std::vector<uint8_t> Join(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

const std::vector<Code>& Functions() {
    static const std::vector<Code> code = {
        {kGodHp, Join({{0x53, 0x48, 0x89, 0xfb}, Nops(12), {0xc5, 0xfa, 0x58, 0x53, 0x40, 0xc5, 0xfa, 0x11, 0x53, 0x40, 0x5b, 0xc3}})},
        {kGodGrey, Join({{0x53, 0x48, 0x89, 0xfb}, Nops(12), {0xc5, 0xfa, 0x58, 0x43, 0x44, 0xc5, 0xfa, 0x11, 0x43, 0x44, 0x5b, 0xc3}})},
        {kSpiritFn, Join({{0x53, 0x48, 0x89, 0xfb, 0xc5, 0xf8, 0x28, 0xc8}, Nops(8),
                          {0xc5, 0xf2, 0x58, 0x43, 0x20, 0xc5, 0xfa, 0x11, 0x43, 0x20, 0x5b, 0xc3}})},
        {kMagicFn, Join({{0x53, 0x48, 0x89, 0xfb}, Nops(12), {0xc5, 0xfa, 0x11, 0x43, 0x28, 0x5b, 0xc3}})},
        {kDamageFn, {0x53, 0x48, 0x89, 0xfb, 0x48, 0x89, 0xf7, 0x48, 0x89, 0xd6, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x90, 0x80, 0x01, 0x00,
                     0x00, 0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0x5b, 0xc3}},
        {kMovementFn, Join({{0x53, 0x48, 0x89, 0xfb, 0x31, 0xf6}, Nops(10), {0xe8, 0x6b, 0x00, 0x00, 0x00, 0x5b, 0xc3}})},
        {kIdentity, {0xc3}},
        {kJumpFn, Join({{0x53, 0x48, 0x89, 0xfb}, Nops(12), {0x80, 0x3d, 0xe9, 0xe9, 0xff, 0xff, 0x00, 0x5b, 0xc3}})},
        {kWalletFn, {0xc3}},
        {kRegsFn, Join({{0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x89, 0xfb},
                        {0xb8, 0x11, 0x11, 0x11, 0x11, 0xb9, 0x22, 0x22, 0x22, 0x22, 0xba, 0x33, 0x33, 0x33, 0x33, 0xbd, 0x44, 0x44,
                         0x44, 0x44},
                        {0x41, 0xb8, 0x55, 0x55, 0x55, 0x55, 0x41, 0xb9, 0x66, 0x66, 0x66, 0x66, 0x41, 0xba, 0x77, 0x77, 0x77, 0x77,
                         0x41, 0xbb, 0x88, 0x88, 0x88, 0x88, 0x41, 0xbc, 0x99, 0x99, 0x99, 0x99, 0x41, 0xbd, 0xaa, 0xaa, 0xaa, 0xaa,
                         0x41, 0xbe, 0xbb, 0xbb, 0xbb, 0xbb, 0x41, 0xbf, 0xcc, 0xcc, 0xcc, 0xcc},
                        {0xc5, 0xf9, 0x6e, 0xc8, 0xc5, 0xf9, 0x6e, 0xf9, 0xc5, 0x79, 0x6e, 0xfa},
                        {0xc7, 0x44, 0x24, 0xf0, 0x5a, 0x5a, 0x5a, 0x5a, 0xf9},
                        Nops(10),
                        {0xc5, 0xfa, 0x58, 0x53, 0x40, 0x0f, 0x92, 0x86, 0x80, 0x00, 0x00, 0x00},
                        {0x48, 0x89, 0x06, 0x48, 0x89, 0x4e, 0x08, 0x48, 0x89, 0x56, 0x10, 0x48, 0x89, 0x6e, 0x18, 0x4c, 0x89, 0x46,
                         0x20, 0x4c, 0x89, 0x4e, 0x28, 0x4c, 0x89, 0x56, 0x30, 0x4c, 0x89, 0x5e, 0x38, 0x4c, 0x89, 0x66, 0x40, 0x4c,
                         0x89, 0x6e, 0x48, 0x4c, 0x89, 0x76, 0x50, 0x4c, 0x89, 0x7e, 0x58, 0x48, 0x89, 0x5e, 0x60},
                        {0xc5, 0xf9, 0x7e, 0x4e, 0x68, 0xc5, 0xf9, 0x7e, 0x7e, 0x6c, 0xc5, 0x79, 0x7e, 0x7e, 0x70},
                        {0x8b, 0x44, 0x24, 0xf0, 0x89, 0x46, 0x74, 0xc5, 0xfa, 0x11, 0x56, 0x78},
                        {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3}})},
    };
    return code;
}

struct Synthetic {
    uint8_t* base = nullptr;
    uint8_t* image = nullptr;
    uint8_t* stubs = nullptr;
    std::vector<uint8_t> pristine;
    SosCheatBuild build{};

    Synthetic() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kImageSize + SOS_CHEAT_STUB_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        image = base;
        stubs = base + kImageSize;
        for (const Code& c : Functions()) std::memcpy(image + c.at, c.bytes.data(), c.bytes.size());
        pristine.assign(image, image + kImageSize);
        build = MakeBuild(kGodHp + 0x10);
    }

    ~Synthetic() { VirtualFree(base, 0, MEM_RELEASE); }

    SosCheatSite Site(uint32_t rva, uint8_t length, uint8_t executed, int8_t rip, uint8_t rel, uint32_t begin, uint32_t end) {
        SosCheatSite s{};
        s.rva = rva;
        s.length = length;
        s.executed = executed;
        s.rip_displacement = rip;
        s.relative_branch = rel;
        std::memcpy(s.original, image + rva, length);
        s.method = {begin, end};
        s.method_hash = Fnv(image + begin, end - begin);
        return s;
    }

    SosCheatBuild MakeBuild(uint32_t god_hp_site) {
        SosCheatBuild b{};
        b.build = "TEST-SYNTHETIC";
        b.image_size = kImageSize;
        const uint32_t god_fn = god_hp_site & ~0xffu;
        b.sites[SOS_CHEAT_SITE_GOD_HP] = Site(god_hp_site, 5, 5, -1, 0, god_fn, god_fn + 0x100);
        b.sites[SOS_CHEAT_SITE_GOD_GREY] = Site(kGodGrey + 0x10, 5, 5, -1, 0, kGodGrey, kGodGrey + 0x20);
        b.sites[SOS_CHEAT_SITE_SPIRIT] = Site(kSpiritFn + 0x10, 5, 5, -1, 0, kSpiritFn, kSpiritFn + 0x20);
        b.sites[SOS_CHEAT_SITE_MAGIC] = Site(kMagicFn + 0x10, 5, 5, -1, 0, kMagicFn, kMagicFn + 0x20);
        b.sites[SOS_CHEAT_SITE_DAMAGE] = Site(kDamageFn + 0x14, 6, 6, -1, 0, kDamageFn, kDamageFn + 0x20);
        b.sites[SOS_CHEAT_SITE_MOVEMENT] = Site(kMovementFn + 0x10, 5, 5, -1, 1, kMovementFn, kMovementFn + 0x20);
        b.sites[SOS_CHEAT_SITE_JUMP] = Site(kJumpFn + 0x10, 7, 7, 2, 0, kJumpFn, kJumpFn + 0x20);
        b.sites[SOS_CHEAT_SITE_WALLET_TICK] = Site(kWalletFn, 5, 1, -1, 0, kWalletFn, kWalletFn + 0x10);
        for (auto& f : b.functions) f = {kIdentity, kIdentity + 0x10, Fnv(image + kIdentity, 0x10)};
        b.string_new = kStringNew;
        b.string_new_hash = Fnv(image + kStringNew, 32);
        return b;
    }

    void Protect(DWORD mode) {
        DWORD old = 0;
        VirtualProtect(base, kImageSize + SOS_CHEAT_STUB_BYTES, mode, &old);
    }

    bool Pristine() const { return std::memcmp(image, pristine.data(), kImageSize) == 0; }

    std::set<uint32_t> Changed() const {
        std::set<uint32_t> out;
        for (uint32_t i = 0; i < kImageSize; ++i)
            if (image[i] != pristine[i]) out.insert(i);
        return out;
    }

    template <typename F> F Fn(uint32_t at) const { return reinterpret_cast<F>(image + at); }
};

// ---- fake game objects and functions ----------------------------------------------------------------------------------

struct Obj {
    alignas(16) uint8_t bytes[0x100] = {};
    Obj(bool player = false) {
        void* self = this;
        std::memcpy(bytes + 0x10, &self, 8);
        bytes[0x18] = player ? 1 : 0;
    }
    template <typename T> T& At(size_t offset) { return *reinterpret_cast<T*>(bytes + offset); }
};

int g_game_calls = 0;
float g_seen_damage = 0.0f;
int g_apply_calls = 0;
Obj* g_pawn = nullptr;
Obj* g_wallet = nullptr;
std::map<std::string, Obj*> g_items;
std::map<void*, int64_t> g_counts;
std::vector<SosNullableBool> g_hides;
std::vector<std::string> g_strings;

SOS_CHEAT_SYSV void* FakeGameObject(void* component, const void*) {
    ++g_game_calls;
    return component;
}
SOS_CHEAT_SYSV uint8_t FakeIsPlayer(void* object, const void*) {
    ++g_game_calls;
    return static_cast<uint8_t*>(object)[0x18];
}
SOS_CHEAT_SYSV void* FakePawn(void*, const void*) {
    ++g_game_calls;
    return g_pawn;
}
SOS_CHEAT_SYSV int32_t FakePlayerIndex(void* controller, const void*) {
    ++g_game_calls;
    return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(controller) + 0x1c);
}
SOS_CHEAT_SYSV void* FakeWallet(void* object, const void*) {
    ++g_game_calls;
    return object == g_pawn ? g_wallet : nullptr;
}
SOS_CHEAT_SYSV void* FakeStringNew(const char* text) {
    ++g_game_calls;
    g_strings.emplace_back(text);
    return reinterpret_cast<void*>(g_strings.size());
}
SOS_CHEAT_SYSV void* FakeItemById(void* id, const void*) {
    ++g_game_calls;
    const auto found = g_items.find(g_strings[reinterpret_cast<size_t>(id) - 1]);
    return found == g_items.end() ? nullptr : found->second;
}
SOS_CHEAT_SYSV int64_t FakeAmount(void*, void* item, const void*) {
    ++g_game_calls;
    return g_counts[item];
}
SOS_CHEAT_SYSV void FakeAdd(void*, void* item, int64_t count, SosNullableBool hide, const void*) {
    ++g_game_calls;
    g_counts[item] += count;
    g_hides.push_back(hide);
}
SOS_CHEAT_SYSV SosVector2 FakeSlope(void*, SosVector2 delta, const void*) {
    ++g_game_calls;
    return delta;
}
SOS_CHEAT_SYSV void FakeApply(void* module, void*, const void*) {
    ++g_apply_calls;
    void* config = *reinterpret_cast<void**>(static_cast<uint8_t*>(module) + 0x10);
    g_seen_damage = *reinterpret_cast<float*>(static_cast<uint8_t*>(config) + 0x10);
}

SosCheatBindings FakeBindings() {
    SosCheatBindings b{};
    b.get_game_object = FakeGameObject;
    b.is_player = FakeIsPlayer;
    b.get_pawn = FakePawn;
    b.player_index = FakePlayerIndex;
    b.try_get_wallet = FakeWallet;
    b.item_by_id = FakeItemById;
    b.wallet_amount = FakeAmount;
    b.wallet_add = FakeAdd;
    b.slope_aligned = FakeSlope;
    b.string_new = FakeStringNew;
    return b;
}

void Install(Synthetic& s, const SosCheatBuild* build, std::string* message = nullptr) {
    s.Protect(PAGE_READWRITE);
    char text[600] = "";
    const SosCheatBindings bindings = FakeBindings();
    runtime_cheats_install_build(s.image, kImageSize, build, s.stubs, SOS_CHEAT_STUB_BYTES, &bindings, text, sizeof(text));
    s.Protect(PAGE_EXECUTE_READ);
    if (message) *message = text;
}

int Toggle(cheats::Toggle t) { return runtime_cheat_toggle_state(int(t)); }
int MultState(cheats::Multiplier m) { return runtime_cheat_multiplier_state(int(m)); }

std::string Notice(cheats::Action action) {
    const char* text = runtime_cheat_action_notice(int(action));
    return text ? text : "";
}

bool ChangedOnlyAt(const Synthetic& s, std::initializer_list<int> sites) {
    std::set<uint32_t> allowed;
    for (int site : sites)
        for (uint32_t i = 0; i < s.build.sites[site].length; ++i) allowed.insert(s.build.sites[site].rva + i);
    const std::set<uint32_t> changed = s.Changed();
    if (changed.empty()) return false;
    for (uint32_t at : changed)
        if (!allowed.contains(at)) return false;
    for (int site : sites)
        if (s.image[s.build.sites[site].rva] != 0xe9) return false;
    return true;
}

// ---- runtime on synthetic code ----------------------------------------------------------------------------------------

void TestSyntheticRuntime(const fs::path& sandbox) {
    Synthetic s;
    std::string message;
    Install(s, &s.build, &message);
    Check(message.find("7 of 7 ready") != std::string::npos && message.find("unavailable") == std::string::npos,
          "every cheat is ready: " + message);
    Check(s.Pristine(), "installing writes nothing to the game code");

    runtime_cheats_start_text("");
    Check(s.Pristine() && Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_OFF && !runtime_cheats_used(),
          "every cheat starts Off: the game code is original byte for byte");

    const auto god_hp = s.Fn<VitalsFn>(kGodHp);
    const auto god_grey = s.Fn<VitalsFn>(kGodGrey);
    Obj player(true), enemy(false);
    const auto vitals = [](Obj& o, float max, float hp, float grey) {
        o.At<float>(0x38) = max;
        o.At<float>(0x40) = hp;
        o.At<float>(0x44) = grey;
    };

    runtime_cheats_start_text("cheat_god_mode=1\n");
    Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_ON && runtime_cheats_used(), "the saved God Mode starts On and marks cheats used");
    Check(ChangedOnlyAt(s, {SOS_CHEAT_SITE_GOD_HP, SOS_CHEAT_SITE_GOD_GREY}), "God Mode writes only its two jumps");
    vitals(player, 100.0f, 60.0f, 10.0f);
    god_hp(&player, -30.0f);
    Check(player.At<float>(0x40) == 100.0f && player.At<float>(0x44) == 0.0f, "God Mode: the player's damage leaves HP full and grey 0");
    vitals(player, 100.0f, 60.0f, 0.0f);
    god_hp(&player, 5.0f);
    Check(player.At<float>(0x40) == 105.0f, "God Mode: a heal still reaches the game, which clamps it");
    vitals(enemy, 100.0f, 60.0f, 0.0f);
    god_hp(&enemy, -30.0f);
    Check(enemy.At<float>(0x40) == 30.0f, "God Mode: an enemy takes its damage");
    vitals(player, 100.0f, 80.0f, 5.0f);
    god_grey(&player, 20.0f);
    Check(player.At<float>(0x44) == 0.0f && player.At<float>(0x40) == 100.0f, "God Mode: grey health does not reduce the bar");
    vitals(enemy, 100.0f, 80.0f, 5.0f);
    god_grey(&enemy, 20.0f);
    Check(enemy.At<float>(0x44) == 25.0f, "God Mode: an enemy's grey health is unchanged");

    runtime_cheat_toggle(int(cheats::Toggle::GodMode));
    Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_OFF && s.Pristine(), "F10 Off restores the original bytes");
    Check(runtime_cheat_saved_toggle(int(cheats::Toggle::GodMode)) == 1, "the F10 override leaves the saved value On");
    vitals(player, 100.0f, 60.0f, 0.0f);
    god_hp(&player, -30.0f);
    Check(player.At<float>(0x40) == 30.0f, "God Mode Off: the game's own damage again");
    runtime_cheats_start_text("cheat_god_mode=1\n");
    Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_ON, "a new session clears the override and follows the saved value");
    runtime_cheats_start_text("");
    Check(s.Pristine() && runtime_cheats_used(), "a session with every cheat Off is original again; used stays set");

    runtime_cheat_toggle(int(cheats::Toggle::InfiniteSpirit));
    Obj spirit_p(true), spirit_e(false);
    spirit_p.At<float>(0x20) = 20.0f;
    spirit_p.At<float>(0x24) = 50.0f;
    s.Fn<VitalsFn>(kSpiritFn)(&spirit_p, -10.0f);
    Check(spirit_p.At<float>(0x20) == 50.0f, "Spirit: consumption blocked, meter at its maximum");
    spirit_p.At<float>(0x20) = 50.0f;
    s.Fn<VitalsFn>(kSpiritFn)(&spirit_p, 5.0f);
    Check(spirit_p.At<float>(0x20) == 55.0f, "Spirit: a gain is allowed (the game clamps it)");
    spirit_e.At<float>(0x20) = 20.0f;
    spirit_e.At<float>(0x24) = 50.0f;
    s.Fn<VitalsFn>(kSpiritFn)(&spirit_e, -10.0f);
    Check(spirit_e.At<float>(0x20) == 10.0f, "Spirit: only the player");
    runtime_cheat_toggle(int(cheats::Toggle::InfiniteSpirit));

    runtime_cheat_toggle(int(cheats::Toggle::InfiniteMagic));
    Obj mana_p(true), mana_e(false);
    mana_p.At<float>(0x2c) = 40.0f;
    s.Fn<VitalsFn>(kMagicFn)(&mana_p, 3.0f);
    Check(mana_p.At<float>(0x28) == 40.0f, "Magic: the stored value is the maximum");
    mana_e.At<float>(0x2c) = 40.0f;
    s.Fn<VitalsFn>(kMagicFn)(&mana_e, 3.0f);
    Check(mana_e.At<float>(0x28) == 3.0f, "Magic: only the player");
    runtime_cheat_toggle(int(cheats::Toggle::InfiniteMagic));
    Check(s.Pristine(), "Spirit and Magic Off leave no residual patch");

    // Damage: the module's vtable calls the fake ApplyModule, which sees the damage at the time of the call.
    std::vector<void*> vtable(0x200 / 8, nullptr);
    vtable[0x178 / 8] = reinterpret_cast<void*>(FakeApply);
    Obj module, config, source_p(true), source_e(false), target_p(true), target_e(false), hit;
    module.At<void*>(0) = vtable.data();
    module.At<void*>(0x10) = &config;
    const auto strike = [&](Obj* source, Obj* target, float damage, bool unmodifiable = false) {
        config.At<float>(0x10) = damage;
        config.bytes[0x35] = unmodifiable ? 1 : 0;
        hit.At<void*>(0x10) = source;
        hit.At<void*>(0x20) = target;
        g_seen_damage = -1.0f;
        s.Fn<DamageFn>(kDamageFn)(&hit, &module, nullptr);
        return g_seen_damage;
    };
    Check(strike(&source_p, &target_e, 10.0f) == 10.0f, "Damage Off: original damage");
    const int expected_cycle[] = {2, 4, 6, 1};
    for (int factor : expected_cycle) {
        runtime_cheat_cycle(int(cheats::Multiplier::Damage));
        Check(runtime_cheat_multiplier_value(int(cheats::Multiplier::Damage)) == factor, "Damage cycles to " + std::to_string(factor));
        const float dealt = strike(&source_p, &target_e, 10.0f);
        Check(dealt == 10.0f * float(factor), "Damage " + std::to_string(factor) + "x: player to enemy");
        Check(config.At<float>(0x10) == 10.0f, "Damage " + std::to_string(factor) + "x: the shared config is restored after the hit");
        if (factor == 2) {
            Check(strike(&source_e, &target_p, 10.0f) == 10.0f, "Damage 2x: enemy to player is not multiplied");
            Check(strike(&source_e, &target_e, 10.0f) == 10.0f, "Damage 2x: environment damage is not multiplied");
            Check(strike(&source_p, &target_p, 10.0f) == 10.0f, "Damage 2x: self damage is not multiplied");
            Check(strike(&source_p, &target_e, 0.0f) == 0.0f, "Damage 2x: zero stays zero");
            Check(std::isnan(strike(&source_p, &target_e, std::numeric_limits<float>::quiet_NaN())), "Damage 2x: NaN is left alone");
            Check(std::isinf(strike(&source_p, &target_e, std::numeric_limits<float>::infinity())), "Damage 2x: infinity is left alone");
            Check(strike(&source_p, &target_e, 10.0f, true) == 10.0f, "Damage 2x: unmodifiable damage is left alone");
        }
    }
    Check(MultState(cheats::Multiplier::Damage) == SOS_CHEAT_OFF && s.Pristine(), "Damage back to Off restores the code");

    const auto move = s.Fn<VectorFn>(kMovementFn);
    Obj mover_p(true), mover_e(false);
    SosVector2 out = move(&mover_p, {0.5f, -0.25f});
    Check(out.x == 0.5f && out.y == -0.25f, "Movement Off: the original call (relocated rel32 not involved) returns the input");
    runtime_cheat_cycle(int(cheats::Multiplier::MovementSpeed));
    out = move(&mover_p, {0.5f, -0.25f});
    Check(out.x == 1.0f && out.y == -0.5f, "Movement 2x: the player's input movement doubles");
    out = move(&mover_e, {0.5f, -0.25f});
    Check(out.x == 0.5f, "Movement 2x: other characters are unchanged");
    runtime_cheat_cycle(int(cheats::Multiplier::MovementSpeed));
    Check(s.Pristine(), "Movement Off restores the code");

    const auto jump = s.Fn<VectorFn>(kJumpFn);
    Obj jumper_ctx(true), jumper_state, other_ctx(false), other_state;
    jumper_state.At<void*>(0x58) = &jumper_ctx;
    other_state.At<void*>(0x58) = &other_ctx;
    for (int factor : {2, 4, 6}) {
        runtime_cheat_cycle(int(cheats::Multiplier::JumpHeight));
        const SosVector2 up = jump(&jumper_state, {0.25f, 2.0f});
        Check(std::fabs(up.y - 2.0f * std::sqrt(float(factor))) < 1e-5f && up.x == 0.25f,
              "Jump " + std::to_string(factor) + "x: the player's impulse is scaled by sqrt(" + std::to_string(factor) + ")");
        Check(jump(&other_state, {0.25f, 2.0f}).y == 2.0f, "Jump " + std::to_string(factor) + "x: another character is unchanged");
        Check(jump(&jumper_state, {0.25f, -2.0f}).y == -2.0f, "Jump " + std::to_string(factor) + "x: a downward impulse is unchanged");
    }
    runtime_cheat_cycle(int(cheats::Multiplier::JumpHeight));
    Check(s.Pristine() && MultState(cheats::Multiplier::JumpHeight) == SOS_CHEAT_OFF, "Jump Off restores the code");

    // Item actions run on the game thread inside PlayerController.Update, never where they are requested.
    Obj pawn(true), wallet, controller1, controller2;
    wallet.At<void*>(0x20) = &wallet;
    controller1.At<int32_t>(0x1c) = 0;
    controller2.At<int32_t>(0x1c) = 1;
    g_pawn = &pawn;
    g_wallet = &wallet;
    std::vector<Obj> items(11);
    g_items.clear();
    g_items[cheats::kBloodOrbId] = &items[0];
    for (size_t i = 0; i < 9; ++i) g_items[cheats::kUpgradeMaterialIds[i]] = &items[1 + i];
    g_items["loot_boss_cyclops"] = &items[10];
    g_counts.clear();
    g_counts[&items[0]] = 500;
    const auto tick = s.Fn<ObjectFn>(kWalletFn);
    g_game_calls = 0;
    runtime_cheat_action(int(cheats::Action::AddBloodOrbs));
    Check(g_game_calls == 0, "requesting an action calls no game function on the requesting thread");
    Check(runtime_cheat_action_state(int(cheats::Action::AddBloodOrbs)) == SOS_CHEAT_PENDING, "a requested action waits for the game");
    Check(ChangedOnlyAt(s, {SOS_CHEAT_SITE_WALLET_TICK}),
          "the wait hooks PlayerController.Update only");
    runtime_cheat_action(int(cheats::Action::AddUpgradeMaterials));
    Check(Notice(cheats::Action::AddUpgradeMaterials) == "Busy",
          "a second action while one waits is Busy");
    tick(&controller2);
    Check(g_counts[&items[0]] == 500, "player 2's controller does not take the action");
    tick(&controller1);
    Check(g_counts[&items[0]] == 1500, "Add 1000 Blood Orbs adds exactly 1000 to the wallet's count");
    Check(!g_hides.empty() && g_hides.back().has_value == 0, "Blood Orbs are announced as a pickup would be");
    Check(Notice(cheats::Action::AddBloodOrbs) == "+1000", "the row shows +1000");
    Check(s.Pristine() && runtime_cheat_action_state(int(cheats::Action::AddBloodOrbs)) == SOS_CHEAT_OFF,
          "the Update hook is removed once the action ran");
    tick(&controller1);
    Check(g_counts[&items[0]] == 1500, "the action runs once per press");

    runtime_cheat_action(int(cheats::Action::AddUpgradeMaterials));
    tick(&controller1);
    bool tens = true;
    for (size_t i = 0; i < 9; ++i) tens &= g_counts[&items[1 + i]] == 10;
    Check(tens, "Add 10 Upgrade Materials adds 10 of each of the nine materials");
    Check(g_counts[&items[10]] == 0, "no boss trophy is added");
    Check(g_hides.back().has_value == 1 && g_hides.back().value == 1, "materials are added without a pickup notice");

    g_items.erase("loot_rare_spear");
    const std::map<void*, int64_t> before = g_counts;
    runtime_cheat_action(int(cheats::Action::AddUpgradeMaterials));
    tick(&controller1);
    Check(g_counts == before, "an unknown material adds nothing at all");
    Check(Notice(cheats::Action::AddUpgradeMaterials) == "Unavailable",
          "the row says Unavailable");
    g_items["loot_rare_spear"] = &items[5];

    g_pawn = nullptr;
    runtime_cheat_action(int(cheats::Action::AddBloodOrbs));
    tick(&controller1);
    Check(g_counts[&items[0]] == 1500 && s.Pristine(), "without a player nothing is added and the hook is removed");
    g_pawn = &pawn;

    g_counts[&items[0]] = std::numeric_limits<int64_t>::max() - 5;
    runtime_cheat_action(int(cheats::Action::AddBloodOrbs));
    tick(&controller1);
    Check(g_counts[&items[0]] == std::numeric_limits<int64_t>::max() - 5, "a count at its largest is never wrapped");

    runtime_cheat_action(int(cheats::Action::AddBloodOrbs));
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    runtime_cheats_frame();
    Check(runtime_cheat_action_state(int(cheats::Action::AddBloodOrbs)) == SOS_CHEAT_OFF && s.Pristine(),
          "an action no controller takes in time is given up and its hook removed");
    Check(Notice(cheats::Action::AddBloodOrbs) == "Unavailable",
          "a timed out action says Unavailable");

    // A restore that fails is Failed, never Off; a retry restores once the code is ours again.
    runtime_cheat_toggle(int(cheats::Toggle::GodMode));
    const uint32_t hp_site = s.build.sites[SOS_CHEAT_SITE_GOD_HP].rva;
    uint8_t patched[5];
    std::memcpy(patched, s.image + hp_site, 5);
    s.Protect(PAGE_EXECUTE_READWRITE);
    std::memset(s.image + hp_site, 0x90, 5);
    runtime_cheat_toggle(int(cheats::Toggle::GodMode));
    Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_FAILED, "a restore over foreign bytes reports Failed, not Off");
    std::memcpy(s.image + hp_site, patched, 5);
    s.Protect(PAGE_EXECUTE_READ);
    runtime_cheat_toggle(int(cheats::Toggle::GodMode));
    Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_OFF && s.Pristine(), "the retry restores the original bytes");

    // F10 never writes the settings file.
    const fs::path file = sandbox / "sons_of_sparta.ini";
    { std::ofstream(file, std::ios::binary) << "cheat_infinite_magic=1\ncheat_jump_height=4\n"; }
    const auto read = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string written = read();
    runtime_cheats_start(file.string().c_str());
    Check(Toggle(cheats::Toggle::InfiniteMagic) == SOS_CHEAT_ON && runtime_cheat_multiplier_value(int(cheats::Multiplier::JumpHeight)) == 4,
          "the session starts from the settings file");
    runtime_cheat_toggle(int(cheats::Toggle::InfiniteMagic));
    runtime_cheat_cycle(int(cheats::Multiplier::JumpHeight));
    runtime_cheat_toggle(int(cheats::Toggle::GodMode));
    Check(read() == written, "F10 changes are not written to the settings file");
    Check(runtime_cheat_saved_toggle(int(cheats::Toggle::InfiniteMagic)) == 1 &&
              runtime_cheat_saved_multiplier(int(cheats::Multiplier::JumpHeight)) == 4,
          "F10 changes leave the saved values alone");
    runtime_cheats_start(file.string().c_str());
    Check(Toggle(cheats::Toggle::InfiniteMagic) == SOS_CHEAT_ON && Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_OFF &&
              runtime_cheat_multiplier_value(int(cheats::Multiplier::JumpHeight)) == 4,
          "the next session starts from the saved values, not the overrides");
    runtime_cheats_start_text("");
    Check(s.Pristine(), "every cheat Off: no residual patch");
}

void TestControllerOnRuntime() {
    Synthetic s;
    Install(s, &s.build);
    runtime_cheats_start_text("");
    TestControllerRows();
    Obj controller;
    controller.At<int32_t>(0x1c) = 0;
    g_pawn = nullptr;
    s.Fn<ObjectFn>(kWalletFn)(&controller);
    runtime_cheats_start_text("");
    Check(s.Pristine() && runtime_cheat_action_state(int(cheats::Action::AddBloodOrbs)) == SOS_CHEAT_OFF,
          "the controller session leaves no patch and no waiting action");
}

void TestRegisters() {
    Synthetic s;
    const SosCheatBuild regs = s.MakeBuild(kRegsFn + 0x70);
    Install(s, &regs);
    runtime_cheats_start_text("cheat_god_mode=1\n");
    Obj enemy(false);
    enemy.At<float>(0x40) = 7.0f;
    alignas(16) uint8_t out[0x90] = {};
    s.Fn<RegsFn>(kRegsFn)(&enemy, out, 2.0f);
    const auto q = [&](size_t at) { uint64_t v; std::memcpy(&v, out + at, 8); return v; };
    const auto d = [&](size_t at) { uint32_t v; std::memcpy(&v, out + at, 4); return v; };
    bool gprs = q(0) == 0x11111111 && q(8) == 0x22222222 && q(16) == 0x33333333 && q(24) == 0x44444444 && q(32) == 0x55555555 &&
                q(40) == 0x66666666 && q(48) == 0x77777777 && q(56) == 0x88888888 && q(64) == 0x99999999 && q(72) == 0xaaaaaaaa &&
                q(80) == 0xbbbbbbbb && q(88) == 0xcccccccc && q(96) == reinterpret_cast<uint64_t>(&enemy);
    Check(gprs, "the stub keeps every general register across the hook");
    Check(d(104) == 0x11111111 && d(108) == 0x22222222 && d(112) == 0x33333333, "the stub keeps xmm1, xmm7 and xmm15");
    Check(d(116) == 0x5a5a5a5a, "the stub leaves the red zone below rsp untouched");
    Check(out[0x80] == 1, "the stub keeps the flags (CF set before the hook)");
    float sum;
    std::memcpy(&sum, out + 120, 4);
    Check(sum == 9.0f, "the relocated instruction runs after the hook");
    runtime_cheats_start_text("");
    Check(s.Pristine(), "the register test leaves no patch");
}

int g_engine_calls = 0;

SOS_CHEAT_SYSV int PassThrough(SosCheatContext*, SosCheatVector*) {
    ++g_engine_calls;
    return 0;
}

// The engine alone: a stub whose handler leaves the instruction to run, so the relocated call rel32 and rip-relative
// operand execute from the stub, and an atomic install and restore.
void TestEngine() {
    Synthetic s;
    for (int site : {SOS_CHEAT_SITE_MOVEMENT, SOS_CHEAT_SITE_JUMP}) {
        s.Protect(PAGE_READWRITE);
        uint8_t* stub = s.stubs + size_t(site) * SOS_CHEAT_STUB_SIZE;
        SosCheatDetour detour{};
        const size_t built = sos_cheat_stub_build(stub, SOS_CHEAT_STUB_SIZE, s.image, &s.build.sites[site], PassThrough);
        Check(built > 0 && built <= SOS_CHEAT_STUB_SIZE, "a stub fits its slot (" + std::to_string(built) + " bytes)");
        Check(sos_cheat_detour_prepare(&detour, s.image, &s.build.sites[site], stub), "the detour reaches its stub");
        Check(!sos_cheat_stub_build(stub, 64, s.image, &s.build.sites[site], PassThrough), "a slot too small is refused");
        sos_cheat_stub_build(stub, SOS_CHEAT_STUB_SIZE, s.image, &s.build.sites[site], PassThrough);
        s.Protect(PAGE_EXECUTE_READ);
        Check(sos_cheat_detour_state(&detour) == 0, "the site starts original");
        Check(sos_cheat_detour_write(&detour, 0) == SOS_CHEAT_WRITE_MISMATCH, "a restore of an original site writes nothing");
        Check(sos_cheat_detour_write(&detour, 1) == SOS_CHEAT_WRITE_DONE && sos_cheat_detour_state(&detour) == 1, "install");
        Check(sos_cheat_detour_write(&detour, 1) == SOS_CHEAT_WRITE_MISMATCH, "a second install writes nothing");
        g_engine_calls = 0;
        Obj object(false);
        const SosVector2 out = s.Fn<VectorFn>(site == SOS_CHEAT_SITE_MOVEMENT ? kMovementFn : kJumpFn)(&object, {1.5f, 2.5f});
        Check(g_engine_calls == 1 && out.x == 1.5f && out.y == 2.5f,
              site == SOS_CHEAT_SITE_MOVEMENT ? "the relocated call rel32 runs from the stub" : "the relocated rip-relative compare runs from the stub");
        Check(sos_cheat_detour_write(&detour, 0) == SOS_CHEAT_WRITE_DONE && s.Pristine(), "restore is byte for byte");
    }
}

void TestRefusals() {
    {
        Synthetic s;
        s.Protect(PAGE_READWRITE);
        s.image[s.build.sites[SOS_CHEAT_SITE_GOD_HP].rva] ^= 0xff;
        s.pristine.assign(s.image, s.image + kImageSize);
        Install(s, &s.build);
        runtime_cheats_start_text("cheat_god_mode=1\n");
        Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_UNAVAILABLE && s.Pristine(), "one changed site byte: Unavailable, nothing written");
        runtime_cheat_toggle(int(cheats::Toggle::GodMode));
        Check(s.Pristine(), "an Unavailable cheat ignores F10");
    }
    {
        Synthetic s;
        s.Protect(PAGE_READWRITE);
        s.image[kGodHp + 0x1a] = 0x90;
        s.pristine.assign(s.image, s.image + kImageSize);
        Install(s, &s.build);
        runtime_cheats_start_text("cheat_god_mode=1\n");
        Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_UNAVAILABLE && s.Pristine(),
              "a changed byte of the method outside the site (fingerprint): Unavailable, nothing written");
    }
    {
        Synthetic s;
        SosCheatBuild moved = s.build;
        moved.sites[SOS_CHEAT_SITE_MAGIC].rva += 1;
        Install(s, &moved);
        runtime_cheats_start_text("cheat_infinite_magic=1\n");
        Check(Toggle(cheats::Toggle::InfiniteMagic) == SOS_CHEAT_UNAVAILABLE && s.Pristine(), "a wrong RVA: Unavailable, nothing written");
    }
    {
        Synthetic s;
        SosCheatBuild other = s.build;
        other.image_size = kImageSize + 0x10;
        Install(s, &other);
        runtime_cheats_start_text("cheat_god_mode=1\ncheat_damage_multiplier=2\n");
        Check(Toggle(cheats::Toggle::GodMode) == SOS_CHEAT_UNAVAILABLE &&
                  MultState(cheats::Multiplier::Damage) == SOS_CHEAT_UNAVAILABLE && s.Pristine(),
              "another build's table: every cheat Unavailable, nothing written");
    }
    {
        Synthetic s;
        Install(s, nullptr);
        runtime_cheats_start_text("cheat_god_mode=1\n");
        Check(!runtime_cheats_available() && s.Pristine(), "an unknown build has no cheats and nothing is written");
    }
    {
        Synthetic s;
        SosCheatDetour detour{};
        Check(!sos_cheat_detour_prepare(&detour, s.image, &s.build.sites[SOS_CHEAT_SITE_GOD_HP], reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(s.image) + 0x7fffffff00ull)),
              "a stub out of rel32 reach is refused");
        SosCheatSite crossing = s.build.sites[SOS_CHEAT_SITE_GOD_HP];
        crossing.rva += 0xd;
        const char* why = nullptr;
        SosCheatBuild cross = s.build;
        cross.sites[SOS_CHEAT_SITE_GOD_HP] = crossing;
        Check(!sos_cheat_site_verify(s.image, kImageSize, &cross, SOS_CHEAT_SITE_GOD_HP, &why), "a site crossing a 16-byte block is refused");
    }
}

// ---- real boot images -------------------------------------------------------------------------------------------------

bool ReadImage(const fs::path& boot, std::vector<uint8_t>& image, uint64_t& size) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    uint64_t header[5] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    size = header[0];
    const uint64_t segments = header[2], relocs = header[3], imports = header[4];
    in.seekg(48, std::ios::cur);
    uint64_t modules = 0;
    in.read(reinterpret_cast<char*>(&modules), 8);
    in.seekg(static_cast<std::streamoff>(modules * 56), std::ios::cur);
    uint64_t bindings = 0;
    in.read(reinterpret_cast<char*>(&bindings), 8);
    in.seekg(static_cast<std::streamoff>(bindings * 24 + segments * 24 + imports * 128 + relocs * 32), std::ios::cur);
    image.assign(size, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size)));
}

void TestContract(const std::string& key, const std::string& other) {
    const SosCheatBuild* build = sos_cheat_build(key.c_str());
    const SosCheatBuild* second = sos_cheat_build(other.c_str());
    Check(build != nullptr && std::string(build->build) == key, key + ": has its own cheat table");
    if (!build || !second) return;
    for (int i = 0; i < SOS_CHEAT_SITE_COUNT; ++i) {
        const SosCheatSite& s = build->sites[i];
        Check(s.rva % 16 + s.length <= 16 && s.length >= 5 && s.executed >= 1 && s.executed <= s.length,
              key + ": site " + std::to_string(i) + " is one instruction inside one aligned 16-byte block");
        Check(s.method.begin <= s.rva && s.rva + s.length <= s.method.end && s.method.end <= build->image_size,
              key + ": site " + std::to_string(i) + " lies in its method inside the image");
        if (i != SOS_CHEAT_SITE_WALLET_TICK) {
            Check(s.rva != second->sites[i].rva, key + ": site " + std::to_string(i) + " is not the other build's offset");
        }
        Check(s.length == second->sites[i].length, key + ": site " + std::to_string(i) + " is the same instruction in both builds");
    }
    Check(build->image_size != second->image_size, key + ": the image size tells the builds apart");
    Check(sos_cheat_build("CUSA57767-9.99") == nullptr && sos_cheat_build(nullptr) == nullptr, "an unknown build has no table");
    sos_test::ReportContract(key, "cheat site table: one aligned instruction per site, distinct offsets per build");
}

void TestRealImage(const std::string& key, const fs::path& boot) {
    std::vector<uint8_t> image;
    uint64_t size = 0;
    if (!ReadImage(boot, image, size)) {
        Check(false, key + ": boot image readable");
        return;
    }
    const SosCheatBuild* build = sos_cheat_build(key.c_str());
    Check(build && size == build->image_size, key + ": the image is the size the table was made for");
    if (!build) return;
    const char* why = nullptr;
    for (int i = 0; i < SOS_CHEAT_SITE_COUNT; ++i) {
        Check(sos_cheat_site_verify(image.data(), size, build, i, &why), key + ": site " + std::to_string(i) + " verifies on the real code");
        const uint32_t at = build->sites[i].rva;
        image[at] ^= 0x01;
        Check(!sos_cheat_site_verify(image.data(), size, build, i, &why), key + ": site " + std::to_string(i) + " with one byte changed is refused");
        image[at] ^= 0x01;
        SosCheatBuild shifted = *build;
        shifted.sites[i].rva += 1;
        Check(!sos_cheat_site_verify(image.data(), size, &shifted, i, &why), key + ": site " + std::to_string(i) + " at a wrong RVA is refused");
    }
    int all[SOS_CHEAT_FN_COUNT + 1];
    for (int i = 0; i < SOS_CHEAT_FN_COUNT; ++i) all[i] = i;
    all[SOS_CHEAT_FN_COUNT] = -1;
    Check(sos_cheat_functions_verify(image.data(), size, build, all, SOS_CHEAT_FN_COUNT + 1, &why),
          key + ": every game function the handlers call is the analysed code");
    const SosCheatSite& move = build->sites[SOS_CHEAT_SITE_MOVEMENT];
    const uint8_t input_delta[] = {0xc5, 0xfb, 0x10, 0x83, 0xe0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0x31, 0xf6};
    int32_t rel = 0;
    std::memcpy(&rel, image.data() + move.rva + 1, 4);
    Check(std::memcmp(image.data() + move.rva - sizeof(input_delta), input_delta, sizeof(input_delta)) == 0 &&
              move.rva + 5 + rel == build->functions[SOS_CHEAT_FN_SLOPE_ALIGNED].begin,
          key + ": Movement Speed hooks only the input delta's slope call (gravity, knockback, root motion are added after it)");
    const SosCheatSite& jump = build->sites[SOS_CHEAT_SITE_JUMP];
    std::memcpy(&rel, image.data() + jump.rva - 4, 4);
    Check(image[jump.rva - 5] == 0xe8 && jump.rva + rel == build->functions[SOS_CHEAT_FN_JUMP_ANGLE].begin,
          key + ": Jump Height follows the jump coroutine's own angle call, which scripted launches do not use");

    const std::string other = key == "CUSA57767-1.00" ? "CUSA57767-1.01" : "CUSA57767-1.00";
    const SosCheatBuild* wrong = sos_cheat_build(other.c_str());
    if (wrong) {
        SosCheatBuild same_size = *wrong;
        same_size.image_size = size;
        bool refused = true;
        for (int i = 0; i < SOS_CHEAT_SITE_COUNT; ++i)
            refused &= !sos_cheat_site_verify(image.data(), size, &same_size, i, &why);
        Check(refused, key + ": the " + other + " sites are refused on this image");
    }

    // The whole chain on a copy of the real image: every site goes in and out byte for byte.
    const size_t stubs_at = (size + 0xfff) & ~size_t(0xfff);
    uint8_t* copy = static_cast<uint8_t*>(VirtualAlloc(nullptr, stubs_at + SOS_CHEAT_STUB_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!copy) {
        Check(false, key + ": memory for a copy of the image");
        return;
    }
    std::memcpy(copy, image.data(), size);
    char message[600] = "";
    runtime_cheats_install_build(copy, size, build, copy + stubs_at, SOS_CHEAT_STUB_BYTES, nullptr, message, sizeof(message));
    Check(std::string(message).find("7 of 7 ready for " + key) != std::string::npos, key + ": " + message);
    runtime_cheats_start_text("cheat_god_mode=1\ncheat_infinite_spirit=1\ncheat_infinite_magic=1\ncheat_damage_multiplier=6\n"
                              "cheat_movement_speed=2\ncheat_jump_height=6\n");
    std::set<uint32_t> changed;
    for (uint64_t i = 0; i < size; ++i)
        if (copy[i] != image[i]) changed.insert(uint32_t(i));
    std::set<uint32_t> expected;
    for (int i = 0; i < SOS_CHEAT_SITE_COUNT; ++i) {
        if (i == SOS_CHEAT_SITE_WALLET_TICK) continue;
        for (uint32_t b = 0; b < build->sites[i].length; ++b)
            if (copy[build->sites[i].rva + b] != image[build->sites[i].rva + b]) expected.insert(build->sites[i].rva + b);
        Check(copy[build->sites[i].rva] == 0xe9, key + ": site " + std::to_string(i) + " jumps to its stub while On");
    }
    Check(changed == expected && !changed.empty(), key + ": turning every cheat On changes only the site bytes");
    runtime_cheats_start_text("");
    Check(std::memcmp(copy, image.data(), size) == 0, key + ": every cheat Off restores the real image byte for byte");
    VirtualFree(copy, 0, MEM_RELEASE);
}

bool SafeSandbox(const fs::path& sandbox, const fs::path& root) {
    std::error_code error;
    if (sandbox.empty() || !sandbox.is_absolute() || !fs::is_directory(sandbox, error)) return false;
    const fs::path canonical = fs::weakly_canonical(sandbox, error);
    const fs::path repo = fs::weakly_canonical(root, error);
    if (canonical.filename().string().rfind("sos_cheats_", 0) != 0) return false;
    const std::string c = canonical.generic_string(), r = repo.generic_string();
    return c.rfind(r, 0) != 0 && r.rfind(c, 0) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path root = SOS_SOURCE_DIR;
    const fs::path sandbox = argc > 1 ? fs::path(argv[1]) : fs::path();
    if (!SafeSandbox(sandbox, root)) {
        std::printf("  [fail] the sandbox folder is missing or unsafe: '%s'\n", sandbox.string().c_str());
        return 1;
    }
    TestModel();
    TestRules();
    TestMenu();
    TestPlacement();
    TestController();
    TestSyntheticRuntime(sandbox);
    TestControllerOnRuntime();
    TestRegisters();
    TestEngine();
    TestRefusals();
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const std::string other = std::string(key) == "CUSA57767-1.00" ? "CUSA57767-1.01" : "CUSA57767-1.00";
        TestContract(key, other);
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "cheat sites, game functions and the patch round trip");
        if (boot.found()) TestRealImage(key, boot.path);
    }
    std::printf("Cheats: %d checks, %d failures (CUSA57767 1.00 and CUSA57767 1.01)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
