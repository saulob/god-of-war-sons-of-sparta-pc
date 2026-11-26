// Frame pacing on the production limiter arithmetic and the Frame rate policy, in simulated time (no sleeping): absolute
// slots without drift, a stall reanchors instead of bursting, every rate against every display refresh, the original
// 60 and the halved 30, and the guest-driven (not repeat-driven) high rate. No window, GPU, launcher or game.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "common/pacing_balance.h"
#include "dcp_frame_pacing.h"
#include "dcp_present.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace Pacing = DcpFramePacing;
using std::chrono::nanoseconds;
using Common::PacingBalance;

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

std::string Source(const char* path) {
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

bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
    const size_t at = text.find(first);
    return at != std::string::npos && text.find(second, at) != std::string::npos;
}

nanoseconds PeriodFor(unsigned hz) {
    return nanoseconds(1000000000 / hz);
}

// The present thread loop: sleep the balance (waking late), do the frame's work, report it.
struct Sim {
    explicit Sim(unsigned hz) : period(PeriodFor(hz)), balance(PeriodFor(hz)) {}

    void Frame(nanoseconds work, nanoseconds late = nanoseconds(0)) {
        const nanoseconds sleep = balance.SleepNeeded();
        const nanoseconds elapsed = sleep > nanoseconds(0) ? sleep + late : nanoseconds(0);
        now += elapsed;
        balance.Slept(elapsed);
        starts.push_back(now);
        now += work;
        balance.FrameFinished(work);
    }

    int Bursts(size_t from) const {
        int bursts = 0;
        for (size_t i = from + 1; i < starts.size(); ++i) {
            bursts += (starts[i] - starts[i - 1]) < period / 2 ? 1 : 0;
        }
        return bursts;
    }

    nanoseconds period;
    PacingBalance balance;
    nanoseconds now{};
    std::vector<nanoseconds> starts;
};

std::uint32_t g_seed = 12345;
nanoseconds Jitter(nanoseconds max) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return nanoseconds(static_cast<std::int64_t>(g_seed >> 8) % (max.count() + 1));
}

void TestSteadyCadence() {
    for (const unsigned hz : {60u, 90u, 120u, 144u, 165u, 240u, 480u}) {
        Sim sim(hz);
        for (int i = 0; i < 2000; ++i) {
            sim.Frame(sim.period / 4);
        }
        bool exact = true;
        for (size_t i = 1; i < sim.starts.size(); ++i) {
            exact = exact && sim.starts[i] - sim.starts[i - 1] == sim.period;
        }
        Check(exact, "steady cadence at " + std::to_string(hz) + " Hz: every interval is the period, none alternates");
    }
}

void TestNoDrift() {
    for (const unsigned hz : {60u, 90u, 120u, 144u, 165u, 240u, 480u}) {
        for (const int seconds : {10, 60, 600}) {
            Sim late(hz);
            Sim jittery(hz);
            const int frames = seconds * static_cast<int>(hz);
            for (int i = 0; i < frames; ++i) {
                late.Frame(late.period / 5, std::chrono::microseconds(100));
                jittery.Frame(jittery.period / 5 + Jitter(nanoseconds(300000)), Jitter(std::chrono::milliseconds(1)));
            }
            const auto ideal = late.period * (frames - 2);
            const auto late_error = std::llabs((late.starts.back() - late.starts[1] - ideal).count());
            const auto jitter_error = std::llabs((jittery.starts.back() - jittery.starts[1] - ideal).count());
            const std::string name = std::to_string(hz) + " Hz over " + std::to_string(seconds) + " s";
            Check(late_error < 1000, "a constant 100 us late wake accumulates no drift at " + name);
            Check(jitter_error <= 1000000, "wake and work jitter keep the cadence within 1 ms of ideal at " + name);
            const double rounding_ms = (1000.0 / hz - double(late.period.count()) / 1e6) * frames;
            Check(rounding_ms < 1.0, "period rounding stays under 1 ms over " + name);
        }
    }
    Sim late_1ms(90);
    for (int i = 0; i < 5400; ++i) {
        late_1ms.Frame(late_1ms.period / 4, std::chrono::milliseconds(1));
    }
    Check(late_1ms.Bursts(0) == 0, "a constant 1 ms late wake never bursts");
    Check(late_1ms.starts.back() - late_1ms.starts[1] == late_1ms.period * 5398, "and never drifts");
}

