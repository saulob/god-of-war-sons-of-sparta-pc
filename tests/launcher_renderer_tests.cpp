// Launcher renderer policy, 60 Hz pacing, minimized behavior and telemetry on the production headers in simulated time
// (no sleeping), plus gates on the host wiring and on the Direct3D 11 teardown hazard. No window, GPU, launcher or game.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "launcher_frame_loop.h"
#include "launcher_renderer.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

using namespace sos;

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

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int Count(const std::string& text, const std::string& part) {
    int count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

bool Before(const std::string& text, const std::string& first, const std::string& second) {
    const size_t a = text.find(first);
    const size_t b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t a = text.find(from);
    if (a == std::string::npos) {
        return "";
    }
    const size_t b = text.find(to, a + from.size());
    return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

double Field(const std::string& line, const std::string& key) {
    const size_t at = line.find(key);
    return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + key.size());
}

constexpr int64_t kI = kLauncherFrameNs;
constexpr int64_t kStart = 1'000'000'000'000;

struct Costs {
    int64_t build_ns = 1 * kNsPerMs;
    int64_t submit_ns = 500'000;
    int64_t present_ns = 100'000;
};

// The environment of sos::WaitForFrame with a simulated clock. A minimized wait ends at the next event when one is due.
struct FakeEnv {
    int64_t now = kStart;
    int64_t cpu_ns = 0;
    int64_t jitter_ns = 0;
    int64_t vblank_ns = 0;
    int64_t pumps = 0;
    int64_t sleeps = 0;
    int64_t waits = 0;
    int64_t slept_ns = 0;
    int64_t quit_at = INT64_MAX;
    std::function<bool(int64_t)> minimized_at = [](int64_t) { return false; };
    std::vector<int64_t> events;
    std::vector<std::string> lines;

    bool PumpEvents() { return now < quit_at && ++pumps < 20'000'000; }
    bool Minimized() { return minimized_at(now); }
    int64_t NowNs() { return now; }
    void SleepNs(int64_t ns) {
        ++sleeps;
        slept_ns += ns;
        now += ns + jitter_ns;
    }
    void WaitForEventNs(int64_t ns) {
        ++waits;
        for (const int64_t event : events) {
            if (event > now && event <= now + ns) {
                now = event;
                return;
            }
        }
        now += ns;
    }
    int64_t ThreadCpuNs() { return cpu_ns; }
    void Emit(const std::string& line) { lines.push_back(line); }
};

struct Run {
    std::vector<int64_t> draws;
    std::vector<StallEvent> stalls;
};

// Draws frames until `duration` passed: each costs build + submit + present (a present on a VSync grid waits for the next
// vblank), the thread burns CPU for build and submit only.
Run Simulate(FakeEnv& env, LoopPacer& pacer, PerformanceMonitor& monitor, int64_t duration, Costs costs = {},
             const std::function<int64_t(size_t)>& stall = nullptr) {
    Run run;
    const int64_t end = env.now + duration;
    env.quit_at = end;
    while (env.now < end) {
        if (!WaitForFrame(env, pacer, monitor)) {
            break;
        }
        run.draws.push_back(env.now);
        int64_t at = env.now + costs.build_ns + costs.submit_ns + (stall ? stall(run.draws.size()) : 0);
        const int64_t before_present = at;
        at += costs.present_ns;
        if (env.vblank_ns > 0) {
            at = (at + env.vblank_ns - 1) / env.vblank_ns * env.vblank_ns;
        }
        env.cpu_ns += costs.build_ns + costs.submit_ns;
        env.now = at;
        if (pacer.capped()) {
            if (const auto event = monitor.FrameDrawn(env.now, {costs.build_ns, costs.submit_ns, env.now - before_present})) {
                run.stalls.push_back(*event);
            }
        }
    }
    return run;
}

int64_t MinGap(const std::vector<int64_t>& draws, size_t from = 1) {
    int64_t gap = INT64_MAX;
    for (size_t i = std::max<size_t>(from, 1); i < draws.size(); ++i) {
        gap = std::min(gap, draws[i] - draws[i - 1]);
    }
    return gap;
}

size_t CountIn(const std::vector<int64_t>& draws, int64_t from, int64_t to) {
    size_t count = 0;
    for (const int64_t at : draws) {
        count += at >= from && at < to ? 1 : 0;
    }
    return count;
}

void TestPacing() {
    {
        FakeEnv env;
        env.jitter_ns = 300'000;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 10 * kNsPerSecond);
        Check(run.draws.size() >= 599 && run.draws.size() <= 602, "60 Hz: 10 s of visible launcher draws 600 frames");
        Check(MinGap(run.draws) >= kI - 400'000, "60 Hz: no frame closer than one interval to the previous one");
        Check(env.sleeps >= 590, "60 Hz: every early frame sleeps until its deadline");
    }
    {
        FakeEnv env;
        env.jitter_ns = 1'200'000;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 600 * kNsPerSecond);
        Check(run.draws.size() >= 35998 && run.draws.size() <= 36003, "long run: 10 minutes with 1.2 ms late wakes draw 36000 frames, no drift");
        const double mean_ms = double(run.draws.back() - run.draws.front()) / double(run.draws.size() - 1) / 1e6;
        Check(std::fabs(mean_ms - 16.6667) < 0.003, "long run: the mean interval stays 16.667 ms");
    }
    {
        FakeEnv a, b;
        LoopPacer pa, pb;
        PerformanceMonitor ma("software"), mb("direct3d11");
        const Run ra = Simulate(a, pa, ma, 30 * kNsPerSecond);
        const Run rb = Simulate(b, pb, mb, 30 * kNsPerSecond);
        Check(ra.draws == rb.draws && !ra.draws.empty(), "the software renderer and an accelerated candidate share one cadence");
    }
    {
        FakeEnv env;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        Simulate(env, pacer, monitor, 5 * kNsPerSecond, {1 * kNsPerMs, 0, 0});
        Check(env.slept_ns > 4 * kNsPerSecond && env.slept_ns < 5 * kNsPerSecond, "early completion sleeps the rest of the interval");
    }
    {
        FramePacer pacer;
        const FrameWait first = pacer.Next(0);
        const FrameWait on_time = pacer.Next(kI);
        Check(first.draw && on_time.draw && on_time.wait_ns == 0, "a frame exactly on its deadline does not sleep");
        const FrameWait early = pacer.Next(kI + 1'000'000);
        Check(!early.draw && early.wait_ns == kI - 1'000'000, "the wait is the time left to the absolute deadline");
        FramePacer uncapped(0);
        Check(uncapped.Next(5).draw && uncapped.Next(5).draw && uncapped.Next(5).wait_ns == 0, "an interval of 0 never waits");
        FramePacer backwards;
        backwards.Next(10 * kNsPerSecond);
        const FrameWait moved = backwards.Next(1 * kNsPerSecond);
        Check(!moved.draw && moved.wait_ns <= kI, "a clock that moved backwards waits at most one interval");
    }
    {
        FakeEnv env;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        Run run;
        uint32_t seed = 12345;
        run = Simulate(env, pacer, monitor, 60 * kNsPerSecond, {}, [&](size_t) {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 16) % 50 == 0 ? int64_t(25 * kNsPerMs + (seed % 20) * kNsPerMs) : int64_t(0);
        });
        Check(MinGap(run.draws) >= kI / 2 - 1000, "late frames are not repaid: no frame follows another by less than half an interval");
        Check(run.draws.size() <= 60 * 60 + 2, "late frames never push the average above 60 Hz");
    }
    {
        FakeEnv env;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 20 * kNsPerSecond, {}, [](size_t index) {
            return index == 300 ? int64_t(2 * kNsPerSecond) : int64_t(0);
        });
        const int64_t after = run.draws[300];
        size_t burst = 0;
        for (size_t i = 301; i < run.draws.size() && run.draws[i] < after + 1 * kNsPerSecond; ++i) {
            ++burst;
        }
        Check(run.draws[300] - run.draws[299] >= 2 * kNsPerSecond, "the stalled frame really took 2 s");
        Check(burst <= 61, "a 2 s stall is followed by at most 60 frames in the next second");
        Check(run.draws[301] - run.draws[300] >= kI - 1000, "a 2 s stall re-anchors: a full interval follows the first frame after it");
        Check(MinGap(run.draws, 300) >= kI - 1000, "after the re-anchor the cadence is one interval again");
    }
    {
        FakeEnv env;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 60 * kNsPerSecond, {}, [](size_t index) {
            return index % 400 == 0 ? int64_t(300 * kNsPerMs + (index % 7) * 100 * kNsPerMs) : int64_t(0);
        });
        Check(run.draws.size() <= 60 * 60 + 2, "repeated stalls keep the rate at or below 60 Hz");
        size_t worst = 0;
        for (size_t i = 0; i < run.draws.size(); ++i) {
            worst = std::max(worst, CountIn(run.draws, run.draws[i], run.draws[i] + 1 * kNsPerSecond));
        }
        Check(worst <= 61, "repeated stalls never allow more than 61 frames in any second");
    }
    for (const int64_t vblank : {int64_t(16'666'667), int64_t(16'700'000), int64_t(6'944'444), int64_t(33'333'333)}) {
        FakeEnv env;
        env.vblank_ns = vblank;
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 20 * kNsPerSecond);
        const double expected = vblank >= 33'000'000 ? 30.0 : 60.0;
        const double hz = double(run.draws.size()) / 20.0;
        Check(std::fabs(hz - expected) < 1.5, "a present blocked on a " + std::to_string(vblank / 1000) + " us vblank gives " + std::to_string(int(expected)) + " Hz, not half of it");
    }
    {
        int64_t now = INT64_MAX / 2;
        FramePacer pacer;
        int64_t frames = 0;
        const int64_t end = now + 7 * 24 * 3600 * kNsPerSecond;
        bool negative = false;
        while (now < end) {
            const FrameWait wait = pacer.Next(now);
            if (wait.draw) {
                ++frames;
                now += 100'000;
            } else {
                negative = negative || wait.wait_ns <= 0;
                now += wait.wait_ns;
            }
        }
        const int64_t expected = 7 * 24 * 3600 * 60;
        Check(frames >= expected - 2 && frames <= expected + 2 && !negative, "seven days of 60 Hz near a huge clock value: exact count, no overflow, no bad wait");
    }
}

