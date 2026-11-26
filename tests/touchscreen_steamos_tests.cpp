// Headless tests of the SteamOS / Gamescope touchscreen route, after Dragon's Crown Pro's (c5a05e2148ed, eb6ac2b67330):
// the production claim (gpu/shim/sos_gamescope_touch.h) against a fake X11 property store with the game's root, Steam's
// Gamescope root and a desktop X server; the policy and telemetry (src/sos_touch_gamescope.h); the touchscreen as the
// DualShock 4 touch surface (src/sos_touchscreen.h); the SDL event routing (gpu/shim/sos_touch_route.h); and the wiring in
// the window, the pad sampler and every exit path. No window, X server, GPU, launcher or game.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "sos_gamescope_touch.h"
#include "sos_touch_route.h"
#include "sos_touchscreen.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

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

std::string Source(const std::string& path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    std::string unix_text;
    for (const char c : text) {
        if (c != '\r') {
            unix_text += c;
        }
    }
    return unix_text;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool InOrder(const std::string& text, const std::vector<std::string>& parts) {
    size_t at = 0;
    for (const std::string& part : parts) {
        at = text.find(part, at);
        if (at == std::string::npos) {
            return false;
        }
        at += part.size();
    }
    return true;
}

// The body of the function whose signature starts at `signature`.
std::string Body(const std::string& text, const std::string& signature) {
    const size_t start = text.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    const size_t brace = text.find('{', start);
    int depth = 0;
    for (size_t at = brace; at < text.size(); ++at) {
        depth += text[at] == '{' ? 1 : text[at] == '}' ? -1 : 0;
        if (depth == 0) {
            return text.substr(brace, at - brace + 1);
        }
    }
    return {};
}

// ---------------------------------------------------------------- fake X11

// One X display: the game's Xwayland, Steam's Gamescope root or a desktop X server. Gamescope keeps one touch mode for
// the whole compositor: the newest STEAM_TOUCH_CLICK_MODE write on any of its roots.
struct FakeDisplay {
    bool answers = true;
    bool gamescope = true; ///< has GAMESCOPE_INPUT_COUNTER
    bool have = false;     ///< the property exists
    std::uint32_t value = 0;
    bool open = false;
    int writes = 0;
};

std::map<std::string, FakeDisplay> g_server;
std::uint32_t g_effective = SOS_GAMESCOPE_TOUCH_LEFT;
std::atomic<int> g_opens{0}, g_closes{0};
// Two threads inside the X calls at once: a release racing the window thread's rewrite (the claim must serialise them).
std::atomic<int> g_inside{0};
std::atomic<bool> g_overlap{false};
std::atomic<bool> g_slow{false};

// A busy wait: Windows sleeps are a timer tick long, far too coarse for a race window.
void Spin(std::chrono::microseconds time) {
    const auto until = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < until) {
    }
}

struct InsideX {
    InsideX() {
        if (g_inside.fetch_add(1) != 0) {
            g_overlap = true;
        }
        if (g_slow.load()) {
            Spin(std::chrono::microseconds(50));
        }
    }
    ~InsideX() {
        g_inside.fetch_sub(1);
    }
};

FakeDisplay* Fake(void* display) {
    return static_cast<FakeDisplay*>(display);
}
void* FakeOpen(const char* name) {
    auto it = g_server.find(name);
    if (it == g_server.end() || !it->second.answers) {
        return nullptr;
    }
    it->second.open = true;
    ++g_opens;
    return &it->second;
}
int FakeClose(void* display) {
    Fake(display)->open = false;
    ++g_closes;
    return 0;
}
unsigned long FakeIntern(void* display, const char* name, int only_if_exists) {
    if (!std::strcmp(name, SOS_GAMESCOPE_TOUCH_MARKER_ATOM)) {
        return Fake(display)->gamescope || !only_if_exists ? 101 : 0;
    }
    return !std::strcmp(name, SOS_GAMESCOPE_TOUCH_ATOM) ? 102 : 0;
}
unsigned long FakeRoot(void*) {
    return 1;
}
int FakeGet(void* display, unsigned long, unsigned long atom, long, long, int, unsigned long, unsigned long* type,
            int* format, unsigned long* count, unsigned long* after, unsigned char** data) {
    const InsideX inside;
    FakeDisplay* const d = Fake(display);
    *after = 0;
    if (atom != 102 || !d->have) {
        *type = 0;
        *format = 0;
        *count = 0;
        *data = nullptr;
        return 0;
    }
    long* word = static_cast<long*>(std::malloc(sizeof(long)));
    *word = long(d->value);
    *type = 6;
    *format = 32;
    *count = 1;
    *data = reinterpret_cast<unsigned char*>(word);
    return 0;
}
int FakeChange(void* display, unsigned long, unsigned long atom, unsigned long type, int format, int mode,
               const unsigned char* data, int elements) {
    const InsideX inside;
    FakeDisplay* const d = Fake(display);
    if (atom != 102 || type != 6 || format != 32 || mode != 0 || elements != 1) {
        return 0;
    }
    long word = 0;
    std::memcpy(&word, data, sizeof(word));
    d->value = std::uint32_t(word);
    d->have = true;
    ++d->writes;
    if (d->gamescope) {
        g_effective = d->value;
    }
    return 1;
}
int FakeFree(void* data) {
    std::free(data);
    return 0;
}
int FakeFlush(void*) {
    return 0;
}

SosGamescopeTouch::XApi FakeApi() {
    SosGamescopeTouch::XApi api;
    api.open = FakeOpen;
    api.close = FakeClose;
    api.intern = FakeIntern;
    api.default_root = FakeRoot;
    api.get = FakeGet;
    api.change = FakeChange;
    api.free = FakeFree;
    api.flush = FakeFlush;
    return api;
}

