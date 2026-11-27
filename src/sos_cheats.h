// The cheats: one list for the launcher page, the F10 menu and the saved settings, the session layer the menu changes,
// and the rules the game hooks apply. Logic only: no game memory, no window.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>

namespace sos::cheats {

enum class Toggle : uint8_t { GodMode, InfiniteSpirit, InfiniteMagic };
inline constexpr size_t kToggleCount = 3;

enum class Multiplier : uint8_t { Damage, MovementSpeed, JumpHeight };
inline constexpr size_t kMultiplierCount = 3;

enum class Action : uint8_t { AddBloodOrbs, AddUpgradeMaterials };
inline constexpr size_t kActionCount = 2;

constexpr size_t Index(Toggle toggle) { return static_cast<size_t>(toggle); }
constexpr size_t Index(Multiplier multiplier) { return static_cast<size_t>(multiplier); }
constexpr size_t Index(Action action) { return static_cast<size_t>(action); }

struct ToggleInfo {
    Toggle toggle;
    const char* key;
    const char* label;
    const char* help;
};

inline constexpr ToggleInfo kToggles[] = {
    {Toggle::GodMode, "cheat_god_mode", "God Mode", "Keeps the player's health full; damage and grey health take nothing"},
    {Toggle::InfiniteSpirit, "cheat_infinite_spirit", "Infinite Spartan Spirit", "Keeps the player's Spartan Spirit full"},
    {Toggle::InfiniteMagic, "cheat_infinite_magic", "Infinite Magic", "Keeps the player's magic full"},
};
static_assert(std::size(kToggles) == kToggleCount);

struct Choice {
    int value;
    const char* saved;
    const char* label;
};

inline constexpr int kOff = 1;

inline constexpr Choice kDamageChoices[] = {{1, "1", "Off"}, {2, "2", "2x"}, {4, "4", "4x"}, {6, "6", "6x"}};
inline constexpr Choice kMovementChoices[] = {{1, "1", "Off"}, {2, "2", "2x"}};
inline constexpr Choice kJumpChoices[] = {{1, "1", "Off"}, {2, "2", "2x"}, {4, "4", "4x"}, {6, "6", "6x"}};

struct MultiplierInfo {
    Multiplier multiplier;
    const char* key;
    const char* label;
    const char* help;
    std::span<const Choice> choices;
};

inline constexpr MultiplierInfo kMultipliers[] = {
    {Multiplier::Damage, "cheat_damage_multiplier", "Damage Multiplier", "Multiplies the damage the player deals to enemies",
     kDamageChoices},
    {Multiplier::MovementSpeed, "cheat_movement_speed", "Movement Speed", "Multiplies the player's own walking and running",
     kMovementChoices},
    {Multiplier::JumpHeight, "cheat_jump_height", "Jump Height", "Multiplies the height of the player's jumps", kJumpChoices},
};
static_assert(std::size(kMultipliers) == kMultiplierCount);

struct ActionInfo {
    Action action;
    const char* label;
    const char* done;
};

inline constexpr ActionInfo kActions[] = {
    {Action::AddBloodOrbs, "Add 1000 Blood Orbs", "+1000"},
    {Action::AddUpgradeMaterials, "Add 10 Upgrade Materials", "+10"},
};
static_assert(std::size(kActions) == kActionCount);

inline constexpr char kUnavailable[] = "Unavailable";
inline constexpr char kBusy[] = "Busy";
inline constexpr char kFailed[] = "Failed";
inline constexpr char kPending[] = "Pending";

inline constexpr char kLauncherHelp[] =
    "Defaults for the next game. Press F10 while playing to change them for that session only; those changes are not saved.";
inline constexpr char kActionsHelp[] = "Add 1000 Blood Orbs and Add 10 Upgrade Materials are in the F10 menu, while playing.";

constexpr const ToggleInfo& Info(Toggle toggle) { return kToggles[Index(toggle)]; }
constexpr const MultiplierInfo& Info(Multiplier multiplier) { return kMultipliers[Index(multiplier)]; }
constexpr const ActionInfo& Info(Action action) { return kActions[Index(action)]; }

constexpr bool IsListed(std::span<const Choice> choices, int value) {
    for (const Choice& choice : choices) {
        if (choice.value == value) {
            return true;
        }
    }
    return false;
}

constexpr int Listed(Multiplier multiplier, int value) { return IsListed(Info(multiplier).choices, value) ? value : kOff; }

constexpr const char* ChoiceLabel(Multiplier multiplier, int value) {
    for (const Choice& choice : Info(multiplier).choices) {
        if (choice.value == value) {
            return choice.label;
        }
    }
    return Info(multiplier).choices[0].label;
}

// The next value in list order; after the last one it is Off again.
constexpr int NextChoice(Multiplier multiplier, int value) {
    const std::span<const Choice> choices = Info(multiplier).choices;
    size_t index = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].value == value) {
            index = i;
        }
    }
    return choices[(index + 1) % choices.size()].value;
}