void TestMinimized() {
    {
        FakeEnv env;
        env.minimized_at = [](int64_t) { return true; };
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        env.quit_at = env.now + 10 * kNsPerSecond;
        int64_t draws = 0;
        while (WaitForFrame(env, pacer, monitor)) {
            ++draws;
            env.now += kI;
        }
        Check(draws == 0, "a minimized launcher never draws, so it builds, submits and presents nothing");
        Check(env.sleeps == 0, "a minimized launcher does not use the frame sleep");
        Check(env.waits >= 95 && env.waits <= 105, "a minimized launcher wakes about ten times a second");
        Check(env.pumps <= env.waits + 3, "a minimized launcher pumps events once per wake, no busy loop");
        Check(kMinimizedWakeNs >= 50 * kNsPerMs && kMinimizedWakeNs <= 250 * kNsPerMs, "the minimized wake is 50 to 250 ms");
    }
    {
        FakeEnv env;
        const int64_t restore = kStart + 5 * kNsPerSecond + 37 * kNsPerMs;
        env.minimized_at = [=](int64_t now) { return now < restore; };
        env.events = {restore};
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 7 * kNsPerSecond);
        Check(!run.draws.empty() && run.draws.front() == restore, "restore with an event draws at the instant of the event");
        Check(CountIn(run.draws, kStart, restore) == 0, "nothing is drawn before the restore");
    }
    {
        FakeEnv env;
        const int64_t restore = kStart + 5 * kNsPerSecond + 37 * kNsPerMs;
        env.minimized_at = [=](int64_t now) { return now < restore; };
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, 7 * kNsPerSecond);
        Check(!run.draws.empty() && run.draws.front() - restore <= kMinimizedWakeNs, "restore without an event is noticed within one wake");
    }
    {
        FakeEnv env;
        std::vector<std::pair<int64_t, int64_t>> spans;
        uint32_t seed = 99;
        int64_t at = kStart + 1 * kNsPerSecond;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int64_t length = 50 * kNsPerMs + (seed >> 8) % (400 * kNsPerMs);
            spans.push_back({at, at + length});
            env.events.push_back(at + length);
            at += length + 30 * kNsPerMs + (seed >> 4) % (300 * kNsPerMs);
        }
        env.minimized_at = [&](int64_t now) {
            for (const auto& span : spans) {
                if (now >= span.first && now < span.second) {
                    return true;
                }
            }
            return false;
        };
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        const Run run = Simulate(env, pacer, monitor, at - kStart + 2 * kNsPerSecond);
        bool minimized_draw = false;
        for (const int64_t draw : run.draws) {
            minimized_draw = minimized_draw || env.minimized_at(draw);
        }
        Check(!minimized_draw, "200 minimize and restore transitions never draw a minimized frame");
        Check(run.draws.size() > 200, "the launcher keeps drawing after every restore");
        Check(env.pumps < 100'000, "repeated transitions stay stable without a busy loop");
    }
    {
        FakeEnv env;
        LoopPacer pacer(0);
        PerformanceMonitor monitor("software");
        int64_t draws = 0;
        for (int i = 0; i < 1000; ++i) {
            draws += WaitForFrame(env, pacer, monitor) ? 1 : 0;
        }
        Check(draws == 1000 && env.sleeps == 0 && env.lines.empty(), "a hidden launcher is not limited and has no telemetry");
    }
}