// Steam writes its own mode on its root (or the game's).
void Steam(const char* display, std::uint32_t value) {
    FakeDisplay& d = g_server[display];
    d.value = value;
    d.have = true;
    if (d.gamescope) {
        g_effective = value;
    }
}

// Gaming Mode: the game's Xwayland (:1, DISPLAY), Steam's Gamescope root (:0) and a desktop X server of this user (:2).
SosGamescopeTouch::Candidates GamingMode(std::uint32_t game = 0, std::uint32_t steam = SOS_GAMESCOPE_TOUCH_LEFT,
                                         bool game_has = false) {
    g_server.clear();
    g_server[":1"] = FakeDisplay{};
    g_server[":1"].have = game_has;
    g_server[":1"].value = game;
    g_server[":0"] = FakeDisplay{};
    g_server[":0"].have = true;
    g_server[":0"].value = steam;
    g_server[":2"] = FakeDisplay{};
    g_server[":2"].gamescope = false;
    g_server[":2"].have = true;
    g_server[":2"].value = 9;
    g_effective = steam;
    SosGamescopeTouch::Candidates list;
    list.Add(":1");
    list.own = 0;
    list.Add(":0");
    list.Add(":2");
    list.Add(":1"); // the socket scan finds the game's own display again: listed once
    return list;
}

SosTouchGamescopeEnv SteamDeck() {
    SosTouchGamescopeEnv env{};
    env.linux_host = 1;
    env.game_window = 1;
    env.gamescope = 1;
    env.display = ":1";
    return env;
}

// ---------------------------------------------------------------- policy

void TestPolicy() {
    SosTouchGamescopeEnv env = SteamDeck();
    Check(sos_touch_gamescope_wanted(&env), "the game window under Gamescope with an X display takes the touchscreen");
    env.opt_out = "0";
    Check(!sos_touch_gamescope_wanted(&env), "SOS_TOUCH_GAMESCOPE=0 keeps Gamescope's own touch handling");
    for (const char* other : {"1", "", "off", "00"}) {
        env.opt_out = other;
        Check(sos_touch_gamescope_wanted(&env), std::string("only \"0\" is the kill switch, not \"") + other + "\"");
    }
    env = SteamDeck();
    env.game_window = 0;
    Check(!sos_touch_gamescope_wanted(&env), "the launcher never claims Passthrough: its taps stay mouse clicks");
    env = SteamDeck();
    env.linux_host = 0;
    Check(!sos_touch_gamescope_wanted(&env), "Windows is unchanged");
    env = SteamDeck();
    env.gamescope = 0;
    Check(!sos_touch_gamescope_wanted(&env), "a desktop Linux session is unchanged");
    env = SteamDeck();
    env.display = nullptr;
    Check(!sos_touch_gamescope_wanted(&env), "no X display: nothing to claim");
    env.display = "";
    Check(!sos_touch_gamescope_wanted(&env), "an empty DISPLAY is none");

    char name[SosGamescopeTouch::kNameSize];
    Check(SosGamescopeTouch::LocalDisplayName(":1", name, sizeof(name)) && !std::strcmp(name, ":1") &&
              SosGamescopeTouch::LocalDisplayName("unix:0.0", name, sizeof(name)) && !std::strcmp(name, ":0") &&
              SosGamescopeTouch::LocalDisplayName(":12.3", name, sizeof(name)) && !std::strcmp(name, ":12"),
          "local display names are read as :N");
    Check(!SosGamescopeTouch::LocalDisplayName("host:0", name, sizeof(name)) &&
              !SosGamescopeTouch::LocalDisplayName(":", name, sizeof(name)) &&
              !SosGamescopeTouch::LocalDisplayName(":1234", name, sizeof(name)) &&
              !SosGamescopeTouch::LocalDisplayName(nullptr, name, sizeof(name)),
          "a remote or malformed display is never contacted");
}

// ---------------------------------------------------------------- claim / restore state machine

void TestClaimOnFocusAndRestore() {
    const SosGamescopeTouch::XApi api = FakeApi();
    const auto candidates = GamingMode();
    SosGamescopeTouch::Claim claim;
    Check(claim.Acquire(&api, candidates, 1000) == SosGamescopeTouch::Result::Applied, "focus claims Passthrough");
    Check(claim.Roots() == 2 && claim.CompositorRoot(), "the game's root and Steam's Gamescope root are claimed");
    Check(g_server[":1"].value == SOS_GAMESCOPE_TOUCH_PASSTHROUGH && g_server[":0"].value == SOS_GAMESCOPE_TOUCH_PASSTHROUGH &&
              g_effective == SOS_GAMESCOPE_TOUCH_PASSTHROUGH,
          "both roots read Passthrough and Gamescope hands the touchscreen over");
    Check(g_server[":2"].value == 9 && g_server[":2"].writes == 0 && !g_server[":2"].open,
          "a desktop X server is opened, found not to be Gamescope, closed and never written");

    // Steam writes Left on its root again: the game takes it back about once a second, not on every poll.
    Steam(":0", SOS_GAMESCOPE_TOUCH_LEFT);
    Check(g_effective == SOS_GAMESCOPE_TOUCH_LEFT, "Steam's write on its own root wins Gamescope's single mode");
    Check(!claim.Maintain(1500) && g_effective == SOS_GAMESCOPE_TOUCH_LEFT, "no rewrite before a second has passed");
    Check(claim.Maintain(2000) && g_effective == SOS_GAMESCOPE_TOUCH_PASSTHROUGH, "the periodic rewrite takes it back");
    const int writes = g_server[":1"].writes;
    Check(!claim.Maintain(2999) && claim.Maintain(3000) && g_server[":1"].writes == writes + 1,
          "and keeps doing so once a second, also on the root that already reads Passthrough");

    // Focus loss: each root gets its own value back (the game's root had none: Gamescope's default Left).
    Check(claim.Release() == 2, "focus loss restores both roots");
    Check(g_server[":1"].value == SOS_GAMESCOPE_TOUCH_LEFT && g_server[":0"].value == SOS_GAMESCOPE_TOUCH_LEFT &&
              g_effective == SOS_GAMESCOPE_TOUCH_LEFT,
          "each root reads its previous value and touch is Gamescope's again");
    Check(!g_server[":0"].open && !g_server[":1"].open && !claim.Claimed() && claim.Roots() == 0,
          "the connections are closed and nothing is held");
    Check(!claim.Maintain(10000), "nothing is rewritten while the game is not focused");
    Check(claim.Release() == 0, "a second release (exit after focus loss) writes nothing");

    // Focus regain claims again from the current values.
    Steam(":0", 2u);
    Check(claim.Acquire(&api, candidates, 20000) == SosGamescopeTouch::Result::Applied &&
              g_effective == SOS_GAMESCOPE_TOUCH_PASSTHROUGH,
          "focus regain claims again");
    Check(claim.Release() == 2 && g_server[":0"].value == 2u, "and gives Steam's newer choice back");
}