void TestStallRecovery() {
    struct Case {
        const char* name;
        int intervals;
        int max_burst;
    };
    const Case cases[] = {{"one full frame late", 2, 1},      {"two frames late", 3, 2},
                          {"several frames late", 8, 0},      {"500 ms stall", 0, 0},
                          {"multi-second stall", 0, 0}};
    for (const unsigned hz : {30u, 60u, 90u, 144u, 240u}) {
        for (int c = 0; c < 5; ++c) {
            Sim sim(hz);
            for (int i = 0; i < 100; ++i) {
                sim.Frame(sim.period / 4);
            }
            nanoseconds stall = sim.period * cases[c].intervals;
            if (c == 3) {
                stall = std::chrono::milliseconds(500);
            } else if (c == 4) {
                stall = std::chrono::seconds(7);
            }
            sim.Frame(stall);
            const size_t after = sim.starts.size();
            for (int i = 0; i < 3 * static_cast<int>(hz); ++i) {
                sim.Frame(sim.period / 4);
            }
            const std::string name = std::string(cases[c].name) + " at " + std::to_string(hz) + " Hz";
            Check(sim.Bursts(after - 1) <= cases[c].max_burst, name + ": no catch-up burst");
            bool steady = true;
            for (size_t i = after + 4; i < sim.starts.size(); ++i) {
                steady = steady && sim.starts[i] - sim.starts[i - 1] == sim.period;
            }
            Check(steady, name + ": resumes at the exact period from a sane deadline");
            const auto window = sim.starts.back() - sim.starts[after - 1];
            Check(window >= sim.period * (3 * static_cast<int>(hz) - 1), name + ": the simulation is not accelerated");
        }
    }
}

void TestPolicyMatrix() {
    const double displays[] = {60.0, 90.0, 119.88, 143.856, 165.0, 240.0, 0.0};
    for (const int setting : Pacing::kFrameRateModes) {
        for (const double display : displays) {
            for (const bool vsync : {false, true}) {
                const unsigned hz = Pacing::GuestVblankHz(setting, vsync, display);
                const std::string name = Pacing::ModeName(setting) + " on " + std::to_string(display) +
                                         " Hz VSync " + (vsync ? "on" : "off");
                Check(hz >= 60 && hz <= 480, name + ": the guest vblank stays in 60..480");
                if (setting == 30 || setting == 60) {
                    Check(hz == 60, name + ": the original 60 Hz guest vblank");
                    Check(Pacing::ShownFramesPerSecond(setting, vsync, display) == static_cast<unsigned>(setting),
                          name + ": shows its rate");
                    continue;
                }
                const bool bounded = vsync && display > 0.0;
                const unsigned cap = bounded ? static_cast<unsigned>(Pacing::NominalHz(display)) : 480u;
                const unsigned want = setting == 0 ? cap : std::min<unsigned>(setting, cap);
                Check(hz == std::max(60u, want), name + ": the vblank is the setting bounded by a known VSync display");
                Check(Pacing::ShownFramesPerSecond(setting, vsync, display) == hz, name + ": new frames follow the vblank");
            }
        }
    }
    Check(Pacing::GuestVblankHz(90) == 90, "90 FPS is a 90 Hz guest vblank");
    Check(Pacing::GuestVblankHz(240, true, 143.856) == 144, "240 on a 144 Hz display with VSync is at most 144");
    Check(Pacing::GuestVblankHz(0, true, 165.0) == 165, "Unlimited on a 165 Hz display with VSync is at most 165");
    Check(Pacing::GuestVblankHz(120, true, 60.0) == 60, "120 on a 60 Hz display with VSync is 60");
    Check(Pacing::GuestVblankHz(90, true, 143.856) == 90, "90 on a 144 Hz display stays 90");
    Check(Pacing::GuestVblankHz(0) == Pacing::kUnlimitedVblankHz, "Unlimited with no known display keeps the 480 cap");
    Check(Pacing::GuestVblankHz(240, true, 0.0) == 240, "an unknown display does not bound a rate");
    Check(Pacing::NominalHz(119.88) == 120 && Pacing::NominalHz(59.94) == 60, "refresh rounds to the nominal rate");
}

