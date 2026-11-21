// Frame pacing on the production limiter arithmetic and the Frame rate policy, in simulated time (no sleeping): absolute
// slots without drift, a stall reanchors instead of bursting, every rate against every display refresh, the original
// 60 and the halved 30, and the guest-driven (not repeat-driven) high rate. No window, GPU, launcher or game.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/pacing_balance.h"
#include "dcp_frame_pacing.h"

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

} // namespace

int main() {
    TestSteadyCadence();
    TestNoDrift();
    TestStallRecovery();
    TestPolicyMatrix();
    TestOriginalRates();
    TestArchitecture();
    std::printf("frame pacing: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