void TestEachRootKeepsItsValue() {
    const SosGamescopeTouch::XApi api = FakeApi();
    const auto candidates = GamingMode(3u, 2u, true);
    SosGamescopeTouch::Claim claim;
    claim.Acquire(&api, candidates, 0);
    Check(claim.Release() == 2 && g_server[":1"].value == 3u && g_server[":0"].value == 2u,
          "two roots that started differently are restored independently");

    // A stale Passthrough left on the game's root by an earlier session is rewritten, but never re-imposed on release.
    GamingMode(SOS_GAMESCOPE_TOUCH_PASSTHROUGH, SOS_GAMESCOPE_TOUCH_LEFT, true);
    g_effective = SOS_GAMESCOPE_TOUCH_LEFT;
    claim.Acquire(&api, candidates, 0);
    Check(g_effective == SOS_GAMESCOPE_TOUCH_PASSTHROUGH && g_server[":1"].writes == 1,
          "a root already reading Passthrough is still written");
    Check(claim.Release() == 1 && g_server[":1"].value == SOS_GAMESCOPE_TOUCH_PASSTHROUGH &&
              g_server[":0"].value == SOS_GAMESCOPE_TOUCH_LEFT,
          "only the root that held another value is given it back");

    // Steam changed its root while the game held it: the newer value is adopted, and a value written after the last
    // claim is left alone on release.
    GamingMode();
    claim.Acquire(&api, candidates, 0);
    Steam(":0", 5u);
    claim.Maintain(1000);
    Check(claim.Release() == 2 && g_server[":0"].value == 5u, "Steam's change while the game ran is what it gets back");
    GamingMode();
    claim.Acquire(&api, candidates, 0);
    Steam(":0", 6u);
    Check(claim.Release() == 1 && g_server[":0"].value == 6u, "a value Steam wrote after the claim is never overwritten");
}

void TestUnavailable() {
    SosGamescopeTouch::Claim claim;
    const auto candidates = GamingMode();
    Check(claim.Acquire(nullptr, candidates, 0) == SosGamescopeTouch::Result::Unavailable &&
              claim.Failure() == SOS_TOUCH_CLAIM_NO_X11 && g_effective == SOS_GAMESCOPE_TOUCH_LEFT,
          "without libX11 nothing is claimed and Gamescope keeps its handling");
    SosGamescopeTouch::XApi partial = FakeApi();
    partial.flush = nullptr;
    Check(claim.Acquire(&partial, candidates, 0) == SosGamescopeTouch::Result::Unavailable, "an incomplete libX11 is none");
    const SosGamescopeTouch::XApi api = FakeApi();
    g_server[":1"].answers = g_server[":0"].answers = false;
    Check(claim.Acquire(&api, candidates, 0) == SosGamescopeTouch::Result::Unavailable &&
              claim.Failure() == SOS_TOUCH_CLAIM_NO_DISPLAY && !claim.Maintain(5000) && claim.Release() == 0,
          "no answering root: unavailable, nothing to maintain or restore");
    GamingMode();
    g_server[":1"].answers = false;
    Check(claim.Acquire(&api, candidates, 0) == SosGamescopeTouch::Result::Applied && claim.Roots() == 1 &&
              claim.CompositorRoot() && g_effective == SOS_GAMESCOPE_TOUCH_PASSTHROUGH,
          "Steam's root alone is enough");
    Check(claim.Release() == 1 && g_effective == SOS_GAMESCOPE_TOUCH_LEFT, "and is restored");
}