void TestOriginalRates() {
    int shown30 = 0;
    int shown60 = 0;
    int shown30_slow = 0;
    for (std::uint64_t flip = 0; flip < 600; ++flip) {
        shown30 += Pacing::PresentsFlip(30, flip, 0) ? 1 : 0;
        shown60 += Pacing::PresentsFlip(60, flip, 0) ? 1 : 0;
        shown30_slow += Pacing::PresentsFlip(30, flip, 1) ? 1 : 0;
    }
    Check(shown30 == 300, "30 FPS shows every second 60 Hz flip");
    Check(shown60 == 600, "60 FPS shows every flip");
    Check(shown30_slow == 600, "a game flipping every second vblank is not halved twice at 30");
    for (const int rate : {90, 120, 144, 165, 240, 0}) {
        int shown = 0;
        for (std::uint64_t flip = 0; flip < 600; ++flip) {
            shown += Pacing::PresentsFlip(rate, flip, 0) ? 1 : 0;
        }
        Check(shown == 600, "every flip is shown at " + Pacing::ModeName(rate));
    }
    Check(Pacing::kGameHz == 60, "the game's own step stays 1/60 s");
    for (const int rate : Pacing::kFrameRateModes) {
        Check(Pacing::kGameHz == 60 && Pacing::RenderFps(rate) == (rate == 0 ? 0 : rate),
              "the setting never changes the game step: " + Pacing::ModeName(rate));
    }
    Check(Pacing::ParseFrameRate("45") == 60 && Pacing::ParseFrameRate("") == 60, "an unknown setting is the original 60");
}

void TestArchitecture() {
    const std::string driver = Source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    Check(InOrder(driver, "const auto request = receive_request();", "if (!request) {") &&
              InOrder(driver, "} else {\n                Flip(request);", "FRAME_END;"),
          "a real flip is taken before any redraw and is presented by Flip");
    Check(Contains(driver, "if (!request) {\n                // The save list") &&
              Contains(driver, "} else if (timer.GetTotalWait().count() < 0) { // Dont draw too fast"),
          "a redraw of the last frame happens only when no real flip is queued, and never while behind");
    Check(Contains(driver, "SetVblankFrequency(DcpFramePacing::GuestVblankHz(") &&
              Contains(driver, "vblank_period(1000000000 /\n                                                 EmulatorSettings.GetVblankFrequency())"),
          "the guest vblank and the present thread period come from the setting");
    Check(Contains(driver, "Common::AccurateTimer timer{vblank_period};"), "the present thread paces with the shared limiter");
    const std::string menu = Source("gpu/shim/dcp_options_menu.h");
    Check(Contains(menu, "\"frame_rate\", \"Frame rate\",") &&
              InOrder(menu, "\"frame_rate\", \"Frame rate\",", "Apply::Restart, kFrameRates"),
          "the Frame rate setting is read once at startup, so no limiter state outlives a change");
    Check(!Contains(menu, "show the last frame again"), "the setting description does not promise repeated frames");
    const std::string thread = Source("gpu/shadps4/common/thread.cpp");
    const size_t timer = thread.find("AccurateTimer::AccurateTimer");
    const std::string timer_code = thread.substr(timer, thread.find("std::string GetCurrentThreadName") - timer);
    Check(Contains(timer_code, "std::chrono::steady_clock::now()") && !Contains(timer_code, "system_clock") &&
              !Contains(timer_code, "high_resolution_clock"),
          "frame deadlines use the monotonic clock");
    Check(Contains(timer_code, "balance.FrameFinished(") && Contains(timer_code, "balance.Slept("),
          "the timer runs on the shared balance arithmetic");
    Check(Contains(thread, "thread_local ThreadTimer thread_timer;") && Contains(thread, "CREATE_WAITABLE_TIMER_HIGH_RESOLUTION") &&
              Contains(thread, "handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);"),
          "short sleeps use one reusable high-resolution timer per thread with a plain-timer fallback");
    const size_t sleep = thread.find("bool AccurateSleep(");
    Check(sleep != std::string::npos && !Contains(thread.substr(sleep, 900), "CreateWaitableTimer("),
          "no timer handle is created per sleep");
}