// The launcher's values: the defaults of every session.
struct Saved {
    std::array<bool, kToggleCount> toggles{};
    std::array<int, kMultiplierCount> multipliers{kOff, kOff, kOff};

    bool operator==(const Saved&) const = default;
};

constexpr std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

constexpr bool TrueValue(std::string_view value) { return value == "1" || value == "true" || value == "on"; }

constexpr int ParseInt(std::string_view value) {
    int result = 0;
    if (value.empty() || value.size() > 3) {
        return -1;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return -1;
        }
        result = result * 10 + (c - '0');
    }
    return result;
}

// The settings file's text; a missing key, an unlisted value or a malformed line keeps the default (every cheat Off).
constexpr Saved ParseSaved(std::string_view text) {
    Saved saved;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const size_t equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = Trim(line.substr(0, equals));
        std::string_view value = Trim(line.substr(equals + 1));
        if (const size_t comment = value.find(" #"); comment != std::string_view::npos) {
            value = Trim(value.substr(0, comment));
        }
        for (const ToggleInfo& info : kToggles) {
            if (key == info.key) {
                saved.toggles[Index(info.toggle)] = TrueValue(value);
            }
        }
        for (const MultiplierInfo& info : kMultipliers) {
            if (key == info.key) {
                saved.multipliers[Index(info.multiplier)] = Listed(info.multiplier, ParseInt(value));
            }
        }
    }
    return saved;
}

// What the F10 menu changed this session, kept as a difference from the saved value: the saved value is never altered,
// and a cheat changed back follows the launcher again.
class Session {
public:
    bool Effective(Toggle toggle, const Saved& saved) const {
        const int state = toggles_[Index(toggle)].load(std::memory_order_relaxed);
        return state == kFollowsSaved ? saved.toggles[Index(toggle)] : state == kOn;
    }

    int Effective(Multiplier multiplier, const Saved& saved) const {
        const int value = multipliers_[Index(multiplier)].load(std::memory_order_relaxed);
        return value == kFollowsSaved ? saved.multipliers[Index(multiplier)] : value;
    }

    void Set(Toggle toggle, const Saved& saved, bool on) {
        toggles_[Index(toggle)].store(on == saved.toggles[Index(toggle)] ? kFollowsSaved : (on ? kOn : kOffState),
                                      std::memory_order_relaxed);
    }

    void Set(Multiplier multiplier, const Saved& saved, int value) {
        const int listed = Listed(multiplier, value);
        multipliers_[Index(multiplier)].store(listed == saved.multipliers[Index(multiplier)] ? kFollowsSaved : listed,
                                              std::memory_order_relaxed);
    }

    bool Overridden() const {
        for (const auto& state : toggles_) {
            if (state.load(std::memory_order_relaxed) != kFollowsSaved) {
                return true;
            }
        }
        for (const auto& value : multipliers_) {
            if (value.load(std::memory_order_relaxed) != kFollowsSaved) {
                return true;
            }
        }
        return false;
    }

    void Reset() {
        for (auto& state : toggles_) {
            state.store(kFollowsSaved, std::memory_order_relaxed);
        }
        for (auto& value : multipliers_) {
            value.store(kFollowsSaved, std::memory_order_relaxed);
        }
    }

private:
    static constexpr int kFollowsSaved = -1;
    static constexpr int kOffState = 0;
    static constexpr int kOn = 1;
    std::array<std::atomic<int>, kToggleCount> toggles_{kFollowsSaved, kFollowsSaved, kFollowsSaved};
    std::array<std::atomic<int>, kMultiplierCount> multipliers_{kFollowsSaved, kFollowsSaved, kFollowsSaved};
};

// God Mode: the green bar is current minus grey health, so both stay full; a change that would shrink it is dropped.
struct Vitals {
    float hp, grey, amount;
};