// The window thread keeps maintaining while a guest thread ends the process: the exit path's release is serialised
// with it and leaves every root restored.
void TestExitPathRelease() {
    const SosGamescopeTouch::XApi api = FakeApi();
    const auto candidates = GamingMode();
    SosGamescopeTouch::Claim claim;
    claim.Acquire(&api, candidates, 0);
    std::atomic<bool> stop{false};
    std::thread window([&] {
        for (std::uint64_t now = 0; !stop.load(); now += 1000) {
            claim.Maintain(now);
        }
    });
    std::thread guest([&] { claim.Release(); });
    guest.join();
    stop = true;
    window.join();
    Check(!claim.Claimed() && claim.Roots() == 0 && g_server[":1"].value == SOS_GAMESCOPE_TOUCH_LEFT &&
              g_server[":0"].value == SOS_GAMESCOPE_TOUCH_LEFT && g_effective == SOS_GAMESCOPE_TOUCH_LEFT,
          "an exit on another thread restores every root and the window thread writes nothing after it");
    Check(!g_server[":0"].open && !g_server[":1"].open, "and every connection the claim opened is closed");

    // Many rounds of a slow X server: the exit-path release never runs inside the window thread's rewrite.
    g_slow = true;
    g_overlap = false;
    for (int round = 0; round < 20 && !g_overlap.load(); ++round) {
        GamingMode();
        SosGamescopeTouch::Claim racing;
        racing.Acquire(&api, candidates, 0);
        std::atomic<bool> go{false};
        std::thread rewrite([&] {
            while (!go.load()) {
            }
            for (std::uint64_t now = 1000; now < 40000; now += 1000) {
                racing.Maintain(now);
            }
        });
        std::thread exit_path([&] {
            while (!go.load()) {
            }
            Spin(std::chrono::microseconds(100 + 37 * round));
            racing.Release();
        });
        go = true;
        rewrite.join();
        exit_path.join();
        racing.Release();
    }
    g_slow = false;
    Check(!g_overlap.load(), "the release and the periodic rewrite never touch the X connections at the same time");
    const std::string claim_source = Source("gpu/shim/sos_gamescope_touch.h");
    for (const char* method : {"Result Acquire(", "bool Maintain(", "int Release() {"}) {
        Check(InOrder(Body(claim_source, method), {"Lock lock{busy_};"}), std::string("Claim::") + method + " takes the claim lock");
    }
}

// The window's switch (SOS_TOUCH_GAMESCOPE, launcher, desktop): when the route is not wanted the claim is never made.
void TestKillSwitchNeverWrites() {
    const SosGamescopeTouch::XApi api = FakeApi();
    for (const char* opt_out : {static_cast<const char*>("0"), static_cast<const char*>(nullptr)}) {
        const auto candidates = GamingMode();
        SosTouchGamescopeEnv env = SteamDeck();
        env.opt_out = opt_out;
        SosGamescopeTouch::Claim claim;
        if (sos_touch_gamescope_wanted(&env)) { // ClaimGamescopeTouch in window.cpp
            claim.Acquire(&api, candidates, 0);
        }
        const bool claimed = g_server[":1"].writes + g_server[":0"].writes > 0;
        Check(opt_out ? !claimed && g_effective == SOS_GAMESCOPE_TOUCH_LEFT : claimed,
              opt_out ? "with SOS_TOUCH_GAMESCOPE=0 no root is ever written" : "without it the roots are written");
        claim.Release();
    }
    const std::string window = Source("gpu/shim/window.cpp");
    const std::string wanted = Body(window, "bool GamescopeTouchWanted() {");
    Check(Contains(wanted, "env.game_window = 1;") && Contains(wanted, "env.opt_out = std::getenv(\"SOS_TOUCH_GAMESCOPE\");") &&
              Contains(wanted, "return sos_touch_gamescope_wanted(&env) != 0;") &&
              Contains(window, "g_touch_routed.store(GamescopeTouchWanted(), std::memory_order_relaxed);"),
          "the window decides the route once from the policy, with the SOS_TOUCH_GAMESCOPE kill switch");
    Check(InOrder(Body(window, "void ClaimGamescopeTouch() {"),
                  {"if (!g_touch_routed.load(std::memory_order_relaxed)) {", "return;", "g_gamescope_touch.Acquire("}),
          "the claim is never made off the route");
}

void TestLauncherNeverClaims() {
    std::vector<std::string> claiming;
    int files = 0;
    const std::filesystem::path launcher = std::filesystem::path(SOS_STRING(SOS_SOURCE_DIR)) / "launcher";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(launcher, error)) {
        const std::string name = entry.path().filename().string();
        const std::string text = Source("launcher/" + name);
        ++files;
        for (const char* needle : {"STEAM_TOUCH_CLICK_MODE", "sos_gamescope_touch", "sos_touch_gamescope", "SosGamescopeTouch",
                                   "dcpgpu_touch_release"}) {
            if (Contains(text, needle)) {
                claiming.push_back(name);
            }
        }
    }
    Check(files > 10, "the launcher sources were read (" + std::to_string(files) + " files)");
    Check(claiming.empty(), "no launcher source claims or releases the Gamescope touch mode" +
                                (claiming.empty() ? std::string() : " (" + claiming.front() + ")"));
    const std::string window = Source("gpu/shim/window.cpp");
    Check(InOrder(window, {"WindowSDL::WindowSDL(", "g_touch_routed.store(GamescopeTouchWanted()", "ClaimGamescopeTouch();"}),
          "the claim belongs to the game window's constructor");
}

// ---------------------------------------------------------------- touchscreen -> touchpad

constexpr std::uint64_t kMs = 1000000;

SosTouchSample Read(SosTouchscreen& s) {
    SosTouchSample sample;
    sos_touchscreen_sample(&s, &sample);
    return sample;
}