// Gamescope shows one image per refresh whatever the present mode: its display paces presents like VSync does, so a
// rate above the display is bounded there too. Windows and desktop Linux are untouched.
void TestGamescopePacesPresents() {
    Check(!Pacing::DisplayPacesPresents(false, false) && Pacing::DisplayPacesPresents(true, false) &&
              Pacing::DisplayPacesPresents(false, true) && Pacing::DisplayPacesPresents(true, true),
          "the display paces presents with VSync or under a pacing compositor");
    const bool gamescope_no_vsync = Pacing::DisplayPacesPresents(false, true);
    Check(Pacing::GuestVblankHz(90, gamescope_no_vsync, 60.0) == 60,
          "90 FPS with VSync off on a 60 Hz Gamescope display is a 60 Hz vblank (frames above it are never shown)");
    Check(Pacing::GuestVblankHz(0, gamescope_no_vsync, 90.0) == 90, "Unlimited on a 90 Hz Gamescope display is 90");
    Check(Pacing::ShownFramesPerSecond(120, gamescope_no_vsync, 60.0) == 60, "and the log says what can be shown");
    Check(Pacing::GuestVblankHz(90, gamescope_no_vsync, 0.0) == 90, "an unknown refresh still bounds nothing");
    const bool desktop_no_vsync = Pacing::DisplayPacesPresents(false, false);
    Check(Pacing::GuestVblankHz(90, desktop_no_vsync, 60.0) == 90 && Pacing::GuestVblankHz(0, desktop_no_vsync, 60.0) == 480,
          "VSync off outside Gamescope keeps the requested rate");
    for (const int setting : Pacing::kFrameRateModes) {
        for (const double display : {60.0, 144.0, 0.0}) {
            for (const bool vsync : {false, true}) {
                Check(Pacing::GuestVblankHz(setting, Pacing::DisplayPacesPresents(vsync, false), display) ==
                          Pacing::GuestVblankHz(setting, vsync, display),
                      "outside Gamescope the vblank is the VSync rule unchanged: " + Pacing::ModeName(setting));
            }
        }
    }
    const std::string driver = Source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    Check(InOrder(driver, "DcpFramePacing::DisplayPacesPresents(settings.vsync.load(), DcpPresent::GamescopeSession());",
                  "EmulatorSettings.SetVblankFrequency(DcpFramePacing::GuestVblankHz(frame_rate, display_paced, display_hz));"),
          "the guest vblank follows the Gamescope rule");
    Check(Contains(driver, "DcpFramePacing::DisplayPacesPresents(DcpSettings::Get().vsync, gamescope)") &&
              Contains(driver, "gamescope ? \" (Gamescope paces presents)\" : \"\""),
          "the VideoOut line reports the same rate and says when Gamescope paces presents");
}

// The swap thread queue: a redraw of the last frame is work, never backlog, and never runs ahead of a real flip.
void TestSwapQueueBacklog() {
    DcpPresent::SwapQueue<int> queue;
    Check(queue.Empty() && !queue.FlipQueued() && queue.Size() == 0, "an empty queue has no backlog");

    Check(queue.PushRedraw(1), "a redraw queues on an idle swap thread");
    Check(!queue.Empty() && !queue.FlipQueued(), "a queued redraw is work for the swap thread but no backlog for flips");
    Check(!queue.PushRedraw(2) && queue.Size() == 1, "a second redraw is pointless while one is queued");

    queue.PushFlip(3);
    Check(queue.FlipQueued() && queue.Size() == 1, "a real flip replaces a queued redraw");
    Check(queue.Take() == 3 && !queue.FlipQueued() && queue.Empty(), "the flip runs first: the old frame is never shown after it");

    Check(!queue.PushRedraw(4), "no redraw while a flip is presenting");
    queue.PushFlip(5);
    queue.PushFlip(6);
    Check(queue.FlipQueued() && queue.Size() == 2, "flips queue behind a running flip and are the backlog");
    Check(!queue.PushRedraw(7), "no redraw behind queued flips");
    queue.Finished();
    Check(queue.Take() == 5 && queue.FlipQueued(), "flips run in order");
    queue.Finished();
    Check(queue.Take() == 6 && !queue.FlipQueued(), "the last queued flip ends the backlog");
    queue.Finished();
    Check(queue.PushRedraw(8) && !queue.FlipQueued(), "an idle swap thread takes a redraw again");
    Check(queue.Take() == 8 && !queue.FlipQueued(), "taking a redraw leaves no flip count behind");
    queue.Finished();
    queue.PushFlip(9);
    Check(queue.FlipQueued() && queue.Take() == 9 && !queue.FlipQueued(), "the flip count stays exact");
}