constexpr Vitals HeldHealth(bool on, bool player, float max_hp, Vitals in) {
    if (!on || !player) {
        return in;
    }
    const float amount = in.amount < 0.0f || in.amount != in.amount ? 0.0f : in.amount;
    return {max_hp, 0.0f, amount};
}

constexpr Vitals HeldGreyHealth(bool on, bool player, float max_hp, Vitals in) {
    if (!on || !player) {
        return in;
    }
    const float amount = in.amount > 0.0f || in.amount != in.amount ? 0.0f : in.amount;
    return {max_hp, 0.0f, amount};
}

struct Meter {
    float current, delta;
};

// Infinite Spartan Spirit: the meter is held at its maximum; a loss becomes 0, a gain goes through (and stops at the max).
constexpr Meter HeldSpirit(bool on, bool player, float max, Meter in) {
    if (!on || !player) {
        return in;
    }
    return {max, in.delta < 0.0f || in.delta != in.delta ? 0.0f : in.delta};
}

// Infinite Magic: the value OffsetMana stores becomes the maximum, so a cost takes nothing and a gain leaves it full.
constexpr float HeldMana(bool on, bool player, float max, float stored) { return on && player ? max : stored; }

// Damage Multiplier: only a hit the player deals to something that is not a player, with a finite nonzero damage the
// game lets modifiers change, is multiplied; the product must be finite too.
constexpr bool MultipliesHit(int factor, bool source_player, bool target_player, bool unmodifiable, float damage) {
    if (factor <= kOff || !source_player || target_player || unmodifiable || damage == 0.0f) {
        return false;
    }
    const float product = damage * static_cast<float>(factor);
    return damage == damage && product == product && product - product == 0.0f && damage - damage == 0.0f;
}

constexpr float MultipliedDamage(int factor, float damage) { return damage * static_cast<float>(factor); }

// Movement Speed: the movement the player's input makes this frame, before gravity, knockback and root motion are added.
struct Vector2 {
    float x, y;
};

constexpr Vector2 InputMovement(int speed, bool player, Vector2 delta) {
    if (speed <= kOff || !player) {
        return delta;
    }
    return {delta.x * static_cast<float>(speed), delta.y * static_cast<float>(speed)};
}

// Jump Height: a jump's height grows with the square of its upward speed, so the jump's own impulse direction is scaled
// by the square root of the factor. Only upward jumps of the player change.
inline float JumpScale(int factor) { return factor <= kOff ? 1.0f : std::sqrt(static_cast<float>(factor)); }

inline Vector2 JumpDirection(int factor, bool player, Vector2 direction) {
    if (factor <= kOff || !player || !(direction.y > 0.0f)) {
        return direction;
    }
    return {direction.x, direction.y * JumpScale(factor)};
}

// Item actions add through the wallet, never by writing a total.
inline constexpr char kBloodOrbId[] = "loot_orb_red";
inline constexpr int64_t kBloodOrbAmount = 1000;
// The upgrade materials without the loot_boss_* trophies, which only their boss gives.
inline constexpr const char* kUpgradeMaterialIds[] = {
    "loot_common_shield",   "loot_rare_shield",     "loot_epic_shield",    "loot_common_spear",         "loot_rare_spear",
    "loot_epic_spear_grip", "loot_epic_spear_tail", "loot_epic_spear_tip", "loot_epic_spear_tip_status",
};
inline constexpr int64_t kUpgradeMaterialAmount = 10;

constexpr bool IsBossTrophy(std::string_view id) { return id.starts_with("loot_boss_"); }

inline constexpr const char* kBloodOrbIds[] = {kBloodOrbId};

constexpr std::span<const char* const> ActionItems(Action action) {
    return action == Action::AddBloodOrbs ? std::span<const char* const>(kBloodOrbIds)
                                          : std::span<const char* const>(kUpgradeMaterialIds);
}

constexpr int64_t ActionAmount(Action action) {
    return action == Action::AddBloodOrbs ? kBloodOrbAmount : kUpgradeMaterialAmount;
}

// An addition that would pass the count's largest value is refused rather than wrapped.
constexpr bool AdditionFits(int64_t current, int64_t amount) {
    return amount > 0 && current <= std::numeric_limits<int64_t>::max() - amount;
}

} // namespace sos::cheats