void TestCoordinates() {
    Check(SOS_TOUCH_MAX_X == 1919 && SOS_TOUCH_MAX_Y == 942 && SOS_TOUCHPAD_BUTTON == 0x100000u,
          "the surface is scePadGetControllerInformation's 1920 x 943 and the click is the PS4 touchpad bit");
    const SosTouchArea whole = sos_touch_area_centred(1920, 1080, 1920, 1080);
    Check(whole.x == 0.0f && whole.y == 0.0f && whole.width == 1.0f && whole.height == 1.0f, "a full-window picture");
    const SosTouchArea unknown = sos_touch_area_centred(0, 0, 0, 0);
    Check(unknown.width == 1.0f && unknown.height == 1.0f, "before the first frame the whole window is the picture");
    // A 16:9 picture centred on a 1280x800 Steam Deck screen: bars of 40 px above and below.
    const SosTouchArea deck = sos_touch_area_centred(1280, 720, 1280, 800);
    std::uint16_t x = 0, y = 0;
    Check(sos_touch_point(deck, 0.5f, 0.5f, &x, &y) && x == 960 && y == 471, "the screen centre is the touchpad centre");
    Check(sos_touch_point(deck, 0.0f, 40.0f / 800.0f, &x, &y) && x == 0 && y == 0, "the picture's top left is 0,0");
    Check(sos_touch_point(deck, 1.0f, 760.0f / 800.0f, &x, &y) && x == 1919 && y == 942, "its bottom right is 1919,942");
    Check(!sos_touch_point(deck, 0.5f, 10.0f / 800.0f, &x, &y) && y == 0, "a letterbox bar is off the picture (clamped)");
    // A 21:9 ultrawide window with a 16:9 picture: side bars.
    const SosTouchArea wide = sos_touch_area_centred(1920, 1080, 2560, 1080);
    Check(sos_touch_point(wide, 320.0f / 2560.0f, 0.5f, &x, &y) && x == 0 && !sos_touch_point(wide, 0.05f, 0.5f, &x, &y),
          "side bars of a pillarboxed picture are outside the surface");

    SosTouchscreen s{};
    sos_touchscreen_down(&s, 1, deck, 0.5f, 10.0f / 800.0f, 0);
    Check(Read(s).count == 0, "a finger landing on a bar starts no contact");
    sos_touchscreen_down(&s, 2, deck, 0.25f, 0.5f, 0);
    SosTouchSample sample = Read(s);
    Check(sample.count == 1 && sample.x[0] == 480 && sample.y[0] == 471, "a finger maps where it lands on the picture");
    sos_touchscreen_motion(&s, 2, deck, 0.75f, 0.25f);
    sample = Read(s);
    Check(sample.count == 1 && sample.x[0] == 1439 && sample.y[0] == 209, "movement follows the finger");
    sos_touchscreen_motion(&s, 2, deck, 1.5f, -0.5f);
    sample = Read(s);
    Check(sample.x[0] == 1919 && sample.y[0] == 0, "a finger dragged off the picture stays at its edge");
}

void TestTwoFingers() {
    const SosTouchArea area = sos_touch_area_centred(1920, 1080, 1920, 1080);
    SosTouchscreen s{};
    sos_touchscreen_down(&s, 11, area, 0.25f, 0.5f, 0);
    sos_touchscreen_down(&s, 22, area, 0.75f, 0.5f, 1 * kMs);
    SosTouchSample sample = Read(s);
    Check(sample.count == 2 && sample.id[0] != sample.id[1], "two fingers are two contacts with their own ids");
    Check(sample.x[0] == 480 && sample.x[1] == 1439, "each at its own point");
    const std::uint8_t first = sample.id[0], second = sample.id[1];
    sos_touchscreen_down(&s, 33, area, 0.5f, 0.5f, 2 * kMs);
    sample = Read(s);
    Check(sample.count == 2 && sample.id[0] == first && sample.id[1] == second, "a third finger is ignored, as on a DS4");
    sos_touchscreen_motion(&s, 22, area, 0.5f, 0.25f);
    sample = Read(s);
    Check(sample.id[1] == second && sample.x[1] == 960 && sample.x[0] == 480, "each finger moves only its own contact");

    // Release of the first finger: shown until a read saw it lifted, then gone; the second keeps its id.
    sos_touchscreen_up(&s, 11, area, 0.25f, 0.5f, 400 * kMs);
    sample = Read(s);
    Check(sample.count == 1 && sample.id[0] == second && sample.buttons == 0,
          "a released finger leaves at the next read; a two-finger gesture never clicks");
    sos_touchscreen_down(&s, 44, area, 0.1f, 0.1f, 500 * kMs);
    sample = Read(s);
    Check(sample.count == 2 && sample.id[0] != first && sample.id[0] != second, "a new finger gets a new id");
    sos_touchscreen_up(&s, 22, area, 0.5f, 0.25f, 600 * kMs);
    sos_touchscreen_up(&s, 44, area, 0.1f, 0.1f, 600 * kMs);
    Read(s);
    Check(Read(s).count == 0, "all released: no contact remains");

    std::uint8_t last = 0;
    bool increasing = true;
    for (int i = 0; i < 300; ++i) {
        sos_touchscreen_down(&s, 100 + std::uint64_t(i), area, 0.5f, 0.5f, 0);
        const SosTouchSample one = Read(s);
        increasing = increasing && one.count == 1 && one.id[0] < 128 && (i == 0 || one.id[0] == ((last + 1) & 0x7f));
        last = one.id[0];
        sos_touchscreen_cancel(&s, 100 + std::uint64_t(i));
    }
    Check(increasing, "contact ids count up and wrap within 0..127 like the DS4's");
}