// The Gamescope flip gate holds the guest's flip only while a real flip is still queued. With a slow swap thread and
// the overlay asking for redraws on every free tick, counting the redraw as backlog refused flips; it must not.
void TestFlipGateCountsOnlyFlips() {
    const auto run = [](bool count_redraws) {
        DcpPresent::SwapQueue<int> queue;
        bool busy = false;
        int flips_taken = 0;
        int refused = 0;
        for (int tick = 0; tick < 400; ++tick) {
            // The vblank thread: the guest flips every second tick; the redraw queued between two ticks is still
            // waiting for the swap thread to wake when the gate looks.
            const bool guest_flip = tick % 2 == 0;
            const bool waits = count_redraws ? !queue.Empty() : queue.FlipQueued();
            if (guest_flip && !waits) {
                queue.PushFlip(tick);
                ++flips_taken;
            } else if (guest_flip) {
                ++refused;
            }
            // The swap thread wakes, takes the next item and finishes it within the tick.
            if (!busy && !queue.Empty()) {
                queue.Take();
                busy = true;
            }
            if (busy) {
                queue.Finished();
                busy = false;
            }
            // The overlay is open: a redraw of the last frame between two ticks.
            queue.PushRedraw(-tick);
        }
        return std::pair{flips_taken, refused};
    };
    const auto [before_taken, before_refused] = run(true);
    const auto [taken, refused] = run(false);
    Check(before_refused > 0, "the model reproduces flips refused behind a queued redraw");
    Check(taken == 200 && refused == 0, "a queued redraw never holds back a guest flip");

    const std::string driver = Source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    Check(InOrder(driver, "bool VideoOutDriver::FlipWaitsForPresenter() {", "if (!swap_queue.FlipQueued()) {"),
          "the Gamescope flip gate waits only for a queued real flip");
    Check(InOrder(driver, "if (!redraw) {\n            swap_queue.PushFlip(std::move(work));",
                  "} else if (!swap_queue.PushRedraw(std::move(work))) {"),
          "a flip always queues, a redraw only on an idle swap thread");
    Check(InOrder(driver, "swap_queue.Finished();", "work = swap_queue.Take();") &&
              Contains(driver, "DcpStartupWatch::SetGauge(DcpStartupWatch::SwapQueue, swap_queue.Size());"),
          "the swap thread marks its work finished before taking more, and the queue gauge stays");
    Check(Contains(driver, "presenter->Present(empty_frame, false, false);\n    }, true);") &&
              Contains(driver, "presenter->Present(frame, true);\n        }\n    }, true);"),
          "the blank and last-frame redraws are queued as redraws");
}

// ---- The display refresh changing at runtime (Steam Deck 60 <-> 90 Hz, a move to a 120 Hz monitor).

// The window thread polls every 2 ms; the display refresh it sees over time.
struct RefreshTimeline {
    struct Step {
        std::uint64_t at_ms;
        double hz;
    };
    std::vector<Step> steps;

    double At(std::uint64_t ms) const {
        double hz = steps.front().hz;
        for (const Step& step : steps) {
            if (step.at_ms <= ms) {
                hz = step.hz;
            }
        }
        return hz;
    }
};

struct Published {
    std::uint64_t at_ms;
    std::uint64_t live;
};

std::vector<Published> Follow(std::uint64_t policy, const RefreshTimeline& timeline, std::uint64_t until_ms) {
    Pacing::RefreshFollower follower;
    follower.Start(policy);
    std::vector<Published> published;
    for (std::uint64_t ms = 0; ms <= until_ms; ms += 2) {
        if (const std::uint64_t live = follower.Observe(timeline.At(ms), ms)) {
            published.push_back({ms, live});
        }
    }
    return published;
}

void TestRefreshFollower() {
    // 120 FPS with VSync on a Steam Deck OLED: 60 -> 90 -> 60.
    const std::uint64_t vsync120 = Pacing::PackPolicy(120, true, 60);
    const RefreshTimeline deck{{{0, 60.0}, {10000, 90.0}, {20000, 60.0}}};
    const auto changes = Follow(vsync120, deck, 30000);
    Check(changes.size() == 2, "60 -> 90 -> 60 publishes two changes");
    if (changes.size() == 2) {
        Check(changes[0].at_ms >= 10000 + Pacing::kRefreshSettleMs && changes[0].at_ms <= 10000 + Pacing::kRefreshSettleMs + 4,
              "the change is published once the new refresh held for the settle time");
        Check(Pacing::LiveDisplayHz(changes[0].live) == 90 && Pacing::LiveVblankHz(changes[0].live) == 90,
              "a 90 Hz display gives 120 FPS with VSync a 90 Hz vblank");
        Check(Pacing::LiveDisplayHz(changes[1].live) == 60 && Pacing::LiveVblankHz(changes[1].live) == 60,
              "back to 60 Hz the vblank follows back");
    }
    const auto to120 = Follow(vsync120, RefreshTimeline{{{0, 60.0}, {5000, 119.88}}}, 10000);
    Check(to120.size() == 1 && Pacing::LiveVblankHz(to120[0].live) == 120 && Pacing::LiveDisplayHz(to120[0].live) == 120,
          "a move to a 119.88 Hz monitor gives 120 FPS its full 120 Hz vblank");
    const auto jitter = Follow(vsync120, RefreshTimeline{{{0, 60.0}, {1000, 59.94}, {2000, 60.02}, {3000, 0.0}}}, 10000);
    Check(jitter.empty(), "59.94 and 60 are the same rate, and an unknown (0) refresh changes nothing");
    const auto unknown_start = Follow(Pacing::PackPolicy(0, true, 0), RefreshTimeline{{{0, 144.0}}}, 2000);
    Check(unknown_start.size() == 1 && Pacing::LiveVblankHz(unknown_start[0].live) == 144,
          "a refresh learned after the driver started (it saw none) is applied once it settles");
    Check(Pacing::RetimeHz(60, Pacing::PackLive(90, 90)) == 90 && Pacing::RetimeHz(90, Pacing::PackLive(90, 90)) == 0 &&
              Pacing::RetimeHz(60, 0) == 0 && Pacing::RetimeHz(60, Pacing::PackLive(30, 30)) == 0,
          "the present thread retimes only to a different vblank of at least 60 Hz");
    // The setting's own semantics are kept: 30 and 60 never leave the 60 Hz vblank, VSync off on a desktop ignores the
    // display.
    for (const int setting : {30, 60}) {
        const auto original = Follow(Pacing::PackPolicy(setting, true, 60), deck, 30000);
        Check(original.size() == 2 && Pacing::LiveVblankHz(original[0].live) == 60 && Pacing::LiveVblankHz(original[1].live) == 60,
              Pacing::ModeName(setting) + " FPS keeps its 60 Hz vblank through refresh changes");
    }
    const auto desktop = Follow(Pacing::PackPolicy(120, false, 60), deck, 30000);
    Check(desktop.size() == 2 && Pacing::LiveVblankHz(desktop[0].live) == 120 && Pacing::LiveVblankHz(desktop[1].live) == 120,
          "VSync off outside Gamescope keeps 120 whatever the display");
}