struct Feeder {
    PerformanceMonitor monitor;
    std::vector<std::string> lines;
    std::vector<StallEvent> stalls;
    int64_t cpu_ns = 0;
    explicit Feeder(const char* renderer = "software") : monitor(renderer) {}

    void Pump(int64_t now, bool minimized, int64_t cpu_rate_permille) {
        monitor.SetMinimized(minimized, now);
        if (monitor.Due(now)) {
            for (std::string& line : monitor.Close(now, cpu_ns)) {
                lines.push_back(std::move(line));
            }
        }
        (void)cpu_rate_permille;
    }

    void Frames(int64_t from, int64_t duration, int64_t period, FrameTimes times, int64_t cpu_per_frame = 0) {
        for (int64_t t = from; t < from + duration; t += period) {
            Pump(t, false, 0);
            cpu_ns += cpu_per_frame;
            if (const auto stall = monitor.FrameDrawn(t, times)) {
                stalls.push_back(*stall);
            }
        }
    }

    int Count(const std::string& part) const {
        int n = 0;
        for (const std::string& line : lines) {
            n += line.find(part) != std::string::npos ? 1 : 0;
        }
        return n;
    }
};

void TestTelemetry() {
    {
        Feeder f("software");
        f.Frames(0, 20 * kNsPerSecond, kI, {2'000'000, 1'000'000, 500'000}, 3 * kNsPerMs);
        const std::string& first = f.lines.empty() ? std::string() : f.lines.front();
        Check(f.lines.size() == 1 && first.rfind("Launcher performance: ", 0) == 0, "the first summary appears once the window closed, nothing before");
        Check(std::fabs(Field(first, "render_hz=") - 60.0) < 0.6, "render Hz over a 5 s window is about 60");
        Check(first.find("renderer=software") != std::string::npos, "the summary names the renderer");
        Check(first.find("build=2.0ms submit=1.0ms present=0.5ms frame=3.5ms") != std::string::npos, "build, submit, present and frame averages aggregate");
        Check(std::fabs(Field(first, "cpu=") - 18.0) < 1.0, "UI thread CPU is its time over the window (3 ms of 16.7 ms)");
        Check(f.Count("warning") == 0, "a normal 60 Hz launcher raises no warning");
    }
    {
        Feeder f("direct3d11");
        f.Frames(0, 40 * kNsPerSecond, 5 * kNsPerMs, {1'000'000, 500'000, 100'000});
        Check(f.Count("running too fast") >= 1, "a sustained 200 Hz launcher is flagged");
        bool ok = false;
        for (const std::string& line : f.lines) {
            if (line.find("running too fast") != std::string::npos) {
                ok = line.find("renderer=direct3d11") != std::string::npos && Field(line, "render_hz=") > 190.0 &&
                     Field(line, "target_hz=") == 60.0;
            }
        }
        Check(ok, "the warning carries the renderer, the measured Hz and the target");
    }
    {
        Feeder f("software");
        f.Frames(0, 600 * kNsPerSecond, 5 * kNsPerMs, {1'000'000, 500'000, 100'000});
        Check(f.Count("running too fast") <= 5, "the sustained-fast warning is rate limited");
        Check(f.Count("running too fast") >= 1, "the rate limit still lets the first warning through");
    }
    {
        Feeder f("software");
        f.Frames(0, 7 * kNsPerSecond, kI, {1'000'000, 500'000, 100'000});
        f.Frames(7 * kNsPerSecond, 1 * kNsPerSecond, 5 * kNsPerMs, {1'000'000, 500'000, 100'000});
        f.Frames(8 * kNsPerSecond, 30 * kNsPerSecond, kI, {1'000'000, 500'000, 100'000});
        Check(f.Count("running too fast") == 0, "one short fast burst is not flagged");
    }
    {
        Feeder f("software");
        f.Frames(0, 30 * kNsPerSecond, kI, {1'000'000, 500'000, 49 * kNsPerMs});
        Check(f.stalls.empty(), "a present just below the threshold is not a stall");
        Feeder g("software");
        g.Frames(0, 30 * kNsPerSecond, kI, {1'000'000, 500'000, 50 * kNsPerMs});
        Check(!g.stalls.empty(), "a present at the threshold is a stall");
        Check(PerformanceMonitor::kStallPresentNs == 50 * kNsPerMs && PerformanceMonitor::kStallPresentNs >= 3 * kI, "the stall threshold is 50 ms, three frame intervals");
        Feeder h("software");
        h.Frames(0, 4 * kNsPerSecond, kI, {1'000'000, 500'000, 400 * kNsPerMs});
        Check(h.stalls.empty(), "stalls during the warm-up are not reported");
    }
    {
        Feeder f("software");
        f.Frames(0, 65 * kNsPerSecond, kI, {1'000'000, 500'000, 120 * kNsPerMs});
        Check(f.stalls.size() >= 2 && f.stalls.size() <= 7, "a driver stalling every frame yields a handful of reports in a minute");
        Check(f.stalls.size() >= 2 && f.stalls[0].suppressed == 0 && f.stalls[1].suppressed > 100, "the next report counts what was suppressed");
        Feeder g("software");
        g.Frames(0, 600 * kNsPerSecond, kI, {1'000'000, 500'000, 120 * kNsPerMs});
        Check(g.stalls.size() <= 10, "stall reports are capped for the whole session");
        const std::string line = g.monitor.StallLine(g.stalls.front(), true);
        Check(line.find("renderer=software") != std::string::npos && line.find("present=120ms") != std::string::npos &&
                  line.find("focused=1") != std::string::npos && line.find("frame=") != std::string::npos,
              "a stall line names the renderer, the present time, focus and the frame number");
    }
    {
        Check(CpuPermille(1'000'000'000, 1'200'000'000, 5'000'000'000) == 40, "CPU: 200 ms of thread time in 5 s is 4.0%");
        Check(CpuPermille(0, 0, 5'000'000'000) == 0, "CPU: no thread time is 0%");
        Check(CpuPermille(0, 100, 0) == -1 && CpuPermille(0, 100, -5) == -1, "CPU: a zero or negative elapsed time is unavailable");
        Check(CpuPermille(500, 100, 1000) == -1, "CPU: thread time running backwards is unavailable");
        Check(CpuPermille(-1, 100, 1000) == -1 && CpuPermille(100, -1, 1000) == -1, "CPU: a missing sample is unavailable");
        Check(CpuPermille(0, 9'000'000'000, 1'000'000'000) == 1000, "CPU: an overshoot is clamped to 100%");
        PerformanceMonitor monitor("software");
        monitor.SetMinimized(false, 0);
        monitor.Close(5 * kNsPerSecond, -1);
        const std::vector<std::string> lines = monitor.Close(10 * kNsPerSecond, -1);
        Check(lines.size() == 1 && lines[0].find("cpu=n/a") != std::string::npos, "a missing CPU sample is reported as n/a, not 0");
    }
    {
        PerformanceMonitor monitor("software");
        monitor.SetMinimized(false, 5 * kNsPerSecond);
        Check(monitor.Close(10 * kNsPerSecond, 0).empty(), "the end of the warm-up only starts the window");
        Check(monitor.Close(10 * kNsPerSecond, 0).empty(), "a window closed with zero elapsed time is ignored");
        Check(monitor.Close(9 * kNsPerSecond, 0).empty(), "a window closed with negative elapsed time is ignored");
        Check(monitor.Close(20 * kNsPerSecond, 0).size() == 1, "the monitor keeps working after invalid intervals");
    }
    {
        Feeder f("software");
        f.Pump(0, false, 0);
        f.Frames(0, 5 * kNsPerSecond, kI, {1'000'000, 500'000, 100'000});
        f.Frames(5 * kNsPerSecond, 2500 * kNsPerMs, kI, {1'000'000, 500'000, 100'000});
        f.Pump(7500 * kNsPerMs, true, 0);
        for (int64_t t = 7500 * kNsPerMs; t < 10 * kNsPerSecond; t += 100 * kNsPerMs) {
            f.monitor.Wake();
            f.Pump(t, true, 0);
        }
        f.Pump(10 * kNsPerSecond, false, 0);
        const std::string line = f.lines.empty() ? std::string() : f.lines.front();
        Check(Field(line, "minimized=") == 50.0, "a window half minimized reports minimized=50%");
        Check(std::fabs(Field(line, "render_hz=") - 60.0) < 1.0, "render Hz is measured over the visible time only");
        Check(Field(line, "wakes=") >= 24 && Field(line, "wakes=") <= 26, "the minimized wakes are counted");
    }
    {
        Feeder f("software");
        f.Pump(0, true, 0);
        for (int64_t t = 0; t <= 12 * kNsPerSecond; t += 100 * kNsPerMs) {
            f.monitor.Wake();
            f.Pump(t, true, 0);
        }
        const std::string line = f.lines.empty() ? std::string() : f.lines.front();
        Check(Field(line, "minimized=") == 100.0 && Field(line, "render_hz=") == 0.0 && Field(line, "wakes=") > 40, "a fully minimized window reports minimized=100% and no Hz");
        Check(f.Count("warning") == 0, "a minimized launcher raises no warning");
    }
    {
        FakeEnv env;
        env.minimized_at = [](int64_t) { return true; };
        LoopPacer pacer;
        PerformanceMonitor monitor("software");
        env.quit_at = kStart + 600 * kNsPerSecond;
        while (WaitForFrame(env, pacer, monitor)) {
        }
        Check(env.lines.size() <= 11, "ten minutes minimized log no more than eleven lines");
        FakeEnv visible;
        LoopPacer pacer2;
        PerformanceMonitor monitor2("software");
        Simulate(visible, pacer2, monitor2, 600 * kNsPerSecond);
        Check(visible.lines.size() >= 5 && visible.lines.size() <= 12, "ten minutes visible log one summary a minute, not one line per frame");
    }
    {
        const PerformanceMonitor monitor("Direct3D 11!");
        const PerformanceSample sample;
        StallEvent event;
        Check(monitor.SummaryLine(sample).find("renderer=Direct3D 11!") != std::string::npos, "the renderer name is printed as given to the monitor");
        Check(RendererLabel("Direct3D 11!") == "direct3d_11_" && RendererLabel("") == "unknown", "the renderer label is a plain token");
        Check(monitor.StallLine(event, false).find("renderer=") != std::string::npos && monitor.ExcessiveLine(sample, 0).find("renderer=") != std::string::npos,
              "every line type names the renderer");
    }
}

void TestRendererPolicy() {
    using R = RendererRequest;
    const auto names = [](const std::vector<std::string_view>& order) {
        std::string text;
        for (const std::string_view name : order) {
            text += "[" + std::string(name) + "]";
        }
        return text;
    };
    Check(names(LauncherRendererOrder(true, R::kAuto)) == "[software]", "Windows defaults to the software renderer");
    Check(names(LauncherRendererOrder(true, R::kSoftware)) == "[software]", "Windows software override is software alone");
    Check(names(LauncherRendererOrder(true, R::kDirect3D11)) == "[direct3d11][software]", "Windows Direct3D 11 is opt-in and always has software behind it");
    Check(names(LauncherRendererOrder(false, R::kAuto)) == "[][software]", "other platforms keep SDL's first working renderer, then software");
    Check(names(LauncherRendererOrder(false, R::kSoftware)) == "[software]", "other platforms honor the software override");
    Check(names(LauncherRendererOrder(false, R::kDirect3D11)) == "[][software]", "Direct3D 11 is ignored where it does not exist");
    Check(ParseRendererRequest(nullptr) == R::kAuto && ParseRendererRequest("") == R::kAuto && ParseRendererRequest("auto") == R::kAuto, "no value and auto are automatic");
    Check(ParseRendererRequest("software") == R::kSoftware && ParseRendererRequest("direct3d11") == R::kDirect3D11, "the two named renderers parse");
    Check(ParseRendererRequest("D3D11") == R::kAuto && ParseRendererRequest("vulkan") == R::kAuto && ParseRendererRequest(" software") == R::kAuto,
          "anything else is automatic, never an accelerated backend");

    struct Probe {
        std::vector<std::string> calls;
        std::vector<std::string> pin_fail;
        std::vector<std::string> create_fail;
        bool Pin(std::string_view module) {
            calls.push_back("pin:" + std::string(module));
            for (const std::string& name : pin_fail) {
                if (name == module) {
                    return false;
                }
            }
            return true;
        }
        bool Create(const std::string& name) {
            calls.push_back("create:" + name);
            for (const std::string& fail : create_fail) {
                if (fail == name) {
                    return false;
                }
            }
            return true;
        }
    };
    const auto select = [](Probe& probe, bool windows, R request) {
        return SelectRenderer(
            LauncherRendererOrder(windows, request), [&](std::string_view m) { return probe.Pin(m); },
            [&](const std::string& n) { return probe.Create(n); });
    };
    const auto joined = [](const Probe& p) {
        std::string text;
        for (const std::string& call : p.calls) {
            text += call + ",";
        }
        return text;
    };
    {
        Probe probe;
        const RendererSelection s = select(probe, true, R::kAuto);
        Check(s.created && s.name == "software" && !s.fallback && s.attempts == 1 && s.pinned == 0 && joined(probe) == "create:software,",
              "software initialization succeeds on the first attempt, nothing pinned");
    }
    {
        Probe probe;
        probe.create_fail = {"direct3d11"};
        const RendererSelection s = select(probe, true, R::kDirect3D11);
        Check(s.created && s.name == "software" && s.fallback && s.attempts == 2, "an accelerated renderer that fails to initialize falls back to software");
        Check(joined(probe) == "pin:d3d11.dll,pin:dxgi.dll,create:direct3d11,create:software,", "modules are pinned before the accelerated renderer, each renderer created once");
    }
    {
        Probe probe;
        probe.pin_fail = {"dxgi.dll"};
        const RendererSelection s = select(probe, true, R::kDirect3D11);
        Check(s.created && s.name == "software" && s.fallback && joined(probe) == "pin:d3d11.dll,pin:dxgi.dll,create:software,",
              "an accelerated renderer whose modules cannot be pinned is never created");
    }
    {
        Probe probe;
        probe.create_fail = {"direct3d11", "software"};
        const RendererSelection s = select(probe, true, R::kDirect3D11);
        Check(!s.created && s.attempts == 2 && Count(joined(probe), "create:") == 2, "when every renderer fails there is no retry loop");
        Probe none;
        none.create_fail = {"software"};
        const RendererSelection t = select(none, true, R::kAuto);
        Check(!t.created && t.attempts == 1 && Count(joined(none), "create:") == 1, "a failing software renderer is attempted once");
    }
    {
        Probe probe;
        const RendererSelection s = select(probe, true, R::kSoftware);
        Check(s.created && joined(probe) == "create:software,", "the software override never pins or creates anything accelerated");
        Probe linux_probe;
        linux_probe.create_fail = {""};
        const RendererSelection l = select(linux_probe, false, R::kAuto);
        Check(l.created && l.name == "software" && l.fallback && joined(linux_probe) == "create:,create:software,", "elsewhere software follows a failed first-working renderer");
    }
    {
        Probe a, b;
        const RendererSelection x = select(a, true, R::kDirect3D11);
        const RendererSelection y = select(b, true, R::kDirect3D11);
        Check(x.name == "direct3d11" && !x.fallback && x.pinned == 2 && x.name == y.name && joined(a) == joined(b),
              "the experimental renderer behaves deterministically");
    }
    const std::string line = RendererSelectedLine("software", R::kAuto, false, 0);
    Check(line == "Launcher renderer: selected=software requested=auto fallback=0 pinned_modules=0", "the selection line names renderer, request, fallback and pins");
    Check(RendererSelectedLine("direct3d11", R::kDirect3D11, true, 2).find("requested=direct3d11 fallback=1 pinned_modules=2") != std::string::npos, "the selection line reports a fallback");
}

void TestWiring() {
    const std::string host = Source("launcher/host.cpp");
    const std::string header = Source("launcher/host.h");
    const std::string launcher = Source("launcher/launcher.cpp");
    const std::string renderer = Source("launcher/launcher_renderer.h");
    const std::string loop = Source("launcher/launcher_frame_loop.h");
    const std::string window = Source("gpu/shim/window.cpp");
    Check(!host.empty() && !header.empty() && !launcher.empty() && !renderer.empty() && !loop.empty() && !window.empty(), "the sources under test were read");

    // The Dragon hazard: a Direct3D 11 device released while the NVIDIA driver's threads still return through d3d11.dll.
    Check(Count(host, "SDL_CreateRenderer(") == 1 && Has(host, "SDL_CreateRenderer(window, name.empty() ? nullptr : name.c_str())"),
          "Dragon gate: the one renderer creation takes its name from the order, never SDL's automatic choice on Windows");
    Check(Before(host, "LauncherRendererOrder(kWindows, request)", "SDL_CreateRenderer("), "Dragon gate: the order is the policy function");
    Check(!Has(host, "direct3d") && !Has(host, "d3d11") && !Has(host, "SDL_HINT_RENDER_DRIVER"),
          "Dragon gate: no Direct3D name or render driver hint outside the policy header");
    const std::string order = Between(renderer, "inline std::vector<std::string_view> LauncherRendererOrder", "inline std::vector<std::string_view> RendererModulesToPin");
    Check(Count(order, "\"direct3d11\"") == 1 && Has(order, "if (request == RendererRequest::kDirect3D11) {\n        return {\"direct3d11\", \"software\"};"),
          "Dragon gate: the order names Direct3D 11 only behind the explicit request, with software behind it");
    const std::string pin = Between(host, "bool PinModule(std::string_view name) {", "int64_t ThreadCpuNs() {");
    Check(Has(pin, "LoadLibraryW(") && Has(pin, "GET_MODULE_HANDLE_EX_FLAG_PIN"), "Dragon gate: the modules are pinned so SDL's unload cannot unmap them");
    Check(Before(renderer, "pin(module)", "create(name)"), "Dragon gate: pinning precedes creation in the selection");
    Check(Has(renderer, "if (!pinned) {\n            continue;"), "Dragon gate: a failed pin skips the renderer without creating it");
    bool unload = false;
    for (const char* file : {"launcher/host.cpp", "launcher/launcher.cpp", "launcher/launcher_renderer.h", "launcher/launcher_frame_loop.h"}) {
        const std::string text = Source(file);
        unload = unload || Has(text, "FreeLibrary") || Has(text, "SDL_UnloadObject") || Has(text, "UnloadObject");
    }
    Check(!unload, "Dragon gate: the launcher never unloads a module itself");
    const std::string close = Between(host, "void LauncherHost::Close() {", "void LauncherHost::HandleEvent");
    Check(Before(close, "SDL_DestroyTexture(texture);", "ImGui_ImplSDLRenderer3_Shutdown();") && Before(close, "ImGui::DestroyContext(imgui_);", "SDL_DestroyRenderer(renderer_);") &&
              Before(close, "SDL_DestroyRenderer(renderer_);", "SDL_DestroyWindow(window_);") && Before(close, "SDL_DestroyWindow(window_);", "SDL_Quit();"),
          "Dragon gate: teardown stays textures, ImGui, renderer, window, SDL");
    Check(Before(launcher, "host_.Close();", "\"closed, starting the game\"") && Has(launcher, "return outcome == Outcome::kPlay ? SOS_LAUNCHER_PLAY : SOS_LAUNCHER_QUIT;"),
          "Dragon gate: the launcher renderer is still torn down in this process before the game starts, so the default must stay software");
    Check(!Has(window, "SDL_CreateRenderer") && !Has(window, "SDL_SOFTWARE_RENDERER") && !Has(window, "D3D11CreateDevice") && !Has(window, "d3d11"),
          "Dragon gate: the game window never uses an SDL renderer or Direct3D 11");
    Check(Has(host, "const RendererRequest request = ParseRendererRequest(std::getenv(kLauncherRendererVariable));") &&
              Has(renderer, "kLauncherRendererVariable = \"SOS_LAUNCHER_RENDERER\""),
          "the override is the SOS_LAUNCHER_RENDERER variable and nothing else");

    const std::string selected = Between(host, "monitor_.SetRenderer(", "ConfigureVsync();");
    Check(Count(host, "RendererSelectedLine(") == 1 && Has(selected, "std::printf(") && Has(selected, "RendererSelectedLine("), "the selected renderer is logged once, after the selection");
    Check(Has(host, "monitor_.SetRenderer(RendererLabel(SDL_GetRendererName(renderer_)));"), "the telemetry carries the real renderer name");
    Check(Count(launcher, "WaitForFrame()") == 3 && !Has(launcher, "PumpEvents") && !Has(launcher, "Minimized()") && Count(launcher, "SDL_Delay(") == 1,
          "all three launcher loops wait through the host; only the closed-window wait sleeps");
    Check(Has(launcher, "if (!host_.WaitForFrame()) {\n                outcome = Outcome::kQuit;") && Has(launcher, "while (dialog.open() && host.WaitForFrame())") &&
              Has(launcher, "if (open && !host.WaitForFrame()) open = false;"),
          "the main, explain and preparing loops each use the paced wait");
    const std::string frame = Between(launcher, "    Outcome Frame() {", "ImGuiIO& io = ImGui::GetIO();");
    Check(Has(frame, "TrackPlacement();") && Has(frame, "PollInstall();") && Has(frame, "PollUpdate();"), "restore runs the background polls again on its first frame");
    Check(Before(launcher, "if (!host_.WaitForFrame()) {", "host_.BeginFrame();\n            outcome = Frame();"), "a frame is built only after the paced wait");

    Check(Has(host, "return sos::WaitForFrame(env, loop_, monitor_);") && Has(host, "loop_ = LoopPacer(hidden_ ? 0 : kLauncherFrameNs);"), "the host loop is the production pacer, capped at 60 Hz unless hidden");
    Check(Has(host, "void SleepNs(int64_t ns) { SDL_DelayNS(static_cast<Uint64>(ns)); }") && !Has(host, "SDL_Delay(") && !Has(host, "yield") && !Has(host, "_mm_pause"),
          "the frame wait is one high-resolution sleep, no spin");
    const std::string wait = Between(host, "void LauncherHost::WaitForEvent(", "bool LauncherHost::Minimized()");
    Check(Has(wait, "SDL_WaitEventTimeout(") && !Has(wait, "SDL_PollEvent"), "the minimized wait blocks on the event queue with a timeout");
    Check(Has(host, "return static_cast<int64_t>(SDL_GetTicksNS());") && !Has(host, "system_clock") && !Has(host, "GetTickCount"), "the clock is SDL's monotonic nanosecond counter");
    Check(!Has(host, "std::thread") && !Has(host, "CreateThread") && !Has(host, "SDL_CreateThread") && !Has(loop, "std::thread"), "there is no telemetry or timer thread");
    const std::string end_frame = Between(host, "void LauncherHost::EndFrame(", "bool LauncherHost::Fullscreen()");
    Check(Has(end_frame, "NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});") &&
              Before(end_frame, "const int64_t presenting = NowNs();", "SDL_RenderPresent(renderer_);") && Before(end_frame, "SDL_RenderPresent(renderer_);", "const int64_t presented = NowNs();"),
          "build, submit and present are timed around Render, the draw commands and SDL_RenderPresent");
    const std::string note = Between(host, "void LauncherHost::NoteFrameDrawn(", "void LauncherHost::BeginFrame()");
    Check(Count(note, "std::printf(") == 1 && Has(note, "if (loop_.capped()) {") && Has(note, "monitor_.FrameDrawn(now_ns, times)"), "a frame logs only when the monitor returns a stall");
    Check(Has(host, "GetThreadTimes(GetCurrentThread(),") && Has(host, "CLOCK_THREAD_CPUTIME_ID") && !Has(host, "GetProcessTimes") && !Has(host, "CLOCK_PROCESS_CPUTIME_ID"),
          "the CPU sample is the UI thread's own time");
    Check(Has(host, "SDL_SetRenderVSync(renderer_, 0);") && Has(host, "SDL_GetWindowSurfaceVSync(window_, &surface_value)") && Has(host, "pacing=%dhz_deadline vsync_effective=%d"),
          "a simulated VSync is turned off, the pacing mode is logged once");
    Check(!Has(loop, "SDL_") && !Has(loop, "#include <chrono>"), "the pacing and telemetry header is renderer and platform independent");
}

} // namespace

int main() {
    TestPacing();
    TestMinimized();
    TestTelemetry();
    TestRendererPolicy();
    TestWiring();
    std::printf("launcher renderer: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