void TestTapAndRelease() {
    const SosTouchArea area = sos_touch_area_centred(1920, 1080, 1920, 1080);
    SosTouchscreen s{};
    // A tap shorter than a frame: shown once where it landed, then clicked for three reads, then gone.
    sos_touchscreen_down(&s, 1, area, 0.5f, 0.5f, 0);
    sos_touchscreen_up(&s, 1, area, 0.5f, 0.5f, 50 * kMs);
    std::vector<SosTouchSample> reads;
    for (int i = 0; i < 6; ++i) {
        reads.push_back(Read(s));
    }
    Check(reads[0].count == 1 && reads[0].buttons == 0, "a tap no read saw is shown once first");
    Check(reads[1].buttons == SOS_TOUCHPAD_BUTTON && reads[2].buttons == SOS_TOUCHPAD_BUTTON &&
              reads[3].buttons == SOS_TOUCHPAD_BUTTON && reads[1].count == 1,
          "then the touchpad click is held for three reads at the same point");
    Check(reads[4].count == 0 && reads[4].buttons == 0 && reads[5].count == 0, "then the contact and the click end");

    // A drag, a long press and a cancel never click.
    sos_touchscreen_down(&s, 2, area, 0.2f, 0.5f, 0);
    Read(s);
    sos_touchscreen_motion(&s, 2, area, 0.4f, 0.5f);
    sos_touchscreen_up(&s, 2, area, 0.4f, 0.5f, 100 * kMs);
    int clicks = 0;
    for (int i = 0; i < 5; ++i) clicks += Read(s).buttons ? 1 : 0;
    sos_touchscreen_down(&s, 3, area, 0.5f, 0.5f, 0);
    Read(s);
    sos_touchscreen_up(&s, 3, area, 0.5f, 0.5f, 900 * kMs);
    for (int i = 0; i < 5; ++i) clicks += Read(s).buttons ? 1 : 0;
    sos_touchscreen_down(&s, 4, area, 0.5f, 0.5f, 0);
    sos_touchscreen_cancel(&s, 4);
    for (int i = 0; i < 5; ++i) clicks += Read(s).buttons || Read(s).count ? 1 : 0;
    Check(clicks == 0, "a drag, a long press and a cancelled finger never click");

    // Blocked (overlay, text entry, focus away): contacts and clicks are dropped and new fingers ignored.
    sos_touchscreen_down(&s, 5, area, 0.5f, 0.5f, 0);
    sos_touchscreen_up(&s, 5, area, 0.5f, 0.5f, 10 * kMs);
    sos_touchscreen_block(&s, 1);
    sos_touchscreen_down(&s, 6, area, 0.5f, 0.5f, 0);
    Check(Read(s).count == 0 && Read(s).buttons == 0, "a blocked touchscreen gives the game nothing");
    sos_touchscreen_block(&s, 0);
    sos_touchscreen_down(&s, 7, area, 0.5f, 0.5f, 0);
    Check(Read(s).count == 1, "and works again once unblocked");
    sos_touchscreen_release_all(&s);
    Check(Read(s).count == 0, "focus loss releases every contact");
}

// ---------------------------------------------------------------- SDL routing: no double input, desktop unchanged

SDL_Event Finger(Uint32 type, SDL_FingerID id, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.touchID = 77;
    e.tfinger.fingerID = id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    return e;
}
SDL_Event Mouse(Uint32 type, SDL_MouseID which) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.which = which;
        e.motion.xrel = 5.0f;
    } else if (type == SDL_EVENT_MOUSE_WHEEL) {
        e.wheel.which = which;
    } else {
        e.button.which = which;
        e.button.button = SDL_BUTTON_LEFT;
    }
    return e;
}

// What the game receives from one touch as SDL delivers it: the finger plus SDL's mouse copy of it.
struct GameInput {
    SosTouchscreen screen{};
    int mouse_events = 0;
    int dropped = 0;

    void Deliver(const SDL_Event& event, bool routed) {
        const SosTouchArea area = sos_touch_area_centred(1920, 1080, 1920, 1080);
        switch (SosTouchRoute::Classify(event, routed)) {
        case SosTouchRoute::Use::Finger:
            if (event.type == SDL_EVENT_FINGER_DOWN) {
                sos_touchscreen_down(&screen, event.tfinger.fingerID, area, event.tfinger.x, event.tfinger.y, 0);
            } else if (event.type == SDL_EVENT_FINGER_MOTION) {
                sos_touchscreen_motion(&screen, event.tfinger.fingerID, area, event.tfinger.x, event.tfinger.y);
            } else if (event.type == SDL_EVENT_FINGER_UP) {
                sos_touchscreen_up(&screen, event.tfinger.fingerID, area, event.tfinger.x, event.tfinger.y, 10 * kMs);
            }
            break;
        case SosTouchRoute::Use::DropMouse:
            ++dropped;
            break;
        case SosTouchRoute::Use::Unchanged:
            mouse_events += event.type >= SDL_EVENT_MOUSE_MOTION && event.type <= SDL_EVENT_MOUSE_WHEEL ? 1 : 0;
            break;
        }
    }
};

std::vector<SDL_Event> OneTouch() {
    return {Finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.5f), Mouse(SDL_EVENT_MOUSE_MOTION, SDL_TOUCH_MOUSEID),
            Mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_TOUCH_MOUSEID), Finger(SDL_EVENT_FINGER_MOTION, 1, 0.5f, 0.5f),
            Finger(SDL_EVENT_FINGER_UP, 1, 0.5f, 0.5f), Mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_TOUCH_MOUSEID)};
}