void TestRefreshDebounceFlapping() {
    const std::uint64_t policy = Pacing::PackPolicy(0, true, 60);
    RefreshTimeline flapping;
    for (std::uint64_t ms = 0; ms < 10000; ms += 300) {
        flapping.steps.push_back({ms, (ms / 300) % 2 ? 90.0 : 60.0});
    }
    Check(Follow(policy, flapping, 10000).empty(), "a refresh flapping every 300 ms never retimes the vblank");
    RefreshTimeline fast;
    for (std::uint64_t ms = 0; ms < 5000; ms += 2) {
        fast.steps.push_back({ms, (ms / 2) % 2 ? 90.0 : 60.0});
    }
    Check(Follow(policy, fast, 5000).empty(), "nor does one changing on every poll");
    const auto settled = Follow(policy, RefreshTimeline{{{0, 60.0}, {1000, 90.0}, {1400, 120.0}, {1600, 90.0}}}, 5000);
    Check(settled.size() == 1 && Pacing::LiveVblankHz(settled[0].live) == 90 && settled[0].at_ms >= 1600 + Pacing::kRefreshSettleMs,
          "passing through 120 for 200 ms on the way publishes only the rate that settled");
    const auto back = Follow(policy, RefreshTimeline{{{0, 60.0}, {1000, 90.0}, {1400, 60.0}}}, 5000);
    Check(back.empty(), "a refresh that returns before it settles changes nothing");
}

// The present thread at the published rate: the production balance and RetimeHz, a refresh change applied between two
// vblanks as PresentThread does. The guest's clock is the host's (sceKernelGetProcessTimeCounter); Unity takes one
// frame per new vblank and steps its physics at a fixed 1/60 s from that clock (eboot 0x1464d00 compares the vblank count
// only for equality).
struct PresentSim {
    explicit PresentSim(unsigned hz) : hz(hz), balance(PeriodFor(hz)) {}

    void Vblank(nanoseconds work) {
        const nanoseconds sleep = balance.SleepNeeded();
        now += sleep;
        balance.Slept(sleep);
        starts.push_back(now);
        hz_at.push_back(hz);
        GuestFrame();
        now += work;
        balance.FrameFinished(work);
    }

    void Apply(std::uint64_t live) {
        if (const unsigned to = Pacing::RetimeHz(hz, live)) {
            balance.Retime(PeriodFor(to));
            hz = to;
            change_at.push_back(starts.size());
        }
    }

    // Unity's player loop on one new vblank: deltaTime from the clock, capped at 1/3 s; FixedUpdate at 1/60 s.
    void GuestFrame() {
        if (have_last) {
            accumulated += std::min(now - last_frame, nanoseconds(333333333));
            while (accumulated >= kStep) {
                accumulated -= kStep;
                ++physics_steps;
            }
        }
        have_last = true;
        last_frame = now;
    }

    int Bursts(size_t from, size_t to) const {
        int bursts = 0;
        for (size_t i = from + 1; i < to; ++i) {
            const nanoseconds shortest = std::min(PeriodFor(hz_at[i]), PeriodFor(hz_at[i - 1]));
            bursts += starts[i] - starts[i - 1] < shortest / 2 ? 1 : 0;
        }
        return bursts;
    }

