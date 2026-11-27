#include "runtime_cheats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

#include "sos_cheats.h"

namespace {

using namespace sos::cheats;

enum UnitId { kGod, kSpirit, kMagic, kDamage, kMovement, kJump, kWallet, kUnitCount };

constexpr int kMaxSites = 2;
constexpr int kMaxFunctions = 8;
constexpr size_t kNativeObject = 0x10;
constexpr size_t kMaxHp = 0x38, kCurrentHp = 0x40, kGreyHp = 0x44;
constexpr size_t kCurrentSpirit = 0x20, kMaxSpirit = 0x24;
constexpr size_t kMaxMana = 0x2c;
constexpr size_t kHitSource = 0x10, kHitTarget = 0x20;
constexpr size_t kDamageConfig = 0x10, kDamageValue = 0x10, kUnmodifiableDamage = 0x35;
constexpr size_t kApplyModuleSlot = 0x178;
constexpr size_t kJumpContext = 0x58;
constexpr size_t kWalletCount = 0x20;
constexpr uint64_t kActionWaitMs = 1000;
constexpr uint64_t kNoticeMs = 2000;

struct UnitSpec {
    const char* name;
    int sites[kMaxSites];
    int site_count;
    int functions[kMaxFunctions];
    int function_count;
};

constexpr UnitSpec kSpecs[kUnitCount] = {
    {"God Mode", {SOS_CHEAT_SITE_GOD_HP, SOS_CHEAT_SITE_GOD_GREY}, 2, {SOS_CHEAT_FN_GET_GAME_OBJECT, SOS_CHEAT_FN_IS_PLAYER}, 2},
    {"Infinite Spartan Spirit", {SOS_CHEAT_SITE_SPIRIT}, 1, {SOS_CHEAT_FN_GET_GAME_OBJECT, SOS_CHEAT_FN_IS_PLAYER}, 2},
    {"Infinite Magic", {SOS_CHEAT_SITE_MAGIC}, 1, {SOS_CHEAT_FN_GET_GAME_OBJECT, SOS_CHEAT_FN_IS_PLAYER}, 2},
    {"Damage Multiplier", {SOS_CHEAT_SITE_DAMAGE}, 1, {SOS_CHEAT_FN_GET_GAME_OBJECT, SOS_CHEAT_FN_IS_PLAYER}, 2},
    {"Movement Speed", {SOS_CHEAT_SITE_MOVEMENT}, 1,
     {SOS_CHEAT_FN_GET_GAME_OBJECT, SOS_CHEAT_FN_IS_PLAYER, SOS_CHEAT_FN_SLOPE_ALIGNED}, 3},
    {"Jump Height", {SOS_CHEAT_SITE_JUMP}, 1, {SOS_CHEAT_FN_GET_GAME_OBJECT, SOS_CHEAT_FN_IS_PLAYER, SOS_CHEAT_FN_JUMP_ANGLE}, 3},
    {"Item actions", {SOS_CHEAT_SITE_WALLET_TICK}, 1,
     {SOS_CHEAT_FN_GET_PAWN, SOS_CHEAT_FN_PLAYER_INDEX, SOS_CHEAT_FN_TRY_GET_WALLET, SOS_CHEAT_FN_ITEM_BY_ID,
      SOS_CHEAT_FN_WALLET_AMOUNT, SOS_CHEAT_FN_WALLET_ADD, -1},
     7},
};

struct Unit {
    SosCheatDetour detours[kMaxSites]{};
    int detour_count = 0;
    bool available = false;
    const char* why = "not a supported game build";
    std::atomic<bool> active{false};
    std::atomic<int> factor{kOff};
    bool failed = false;
};

Unit units[kUnitCount];
SosCheatBindings game{};
Saved saved;
Session session;
std::recursive_mutex lock;
std::atomic<bool> used{false};
std::atomic<bool> any_available{false};

constexpr UnitId UnitOf(Toggle toggle) {
    switch (toggle) {
    case Toggle::GodMode: return kGod;
    case Toggle::InfiniteSpirit: return kSpirit;
    case Toggle::InfiniteMagic: return kMagic;
    }
    return kGod;
}

constexpr UnitId UnitOf(Multiplier multiplier) {
    switch (multiplier) {
    case Multiplier::Damage: return kDamage;
    case Multiplier::MovementSpeed: return kMovement;
    case Multiplier::JumpHeight: return kJump;
    }
    return kDamage;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename T> T& Field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
}

bool Alive(void* object) { return object != nullptr && Field<void*>(object, kNativeObject) != nullptr; }

bool PlayerObject(void* game_object) { return Alive(game_object) && game.is_player(game_object, nullptr) != 0; }

bool OnPlayer(void* component) { return Alive(component) && PlayerObject(game.get_game_object(component, nullptr)); }

void MarkUsed() {
    if (!used.exchange(true)) {
        std::printf("Cheats: used this session\n");
    }
}

// Every site of one cheat goes in or out together: a site that cannot be written rolls the earlier ones back.
SosCheatWrite Transition(Unit& unit, bool install) {
    int written = 0;
    SosCheatWrite result = SOS_CHEAT_WRITE_DONE;
    for (; written < unit.detour_count; ++written) {
        result = sos_cheat_detour_write(&unit.detours[written], install);
        if (result != SOS_CHEAT_WRITE_DONE) {
            break;
        }
    }
    if (result != SOS_CHEAT_WRITE_DONE) {
        for (int i = 0; i < written; ++i) {
            sos_cheat_detour_write(&unit.detours[i], !install);
        }
    }
    return result;
}

bool Installed(const Unit& unit) {
    for (int i = 0; i < unit.detour_count; ++i) {
        if (sos_cheat_detour_state(&unit.detours[i]) != 0) {
            return true;
        }
    }
    return false;
}

const char* WriteFailure(SosCheatWrite result) {
    return result == SOS_CHEAT_WRITE_PROTECT ? "the game code could not be made writable"
                                             : "the game code at a site is not the original";
}

// The cheat stops taking effect before its code is removed, so a call already in flight behaves as Off. A restore that
// fails leaves the cheat Failed, never Off.
bool Enable(UnitId id) {
    Unit& unit = units[id];
    if (unit.active.load()) {
        return true;
    }
    if (!unit.available) {
        return false;
    }
    if (unit.failed) {
        unit.failed = false;
        unit.active.store(true);
        if (id != kWallet) {
            MarkUsed();
        }
        return true;
    }
    const SosCheatWrite result = Transition(unit, true);
    if (result != SOS_CHEAT_WRITE_DONE) {
        std::printf("Cheats: %s not turned on: %s\n", kSpecs[id].name, WriteFailure(result));
        if (result == SOS_CHEAT_WRITE_MISMATCH) {
            unit.available = false;
            unit.why = "the game code at a site changed";
        }
        unit.failed = Installed(unit);
        return false;
    }
    unit.active.store(true);
    if (id != kWallet) {
        MarkUsed();
    }
    return true;
}

void Disable(UnitId id) {
    Unit& unit = units[id];
    const bool was = unit.active.exchange(false);
    if (!was && !unit.failed) {
        return;
    }
    const SosCheatWrite result = Transition(unit, false);
    unit.failed = result != SOS_CHEAT_WRITE_DONE && Installed(unit);
    if (unit.failed) {
        std::printf("Cheats: %s is still patched: %s; it has no effect\n", kSpecs[id].name, WriteFailure(result));
    }
}

bool SetToggleUnit(UnitId id, bool on) {
    if (on) {
        return Enable(id);
    }
    Disable(id);
    return true;
}

bool SetFactorUnit(UnitId id, int value) {
    Unit& unit = units[id];
    if (value <= kOff) {
        unit.factor.store(kOff);
        Disable(id);
        return true;
    }
    if (!Enable(id)) {
        return false;
    }
    unit.factor.store(value);
    return true;
}

int StateOf(UnitId id) {
    const Unit& unit = units[id];
    if (unit.failed) {
        return SOS_CHEAT_FAILED;
    }
    if (!unit.available) {
        return SOS_CHEAT_UNAVAILABLE;
    }
    return unit.active.load() ? SOS_CHEAT_ON : SOS_CHEAT_OFF;
}

std::string ActiveSummary() {
    std::string out;
    const auto add = [&out](const std::string& text) { out += (out.empty() ? "" : ", ") + text; };
    for (const ToggleInfo& info : kToggles) {
        if (StateOf(UnitOf(info.toggle)) == SOS_CHEAT_ON) {
            add(info.label);
        }
    }
    for (const MultiplierInfo& info : kMultipliers) {
        const UnitId id = UnitOf(info.multiplier);
        if (StateOf(id) == SOS_CHEAT_ON) {
            add(std::string(info.label) + " " + ChoiceLabel(info.multiplier, units[id].factor.load()));
        }
    }
    return out;
}

// ---- handlers: game thread ----------------------------------------------------------------------------------------

SOS_CHEAT_SYSV int GodHp(SosCheatContext* context, SosCheatVector* vectors) {
    void* vitals = reinterpret_cast<void*>(context->rbx);
    if (!units[kGod].active.load(std::memory_order_relaxed) || !OnPlayer(vitals)) {
        return 0;
    }
    const Vitals held = HeldHealth(true, true, Field<float>(vitals, kMaxHp),
                                   {Field<float>(vitals, kCurrentHp), Field<float>(vitals, kGreyHp), vectors[0].f[0]});
    Field<float>(vitals, kCurrentHp) = held.hp;
    Field<float>(vitals, kGreyHp) = held.grey;
    vectors[0].f[0] = held.amount;
    return 0;
}

SOS_CHEAT_SYSV int GodGrey(SosCheatContext* context, SosCheatVector* vectors) {
    void* vitals = reinterpret_cast<void*>(context->rbx);
    if (!units[kGod].active.load(std::memory_order_relaxed) || !OnPlayer(vitals)) {
        return 0;
    }
    const Vitals held = HeldGreyHealth(true, true, Field<float>(vitals, kMaxHp),
                                       {Field<float>(vitals, kCurrentHp), Field<float>(vitals, kGreyHp), vectors[0].f[0]});
    Field<float>(vitals, kCurrentHp) = held.hp;
    Field<float>(vitals, kGreyHp) = held.grey;
    vectors[0].f[0] = held.amount;
    return 0;
}

SOS_CHEAT_SYSV int Spirit(SosCheatContext* context, SosCheatVector* vectors) {
    void* spirit = reinterpret_cast<void*>(context->rbx);
    if (!units[kSpirit].active.load(std::memory_order_relaxed) || !OnPlayer(spirit)) {
        return 0;
    }
    const Meter held = HeldSpirit(true, true, Field<float>(spirit, kMaxSpirit),
                                  {Field<float>(spirit, kCurrentSpirit), vectors[1].f[0]});
    Field<float>(spirit, kCurrentSpirit) = held.current;
    vectors[1].f[0] = held.delta;
    return 0;
}

SOS_CHEAT_SYSV int Magic(SosCheatContext* context, SosCheatVector* vectors) {
    void* mana = reinterpret_cast<void*>(context->rbx);
    if (!units[kMagic].active.load(std::memory_order_relaxed) || !OnPlayer(mana)) {
        return 0;
    }
    vectors[0].f[0] = HeldMana(true, true, Field<float>(mana, kMaxMana), vectors[0].f[0]);
    return 0;
}

using ApplyModule = void(SOS_CHEAT_SYSV*)(void* module, void* args, const void* method);
std::atomic<int> damage_depth{0};

// Replaces the hit's damageModule.ApplyModule(eventModuleArgs) call: the damage is multiplied for this call only and
// restored after it, so a config shared by other hits is never left changed. A hit inside it is not multiplied again.
SOS_CHEAT_SYSV int Damage(SosCheatContext* context, SosCheatVector*) {
    const int factor = units[kDamage].factor.load(std::memory_order_relaxed);
    void* module = reinterpret_cast<void*>(context->rdi);
    void* hit = reinterpret_cast<void*>(context->rbx);
    if (!units[kDamage].active.load(std::memory_order_relaxed) || factor <= kOff || module == nullptr ||
        damage_depth.load(std::memory_order_relaxed) != 0) {
        return 0;
    }
    void* config = Field<void*>(module, kDamageConfig);
    if (config == nullptr) {
        return 0;
    }
    float& damage = Field<float>(config, kDamageValue);
    void* target = Field<void*>(hit, kHitTarget);
    const bool target_player = !Alive(target) || OnPlayer(target);
    if (!MultipliesHit(factor, PlayerObject(Field<void*>(hit, kHitSource)), target_player,
                       Field<uint8_t>(config, kUnmodifiableDamage) != 0, damage)) {
        return 0;
    }
    const auto apply = *reinterpret_cast<ApplyModule*>(context->rax + kApplyModuleSlot);
    const float original = damage;
    damage = MultipliedDamage(factor, original);
    damage_depth.fetch_add(1, std::memory_order_relaxed);
    apply(module, reinterpret_cast<void*>(context->rsi), reinterpret_cast<void*>(context->rdx));
    damage_depth.fetch_sub(1, std::memory_order_relaxed);
    damage = original;
    return 1;
}

// Replaces TickPseudoPhysics's call to GetSlopeAlignedVersionOfMovementDelta with the input delta, so only the input
// movement changes: gravity, knockback, scripted movement and root motion are added after it.
SOS_CHEAT_SYSV int Movement(SosCheatContext* context, SosCheatVector* vectors) {
    const int speed = units[kMovement].factor.load(std::memory_order_relaxed);
    if (!units[kMovement].active.load(std::memory_order_relaxed) || speed <= kOff) {
        return 0;
    }
    void* movement = reinterpret_cast<void*>(context->rdi);
    const SosVector2 aligned = game.slope_aligned(movement, {vectors[0].f[0], vectors[0].f[1]},
                                                  reinterpret_cast<void*>(context->rsi));
    const Vector2 result = InputMovement(speed, OnPlayer(movement), {aligned.x, aligned.y});
    vectors[0].f[0] = result.x;
    vectors[0].f[1] = result.y;
    return 1;
}

// After the jump coroutine's GetJumpAngleAsForwardDirectionVector: its result times the jump magnitude is the jump's
// one impulse. Scripted launches do not come here.
SOS_CHEAT_SYSV int Jump(SosCheatContext* context, SosCheatVector* vectors) {
    const int factor = units[kJump].factor.load(std::memory_order_relaxed);
    if (!units[kJump].active.load(std::memory_order_relaxed) || factor <= kOff) {
        return 0;
    }
    void* state = reinterpret_cast<void*>(context->rbx);
    const Vector2 direction =
        JumpDirection(factor, OnPlayer(Field<void*>(state, kJumpContext)), {vectors[0].f[0], vectors[0].f[1]});
    vectors[0].f[0] = direction.x;
    vectors[0].f[1] = direction.y;
    return 0;
}

// ---- item actions ----------------------------------------------------------------------------------------------------

enum : int { kIdle, kQueued, kRunning };
std::atomic<int> wallet_state{kIdle};
std::atomic<int> wallet_action{0};
std::atomic<uint64_t> wallet_requested{0};
std::atomic<const char*> notices[kActionCount]{};
std::atomic<uint64_t> notice_until[kActionCount]{};

void Notice(Action action, const char* text) {
    notices[Index(action)].store(text);
    notice_until[Index(action)].store(NowMs() + kNoticeMs);
}

const char* Give(Action action, void* controller, const char** item) {
    void* player = game.get_pawn(controller, nullptr);
    if (!Alive(player)) {
        return "no player";
    }
    void* wallet = game.try_get_wallet(player, nullptr);
    if (wallet == nullptr) {
        return "no player wallet";
    }
    const auto ids = ActionItems(action);
    const int64_t amount = ActionAmount(action);
    void* items[std::size(kUpgradeMaterialIds)] = {};
    for (size_t i = 0; i < ids.size(); ++i) {
        *item = ids[i];
        void* id = game.string_new(ids[i]);
        items[i] = id != nullptr ? game.item_by_id(id, nullptr) : nullptr;
        if (!Alive(items[i])) {
            return "item not found";
        }
        const int64_t current = Field<void*>(wallet, kWalletCount) != nullptr ? game.wallet_amount(wallet, items[i], nullptr) : 0;
        if (!AdditionFits(current, amount)) {
            return "count at the largest number it can hold";
        }
    }
    *item = nullptr;
    const bool announce = action == Action::AddBloodOrbs;
    const SosNullableBool hide{static_cast<uint8_t>(!announce), static_cast<uint8_t>(!announce)};
    for (size_t i = 0; i < ids.size(); ++i) {
        game.wallet_add(wallet, items[i], amount, hide, nullptr);
    }
    return nullptr;
}

void RemoveWalletHook() {
    Disable(kWallet);
    if (units[kWallet].failed) {
        units[kWallet].available = false;
        units[kWallet].why = "PlayerController.Update is still patched";
    }
}

void Finish(Action action, const char* failure, const char* item) {
    if (failure == nullptr) {
        std::printf("Cheats: %s %s\n", Info(action).label, Info(action).done);
        Notice(action, Info(action).done);
        MarkUsed();
    } else {
        std::printf("Cheats: %s not added (%s%s%s)\n", Info(action).label, failure, item ? ": " : "", item ? item : "");
        Notice(action, kUnavailable);
    }
}

// PlayerController.Update, only while an action waits: player 1's controller runs it once.
SOS_CHEAT_SYSV int WalletTick(SosCheatContext* context, SosCheatVector*) {
    void* controller = reinterpret_cast<void*>(context->rdi);
    int state = wallet_state.load();
    if (state != kQueued || !Alive(controller) || game.player_index(controller, nullptr) != 0 ||
        !wallet_state.compare_exchange_strong(state, kRunning)) {
        return 0;
    }
    const Action action = static_cast<Action>(wallet_action.load());
    const char* item = nullptr;
    const char* failure = Give(action, controller, &item);
    {
        const std::lock_guard guard(lock);
        RemoveWalletHook();
    }
    wallet_state.store(kIdle);
    Finish(action, failure, item);
    return 0;
}

void ExpireAction(uint64_t now) {
    int state = wallet_state.load();
    if (state != kQueued || now < wallet_requested.load() + kActionWaitMs || !wallet_state.compare_exchange_strong(state, kIdle)) {
        return;
    }
    const std::lock_guard guard(lock);
    RemoveWalletHook();
    Finish(static_cast<Action>(wallet_action.load()), "no player in the game", nullptr);
}

constexpr SosCheatHandler kHandlers[SOS_CHEAT_SITE_COUNT] = {GodHp, GodGrey, Spirit, Magic, Damage, Movement, Jump, WalletTick};

std::string ReadFile(const char* path) {
    std::ifstream in(path ? path : "", std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

SosCheatBindings ResolveBindings(uint8_t* image, const SosCheatBuild* build) {
    const auto at = [&](int function) { return image + build->functions[function].begin; };
    SosCheatBindings b{};
    b.get_game_object = reinterpret_cast<decltype(b.get_game_object)>(at(SOS_CHEAT_FN_GET_GAME_OBJECT));
    b.is_player = reinterpret_cast<decltype(b.is_player)>(at(SOS_CHEAT_FN_IS_PLAYER));
    b.get_pawn = reinterpret_cast<decltype(b.get_pawn)>(at(SOS_CHEAT_FN_GET_PAWN));
    b.player_index = reinterpret_cast<decltype(b.player_index)>(at(SOS_CHEAT_FN_PLAYER_INDEX));
    b.try_get_wallet = reinterpret_cast<decltype(b.try_get_wallet)>(at(SOS_CHEAT_FN_TRY_GET_WALLET));
    b.item_by_id = reinterpret_cast<decltype(b.item_by_id)>(at(SOS_CHEAT_FN_ITEM_BY_ID));
    b.wallet_amount = reinterpret_cast<decltype(b.wallet_amount)>(at(SOS_CHEAT_FN_WALLET_AMOUNT));
    b.wallet_add = reinterpret_cast<decltype(b.wallet_add)>(at(SOS_CHEAT_FN_WALLET_ADD));
    b.slope_aligned = reinterpret_cast<decltype(b.slope_aligned)>(at(SOS_CHEAT_FN_SLOPE_ALIGNED));
    b.string_new = reinterpret_cast<decltype(b.string_new)>(image + build->string_new);
    return b;
}

} // namespace

extern "C" {

void runtime_cheats_install_build(uint8_t* image, uint64_t size, const SosCheatBuild* build, uint8_t* stubs,
                                  size_t stubs_size, const SosCheatBindings* bindings, char* message, size_t message_size) {
    const std::lock_guard guard(lock);
    int ready = 0;
    std::string unavailable;
    for (int id = 0; id < kUnitCount; ++id) {
        Unit& unit = units[id];
        const UnitSpec& spec = kSpecs[id];
        unit.available = false;
        unit.active.store(false);
        unit.factor.store(kOff);
        unit.failed = false;
        unit.detour_count = 0;
        const char* why = build ? nullptr : "not a supported game build";
        if (!why && !sos_cheat_functions_verify(image, size, build, spec.functions, size_t(spec.function_count), &why)) {
            why = why ? why : "game function not recognised";
        }
        for (int s = 0; !why && s < spec.site_count; ++s) {
            const int site = spec.sites[s];
            uint8_t* stub = stubs + size_t(site) * SOS_CHEAT_STUB_SIZE;
            if (!sos_cheat_site_verify(image, size, build, site, &why)) {
                break;
            }
            if (!stubs || stubs_size < size_t(site + 1) * SOS_CHEAT_STUB_SIZE ||
                !sos_cheat_stub_build(stub, SOS_CHEAT_STUB_SIZE, image, &build->sites[site], kHandlers[site]) ||
                !sos_cheat_detour_prepare(&unit.detours[s], image, &build->sites[site], stub)) {
                why = "no room for its stub near the game code";
                break;
            }
            unit.detour_count = s + 1;
        }
        unit.why = why ? why : "";
        unit.available = why == nullptr;
        if (unit.available) {
            ++ready;
        } else {
            unavailable += std::string(unavailable.empty() ? "" : "; ") + spec.name + ": " + unit.why;
        }
    }
    if (build) {
        game = bindings ? *bindings : ResolveBindings(image, build);
    }
    any_available.store(ready > 0);
    if (message && message_size) {
        std::snprintf(message, message_size, "%d of %d ready for %s%s%s", ready, int(kUnitCount), build ? build->build : "this build",
                      unavailable.empty() ? "" : "; unavailable: ", unavailable.c_str());
    }
}

void runtime_cheats_install(uint8_t* image, uint64_t size, const char* build_key, uint8_t* stubs, size_t stubs_size,
                            char* message, size_t message_size) {
    runtime_cheats_install_build(image, size, sos_cheat_build(build_key), stubs, stubs_size, nullptr, message, message_size);
}

void runtime_cheats_start_text(const char* settings_text) {
    const std::lock_guard guard(lock);
    saved = ParseSaved(settings_text ? settings_text : "");
    session.Reset();
    for (const ToggleInfo& info : kToggles) {
        const bool on = session.Effective(info.toggle, saved);
        if (!SetToggleUnit(UnitOf(info.toggle), on) && on) {
            std::printf("Cheats: %s unavailable: %s\n", info.label, units[UnitOf(info.toggle)].why);
        }
    }
    for (const MultiplierInfo& info : kMultipliers) {
        const int value = session.Effective(info.multiplier, saved);
        if (!SetFactorUnit(UnitOf(info.multiplier), value) && value > kOff) {
            std::printf("Cheats: %s unavailable: %s\n", info.label, units[UnitOf(info.multiplier)].why);
        }
    }
    const std::string active = ActiveSummary();
    std::printf("Cheats: %s\n", active.empty() ? "off" : ("active " + active).c_str());
}

void runtime_cheats_start(const char* settings_file) { runtime_cheats_start_text(ReadFile(settings_file).c_str()); }

int runtime_cheat_saved_toggle(int toggle) {
    const std::lock_guard guard(lock);
    return toggle >= 0 && size_t(toggle) < kToggleCount && saved.toggles[size_t(toggle)] ? 1 : 0;
}

int runtime_cheat_saved_multiplier(int multiplier) {
    const std::lock_guard guard(lock);
    return multiplier >= 0 && size_t(multiplier) < kMultiplierCount ? saved.multipliers[size_t(multiplier)] : kOff;
}

int runtime_cheat_toggle_state(int toggle) {
    return toggle >= 0 && size_t(toggle) < kToggleCount ? StateOf(UnitOf(static_cast<Toggle>(toggle))) : SOS_CHEAT_UNAVAILABLE;
}

int runtime_cheat_multiplier_state(int multiplier) {
    return multiplier >= 0 && size_t(multiplier) < kMultiplierCount ? StateOf(UnitOf(static_cast<Multiplier>(multiplier)))
                                                                     : SOS_CHEAT_UNAVAILABLE;
}

int runtime_cheat_multiplier_value(int multiplier) {
    if (multiplier < 0 || size_t(multiplier) >= kMultiplierCount) {
        return kOff;
    }
    const Unit& unit = units[UnitOf(static_cast<Multiplier>(multiplier))];
    return unit.active.load() ? unit.factor.load() : kOff;
}

int runtime_cheat_action_state(int action) {
    if (action < 0 || size_t(action) >= kActionCount || !units[kWallet].available) {
        return SOS_CHEAT_UNAVAILABLE;
    }
    return wallet_state.load() != kIdle ? SOS_CHEAT_PENDING : SOS_CHEAT_OFF;
}

const char* runtime_cheat_action_notice(int action) {
    if (action < 0 || size_t(action) >= kActionCount || NowMs() >= notice_until[action].load()) {
        return nullptr;
    }
    return notices[action].load();
}

void runtime_cheat_toggle(int toggle) {
    if (toggle < 0 || size_t(toggle) >= kToggleCount) {
        return;
    }
    const std::lock_guard guard(lock);
    const Toggle which = static_cast<Toggle>(toggle);
    const UnitId id = UnitOf(which);
    const bool on = !(units[id].active.load() || units[id].failed);
    if (!SetToggleUnit(id, on)) {
        std::printf("Cheats: %s unavailable: %s\n", Info(which).label, units[id].why);
        return;
    }
    session.Set(which, saved, on);
    std::printf("Cheats: %s %s (this session)\n", Info(which).label,
                StateOf(id) == SOS_CHEAT_FAILED ? kFailed : (on ? "On" : "Off"));
}

void runtime_cheat_cycle(int multiplier) {
    if (multiplier < 0 || size_t(multiplier) >= kMultiplierCount) {
        return;
    }
    const std::lock_guard guard(lock);
    const Multiplier which = static_cast<Multiplier>(multiplier);
    const UnitId id = UnitOf(which);
    const int current = units[id].active.load() ? units[id].factor.load() : kOff;
    const int next = NextChoice(which, current);
    if (!SetFactorUnit(id, next)) {
        std::printf("Cheats: %s unavailable: %s\n", Info(which).label, units[id].why);
        return;
    }
    session.Set(which, saved, next);
    std::printf("Cheats: %s %s (this session)\n", Info(which).label,
                StateOf(id) == SOS_CHEAT_FAILED ? kFailed : ChoiceLabel(which, next));
}

void runtime_cheat_action(int action) {
    if (action < 0 || size_t(action) >= kActionCount) {
        return;
    }
    const Action which = static_cast<Action>(action);
    const uint64_t now = NowMs();
    ExpireAction(now);
    const std::lock_guard guard(lock);
    if (!units[kWallet].available) {
        Notice(which, kUnavailable);
        return;
    }
    if (wallet_state.load() != kIdle) {
        Notice(which, kBusy);
        return;
    }
    wallet_action.store(action);
    wallet_requested.store(now);
    wallet_state.store(kQueued);
    if (!Enable(kWallet)) {
        wallet_state.store(kIdle);
        Finish(which, "the game code could not be patched", nullptr);
    }
}

void runtime_cheats_frame(void) { ExpireAction(NowMs()); }

int runtime_cheats_used(void) { return used.load() ? 1 : 0; }

int runtime_cheats_available(void) { return any_available.load() ? 1 : 0; }

} // extern "C"