void TestNoDoubleInput() {
    GameInput routed;
    int contacts = 0;
    for (const SDL_Event& e : OneTouch()) {
        routed.Deliver(e, true);
        contacts += Read(routed.screen).count;
    }
    Check(contacts > 0 && routed.mouse_events == 0 && routed.dropped == 3,
          "on the Gamescope route a touch is a touchpad contact only: SDL's mouse copy never reaches the game");
    // A real mouse (or the Steam Deck trackpad, which Steam Input presents as a mouse) is untouched by the route.
    for (const Uint32 type : {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP,
                              SDL_EVENT_MOUSE_WHEEL}) {
        routed.Deliver(Mouse(type, 1), true);
    }
    Check(routed.mouse_events == 4, "a real mouse keeps working while the route is on");
    Check(SosTouchRoute::AcceptedTouch(77, SDL_TOUCH_DEVICE_DIRECT) &&
              !SosTouchRoute::AcceptedTouch(77, SDL_TOUCH_DEVICE_INDIRECT_RELATIVE) &&
              !SosTouchRoute::AcceptedTouch(SDL_MOUSE_TOUCHID, SDL_TOUCH_DEVICE_DIRECT) &&
              !SosTouchRoute::AcceptedTouch(SDL_PEN_TOUCHID, SDL_TOUCH_DEVICE_DIRECT),
          "only a direct touchscreen is a touchpad; trackpads and SDL's mouse or pen touches are not");

    const std::string window = Source("gpu/shim/window.cpp");
    const std::string poll = Body(window, "bool WindowSDL::PollEvents() {");
    Check(Contains(poll, "const SosTouchRoute::Use touch = SosTouchRoute::Classify(event, touch_routed);") &&
              Contains(poll, "switch (touch == SosTouchRoute::Use::DropMouse ? Uint32(SDL_EVENT_FIRST) : event.type) {"),
          "the window classifies every event; a touch-made mouse event never moves the game cursor state");
    Check(InOrder(poll, {"if (DcpOverlay::HandleEvent(event)) {", "if (touch == SosTouchRoute::Use::Finger) {",
                         "HandleTouch(event, ui);", "continue;", "if (touch == SosTouchRoute::Use::DropMouse) {",
                         "continue;", "switch (event.type) {", "dcp_mouse_motion(", "dcp_mouse_button("}),
          "the overlay still gets touch input; the game mouse path never sees a finger or its mouse copy");
    const std::string handle = Body(window, "void WindowSDL::HandleTouch(");
    Check(Contains(handle, "SosTouchRoute::AcceptedTouch(finger.touchID, SDL_GetTouchDeviceType(finger.touchID))") &&
              InOrder(handle, {"case SDL_EVENT_FINGER_DOWN:", "if (ui) {", "sos_touchscreen_down("}) &&
              Contains(handle, "sos_touchscreen_up(") && Contains(handle, "sos_touchscreen_cancel("),
          "HandleTouch takes direct screens only and starts no contact under an overlay");
    Check(Contains(window, "WithTouchscreen([blocked](SosTouchscreen& s) { sos_touchscreen_block(&s, blocked); });") &&
              InOrder(Body(window, "void WindowSDL::ReleaseMouse() {"), {"sos_touchscreen_release_all(&s);"}),
          "the touchscreen is blocked and released with the mouse (overlay, text entry, focus, mode switch)");
}

void TestDesktopUnchanged() {
    GameInput desktop;
    for (const SDL_Event& e : OneTouch()) {
        desktop.Deliver(e, false);
    }
    Check(Read(desktop.screen).count == 0 && desktop.dropped == 0 && desktop.mouse_events == 3,
          "off the route (Windows, desktop Linux, the kill switch) a touch reaches the game exactly as before");
    for (const SDL_Event& e : OneTouch()) {
        Check(SosTouchRoute::Classify(e, false) == SosTouchRoute::Use::Unchanged, "every event is unchanged off the route");
    }
    const std::string window = Source("gpu/shim/window.cpp");
    const std::string sample = Body(window, "extern \"C\" int dcpgpu_touchscreen_sample(");
    Check(InOrder(sample, {"*buttons = 0;", "if (!Frontend::g_touch_routed.load(std::memory_order_relaxed)) {", "return 0;"}),
          "off the route the pad sampler gets no contact after one atomic load");
    const std::string pad = Source("src/runtime_pad.c");
    const std::string host = Body(pad, "static void sample_host(int slot, PadData *d) {");
    Check(InOrder(host, {"if (dcpgpu_overlay_captures_input()) return;", "if (slot==0) {",
                         "const int screen=dcpgpu_touchscreen_sample(tx,ty,tid,&tap);",
                         "d->touches[d->touch_count++]=(PadTouch){.x=tx[i],.y=ty[i],.id=tid[i]};", "d->buttons|=tap;",
                         "if (g) {", "for (int finger=0;finger<fingers && d->touch_count<2;++finger) {"}),
          "player 1's pad takes the screen contacts first, a controller touchpad fills the rest, a tap clicks");
    Check(Contains(pad, "touch_axis(x,1919)") && Contains(pad, "touch_axis(y,942)"),
          "the controller touchpad uses the same 1920 x 943 surface");
    Check(Contains(Source("src/dcpgpu.h"), "int dcpgpu_touchscreen_sample(uint16_t *x, uint16_t *y, uint8_t *id, uint32_t *buttons);"),
          "the pad sampler's interface is declared once");
}

// ---------------------------------------------------------------- focus and exit paths