    static constexpr nanoseconds kStep{16666667};
    unsigned hz;
    PacingBalance balance;
    nanoseconds now{};
    std::vector<nanoseconds> starts;
    std::vector<unsigned> hz_at;
    std::vector<size_t> change_at;
    bool have_last = false;
    nanoseconds last_frame{};
    nanoseconds accumulated{};
    long long physics_steps = 0;
};

void TestRefreshRetimes() {
    // 60 -> 90 -> 60 and 60 -> 120, each change landing right after a late frame (the worst case for a burst).
    struct Case {
        const char* name;
        std::vector<unsigned> rates;
    };
    const Case cases[] = {{"60 -> 90 -> 60", {60, 90, 60}}, {"60 -> 120", {60, 120}}, {"144 -> 60 -> 165", {144, 60, 165}}};
    for (const Case& c : cases) {
        PresentSim sim(c.rates.front());
        const int seconds = 10;
        for (size_t segment = 0; segment < c.rates.size(); ++segment) {
            if (segment > 0) {
                sim.Vblank(PeriodFor(sim.hz) * 5 / 2); // a frame 1.5 periods late just before the change
                sim.Apply(Pacing::PackLive(c.rates[segment], c.rates[segment]));
            }
            const int frames = seconds * static_cast<int>(sim.hz);
            for (int i = 0; i < frames; ++i) {
                sim.Vblank(PeriodFor(sim.hz) / 4 + Jitter(nanoseconds(200000)));
            }
        }
        const std::string name = c.name;
        Check(sim.change_at.size() == c.rates.size() - 1, name + ": every change retimes the vblank");
        Check(sim.Bursts(0, sim.starts.size()) == 0, name + ": no catch-up burst, also after a late frame at the change");
        bool exact = true, steady_counts = true;
        for (size_t segment = 0; segment < c.rates.size(); ++segment) {
            const size_t begin = segment == 0 ? 0 : sim.change_at[segment - 1];
            const size_t end = segment + 1 < c.rates.size() ? sim.change_at[segment] - 1 : sim.starts.size();
            const nanoseconds period = PeriodFor(c.rates[segment]);
            for (size_t i = begin + 2; i < end; ++i) {
                exact = exact && sim.starts[i] - sim.starts[i - 1] == period;
            }
            // No drift: the segment's vblanks over its wall time are its rate, to within one.
            const double wall = double((sim.starts[end - 1] - sim.starts[begin + 1]).count()) / 1e9;
            const double expected = wall * c.rates[segment];
            steady_counts = steady_counts && std::fabs(double(end - begin - 2) - expected) <= 1.0;
        }
        Check(exact, name + ": after each change every interval is the new period exactly");
        Check(steady_counts, name + ": no drift: each segment runs at exactly its rate");
        // The guest's time base is the clock: whatever the vblank does, the game simulates real time at 60 steps/s.
        const double elapsed = double((sim.starts.back() - sim.starts.front()).count()) / 1e9;
        Check(std::fabs(double(sim.physics_steps) - elapsed * 60.0) <= 2.0,
              name + ": the game's 1/60 s physics advances with real time (" + std::to_string(sim.physics_steps) +
                  " steps in " + std::to_string(elapsed) + " s), never faster or slower");
        // A guest that counted vblanks as 1/60 s ticks would run fast or slow: the reason it must not (and does not).
        const double vblank_seconds = double(sim.starts.size()) / 60.0;
        Check(std::fabs(vblank_seconds - elapsed) > 1.0, name + ": the vblank count is not a clock once the rate changes");
    }

    // Gamescope paces presents: Unlimited with VSync off on a Steam Deck switched 60 -> 90 Hz follows the panel.
    const std::uint64_t deck = Pacing::PackPolicy(0, Pacing::DisplayPacesPresents(false, true), 60);
    const auto published = Follow(deck, RefreshTimeline{{{0, 60.0}, {3000, 90.0}}}, 6000);
    Check(published.size() == 1 && Pacing::LiveVblankHz(published[0].live) == 90,
          "under Gamescope (display-paced, VSync off) the vblank follows the panel from 60 to 90 Hz");
    PresentSim gamescope(Pacing::GuestVblankHz(0, Pacing::DisplayPacesPresents(false, true), 60.0));
    for (int i = 0; i < 600; ++i) gamescope.Vblank(PeriodFor(gamescope.hz) / 3);
    gamescope.Apply(published.empty() ? 0 : published[0].live);
    const size_t changed = gamescope.starts.size();
    for (int i = 0; i < 900; ++i) gamescope.Vblank(PeriodFor(gamescope.hz) / 3);
    const nanoseconds across = gamescope.starts[changed] - gamescope.starts[changed - 1];
    Check(across <= PeriodFor(60) && across >= PeriodFor(90) / 2,
          "the vblank across the change comes within one old period: no stall, no restart from a fresh full wait");
    Check(gamescope.hz == 90 && gamescope.Bursts(0, gamescope.starts.size()) == 0 &&
              gamescope.starts.back() - gamescope.starts[gamescope.starts.size() - 2] == PeriodFor(90),
          "and the present thread runs at 90 Hz without a burst");
    const auto desktop = Follow(Pacing::PackPolicy(0, Pacing::DisplayPacesPresents(false, false), 60),
                                RefreshTimeline{{{0, 60.0}, {3000, 90.0}}}, 6000);
    Check(desktop.size() == 1 && Pacing::RetimeHz(480, desktop[0].live) == 0,
          "outside Gamescope with VSync off Unlimited stays at its 480 Hz cap");
}