void TestFocusAndExitWiring() {
    const std::string window = Source("gpu/shim/window.cpp");
    const std::string poll = Body(window, "bool WindowSDL::PollEvents() {");
    Check(InOrder(poll, {"case SDL_EVENT_WINDOW_FOCUS_LOST:", "ReleaseMouse();", "ReleaseGamescopeTouch();", "break;",
                         "case SDL_EVENT_WINDOW_FOCUS_GAINED:", "ClaimGamescopeTouch();"}),
          "focus loss restores the roots, focus gain claims again");
    Check(Contains(poll, "if (g_gamescope_touch.Claimed() && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&\n"
                         "        g_gamescope_touch.Maintain(SDL_GetTicks())) {"),
          "Passthrough is rewritten only while the game window has focus");
    Check(InOrder(poll, {"case SDL_EVENT_WINDOW_CLOSE_REQUESTED:", "is_open = false;", "ReleaseGamescopeTouch();"}) &&
              InOrder(poll, {"std::printf(\"Exit Game: hard exit\\n\");", "ReleaseGamescopeTouch();"}),
          "closing the window and Exit Game restore the roots on the window thread");
    Check(InOrder(Body(window, "extern \"C\" void dcpgpu_touch_release(void) {"), {"Frontend::g_gamescope_touch.Release()"}),
          "the exit-path release gives every root back");
    const std::string gpu = Source("gpu/shim/dcpgpu.cpp");
    Check(InOrder(gpu, {"while (window->PollEvents())", "dcpgpu_touch_release();", "runtime_log_finalize();", "std::_Exit(0);"}),
          "the window thread's exit restores the roots before _Exit");
    struct Exit {
        const char* file;
        const char* function;
        const char* end;
    };
    const Exit exits[] = {
        {"src/runtime.c", "void guest_libc_exit(int status) {", "runtime_exit(status);"},
        {"src/runtime.c", "void guest_quick_exit(int status) {", "runtime_exit(status);"},
        {"src/runtime.c", "void guest_abort(void) {", "runtime_exit(134);"},
        {"src/runtime_kernel.c", "void hard_exit(int status) {", "_exit(status);"},
        {"src/runtime_kernel.c", "void kernel_exit_process(int32_t status) {", "runtime_exit(status);"},
        {"src/runtime_kernel.c", "void hard__Exit(int32_t status) {", "runtime_exit(status);"},
        {"src/runtime_services.c", "void load_exec(const char *path, char *const *argv) {", "_exit(LOAD_EXEC_EXIT);"},
    };
    for (const Exit& exit : exits) {
        const std::string body = Body(Source(exit.file), exit.function);
        Check(InOrder(body, {"dcpgpu_touch_release();", "runtime_log_finalize();", exit.end}),
              std::string("the guest exit ") + exit.function + " restores the roots before the process ends");
    }
    for (const char* path : {"gpu/shim/sos_gamescope_touch.h", "src/sos_touch_gamescope.h", "src/sos_touchscreen.h",
                             "gpu/shim/sos_touch_route.h"}) {
        const std::string text = Source(path);
        Check(!text.empty() && !Contains(text, "atexit") && !Contains(text, "SDL_UnloadObject") && !Contains(text, "thread_local") &&
                  !Contains(text, "~Claim"),
              std::string(path) + ": no atexit, unload, thread_local or destructor teardown (the shutdown gate)");
    }
}

// ---------------------------------------------------------------- telemetry

void TestTelemetry() {
    SosTouchRouteLog log{};
    SosTouchRouteInfo route{"x11", 1, 1, 1, 2, 1, 0};
    char line[SOS_TOUCH_ROUTE_LINE_SIZE];
    Check(sos_touch_route_note(&log, SOS_TOUCH_ROUTE_READY, &route, line, sizeof(line)) &&
              !std::strcmp(line, "Touchscreen telemetry: event=touch_route_ready video=x11 gamescope=yes routed=yes "
                                 "direct_devices=1 count=1"),
          "touch_route_ready says the video driver, Gamescope, the route and the direct screens");
    Check(!sos_touch_route_note(&log, SOS_TOUCH_ROUTE_READY, &route, line, sizeof(line)), "and is logged once");
    Check(!sos_touch_route_note(&log, SOS_TOUCH_ROUTE_REASSERT, &route, line, sizeof(line)), "no reassert before a claim");
    Check(sos_touch_route_note(&log, SOS_TOUCH_ROUTE_CLAIMED, &route, line, sizeof(line)) &&
              !std::strcmp(line, "Touchscreen telemetry: event=gamescope_touch_claim roots=2 compositor=yes count=1"),
          "the claim line names the roots");
    int reasserts = 0;
    for (int i = 0; i < 3600; ++i) {
        reasserts += sos_touch_route_note(&log, SOS_TOUCH_ROUTE_REASSERT, &route, line, sizeof(line));
    }
    Check(reasserts == 4, "an hour of once-a-second rewrites logs 4 lines (1, 10, 100, 1000)");
    route.failure = SOS_TOUCH_CLAIM_NO_DISPLAY;
    Check(sos_touch_route_note(&log, SOS_TOUCH_ROUTE_CLAIM_FAILED, &route, line, sizeof(line)) &&
              Contains(line, "reason=display"),
          "a failed claim says why");
    route.video = "/home/user/secret";
    log.logged[SOS_TOUCH_ROUTE_READY] = 0;
    sos_touch_route_note(&log, SOS_TOUCH_ROUTE_READY, &route, line, sizeof(line));
    Check(Contains(line, "video=other") && !Contains(line, "secret"), "only known driver names reach the log");

    std::uint64_t counts[SOS_TOUCH_EVENT_COUNT] = {};
    char event[96];
    int detected = 0, used = 0;
    for (int i = 0; i < 1000; ++i) {
        detected += sos_touch_event_note(counts, SOS_TOUCH_EVENT_DETECTED, event, sizeof(event));
        used += sos_touch_event_note(counts, SOS_TOUCH_EVENT_TOUCHPAD_USED, event, sizeof(event));
    }
    Check(detected == 1 && used == 4 && Contains(event, "touchscreen_touchpad_used count=1000"),
          "touchscreen_detected once, touchscreen_touchpad_used bounded");
}

} // namespace

int main() {
    TestPolicy();
    TestClaimOnFocusAndRestore();
    TestEachRootKeepsItsValue();
    TestUnavailable();
    TestExitPathRelease();
    TestKillSwitchNeverWrites();
    TestLauncherNeverClaims();
    TestCoordinates();
    TestTwoFingers();
    TestTapAndRelease();
    TestNoDoubleInput();
    TestDesktopUnchanged();
    TestFocusAndExitWiring();
    TestTelemetry();
    std::printf("touchscreen steamos: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