void TestRefreshLogAndWiring() {
    unsigned lines = 0;
    char line[128];
    Check(Pacing::RefreshChangeLine(lines, 60, 90, 60, 90, line, sizeof(line)) &&
              std::string(line) == "VideoOut: display refresh changed 60 -> 90 Hz, guest vblank 60 -> 90 Hz",
          "one line per change names both rates");
    int written = 1;
    for (int i = 0; i < 100; ++i) {
        written += Pacing::RefreshChangeLine(lines, 90, 60, 90, 60, line, sizeof(line)) ? 1 : 0;
    }
    Check(written == static_cast<int>(Pacing::kRefreshLogLines) + 1 &&
              std::string(line) == "VideoOut: further display refresh changes are not logged",
          "a flapping display is logged a bounded number of times, then once more to say so");

    const std::string driver = Source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    Check(InOrder(driver, "EmulatorSettings.SetVblankFrequency(DcpFramePacing::GuestVblankHz(frame_rate, display_paced, display_hz));",
                  "DcpFramePacing::g_live_vblank.policy.store(DcpFramePacing::PackPolicy("),
          "the driver publishes the policy it chose the startup vblank with");
    const size_t thread = driver.find("void VideoOutDriver::PresentThread(");
    const std::string present = thread == std::string::npos ? std::string() : driver.substr(thread);
    Check(InOrder(present, "while (!token.stop_requested()) {",
                  "if (const u64 live = DcpFramePacing::g_live_vblank.target.load(std::memory_order_relaxed);") &&
              InOrder(present, "ApplyDisplayRefresh(timer, live, display_hz, refresh_lines);", "timer.Start();"),
          "the present thread pays one relaxed load per vblank and applies a change before the next wait");
    const size_t apply = driver.find("static void ApplyDisplayRefresh(");
    const std::string retime = apply == std::string::npos ? std::string() : driver.substr(apply, 900);
    Check(Contains(retime, "const unsigned to = DcpFramePacing::RetimeHz(from, live);") &&
              Contains(retime, "timer.Retime(std::chrono::nanoseconds(1000000000 / to));") &&
              Contains(retime, "EmulatorSettings.SetVblankFrequency(to);"),
          "a change retimes the shared limiter, never a new one with a fresh deadline");
    Check(!Contains(present, "refresh_rate") && !Contains(retime, "refresh_rate") &&
              Contains(driver, "main_port.resolution.refresh_rate = SCE_VIDEO_OUT_REFRESH_RATE_119_88HZ;"),
          "the refresh rate reported to the guest is decided once at startup and never flips mid-run");
    const std::string window = Source("gpu/shim/window.cpp");
    Check(Contains(window, "refresh_follower.Observe(DcpSettings::Get().display_refresh_hz.load(std::memory_order_relaxed),") &&
              Contains(window, "DcpFramePacing::g_live_vblank.target.store(live, std::memory_order_relaxed);") &&
              Contains(window, "    FollowDisplayRefresh();\n    UpdateMouseCapture();") &&
              Contains(window, "case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:"),
          "the window thread feeds every poll's display refresh to the follower");
    Check(Contains(window, "if (now - refresh_polled_at >= 2000) {") &&
              Contains(window, "DcpSettings::Get().display_refresh_hz = mode->refresh_rate;\n            }\n        }\n    }"),
          "the current display mode is read again every 2 s, for changes reported without a display event");
}

} // namespace

int main() {
    TestSteadyCadence();
    TestNoDrift();
    TestStallRecovery();
    TestPolicyMatrix();
    TestOriginalRates();
    TestArchitecture();
    TestGamescopePacesPresents();
    TestSwapQueueBacklog();
    TestFlipGateCountsOnlyFlips();
    TestRefreshFollower();
    TestRefreshDebounceFlapping();
    TestRefreshRetimes();
    TestRefreshLogAndWiring();
    std::printf("frame pacing: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
