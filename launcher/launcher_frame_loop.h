#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sos {

inline constexpr int kLauncherTargetHz = 60;
inline constexpr int64_t kNsPerSecond = 1'000'000'000;
inline constexpr int64_t kNsPerMs = 1'000'000;
inline constexpr int64_t kLauncherFrameNs = kNsPerSecond / kLauncherTargetHz;
inline constexpr int64_t kMinimizedWakeNs = 100 * kNsPerMs;

struct FrameWait {
    bool draw = false;
    int64_t wait_ns = 0;
};

// Absolute deadlines on a monotonic clock: a frame that wakes late does not move the grid, and a frame later than half
// an interval re-anchors it, so a stall is never repaid with a burst. An interval of 0 never waits.
class FramePacer {
public:
    explicit FramePacer(int64_t interval_ns = kLauncherFrameNs) : interval_(interval_ns) {}

    FrameWait Next(int64_t now_ns) {
        if (interval_ <= 0) {
            return {true, 0};
        }
        if (!scheduled_) {
            scheduled_ = true;
            deadline_ = now_ns + interval_;
            return {true, 0};
        }
        if (deadline_ - now_ns > interval_) {
            deadline_ = now_ns + interval_;
        }
        if (now_ns < deadline_) {
            return {false, deadline_ - now_ns};
        }
        const int64_t late = now_ns - deadline_;
        deadline_ = late > interval_ / 2 ? now_ns + interval_ : deadline_ + interval_;
        return {true, 0};
    }

    void Reset() { scheduled_ = false; }
    int64_t interval_ns() const { return interval_; }

private:
    int64_t interval_;
    int64_t deadline_ = 0;
    bool scheduled_ = false;
};

enum class StepKind { kDraw, kSleep, kIdle };

struct LoopStep {
    StepKind kind = StepKind::kDraw;
    int64_t wait_ns = 0;
};

// What the launcher loop does next, whatever the renderer: draw, sleep to the next deadline, or (minimized) wait for an
// event for at most kMinimizedWakeNs with nothing built, submitted or presented.
class LoopPacer {
public:
    explicit LoopPacer(int64_t frame_ns = kLauncherFrameNs) : pacer_(frame_ns) {}

    LoopStep Next(int64_t now_ns, bool minimized) {
        if (minimized) {
            pacer_.Reset();
            return {StepKind::kIdle, kMinimizedWakeNs};
        }
        const FrameWait wait = pacer_.Next(now_ns);
        return wait.draw ? LoopStep{StepKind::kDraw, 0} : LoopStep{StepKind::kSleep, wait.wait_ns};
    }

    bool capped() const { return pacer_.interval_ns() > 0; }

private:
    FramePacer pacer_;
};

// UI-thread CPU share of a wall-clock interval in tenths of a percent; -1 when either sample is missing or time ran
// backwards.
inline int CpuPermille(int64_t start_cpu_ns, int64_t end_cpu_ns, int64_t elapsed_ns) {
    if (start_cpu_ns < 0 || end_cpu_ns < start_cpu_ns || elapsed_ns <= 0) {
        return -1;
    }
    return static_cast<int>(std::clamp<int64_t>((end_cpu_ns - start_cpu_ns) * 1000 / elapsed_ns, 0, 1000));
}

// Wall-clock time of the three parts of a drawn frame. Build is the UI construction (NewFrame to Render). Submit is
// recording the draw commands, which the SDL renderer only queues. Present executes the queued commands and presents,
// and may block on VSync.
struct FrameTimes {
    int64_t build_ns = 0;
    int64_t submit_ns = 0;
    int64_t present_ns = 0;
};

struct StallEvent {
    int64_t present_ns = 0;
    int64_t frame = 0;
    int suppressed = 0;
};

class EventLimiter {
public:
    EventLimiter(int64_t cooldown_ns, int max_events) : cooldown_(cooldown_ns), max_(max_events) {}

    bool Allow(int64_t now_ns) {
        if (emitted_ >= max_ || (emitted_ > 0 && now_ns - last_ < cooldown_)) {
            ++suppressed_;
            return false;
        }
        ++emitted_;
        last_ = now_ns;
        return true;
    }

    int TakeSuppressed() {
        const int count = suppressed_;
        suppressed_ = 0;
        return count;
    }

private:
    int64_t cooldown_;
    int max_;
    int emitted_ = 0;
    int suppressed_ = 0;
    int64_t last_ = 0;
};

struct PerformanceSample {
    int64_t elapsed_ns = 0;
    int64_t minimized_ns = 0;
    int64_t frames = 0;
    int64_t wakes = 0;
    FrameTimes total;
    int64_t slow_presents = 0;
    int hz_x10 = -1;
    int cpu_permille = -1;
};

// Counters only: a window of drawn frames and minimized wakes is closed every kWindowNs, and the lines it yields are
// bounded (a summary at the start and then once a minute, rate-limited warnings).
class PerformanceMonitor {
public:
    static constexpr int64_t kWarmupNs = 5 * kNsPerSecond;
    static constexpr int64_t kWindowNs = 5 * kNsPerSecond;
    static constexpr int64_t kSummaryEveryNs = 60 * kNsPerSecond;
    static constexpr int64_t kMinVisibleNs = kNsPerSecond;
    static constexpr int kExcessiveHzX10 = kLauncherTargetHz * 10 * 5 / 4;
    static constexpr int kExcessiveWindows = 2;
    static constexpr int64_t kStallPresentNs = 50 * kNsPerMs;

    explicit PerformanceMonitor(std::string_view renderer = "unknown")
        : renderer_(renderer), stalls_(10 * kNsPerSecond, 10), fast_(kSummaryEveryNs, 5) {}

    void SetRenderer(std::string_view renderer) { renderer_ = renderer; }

    void SetMinimized(bool minimized, int64_t now_ns) {
        if (!started_) {
            started_ = true;
            started_at_ = now_ns;
        }
        if (minimized == minimized_) {
            return;
        }
        if (minimized) {
            minimized_since_ = now_ns;
        } else {
            minimized_ns_ += std::max<int64_t>(now_ns - minimized_since_, 0);
        }
        minimized_ = minimized;
    }

    void Wake() {
        if (windowed_) {
            ++wakes_;
        }
    }

    std::optional<StallEvent> FrameDrawn(int64_t now_ns, const FrameTimes& times) {
        ++frame_index_;
        if (!windowed_) {
            return std::nullopt;
        }
        ++frames_;
        total_.build_ns += times.build_ns;
        total_.submit_ns += times.submit_ns;
        total_.present_ns += times.present_ns;
        if (times.present_ns < kStallPresentNs) {
            return std::nullopt;
        }
        ++slow_presents_;
        if (!stalls_.Allow(now_ns)) {
            return std::nullopt;
        }
        return StallEvent{times.present_ns, frame_index_, stalls_.TakeSuppressed()};
    }

    bool Due(int64_t now_ns) const {
        if (!started_) {
            return false;
        }
        return windowed_ ? now_ns - window_start_ >= kWindowNs : now_ns - started_at_ >= kWarmupNs;
    }

    std::vector<std::string> Close(int64_t now_ns, int64_t cpu_ns) {
        std::vector<std::string> lines;
        const int64_t elapsed = now_ns - window_start_;
        if (windowed_ && elapsed > 0) {
            PerformanceSample sample = Sample(now_ns, elapsed, cpu_ns);
            const bool summary = !summarized_ || now_ns - last_summary_ >= kSummaryEveryNs;
            if (summary) {
                summarized_ = true;
                last_summary_ = now_ns;
                lines.push_back(SummaryLine(sample));
            }
            fast_windows_ = sample.hz_x10 > kExcessiveHzX10 ? fast_windows_ + 1 : 0;
            if (fast_windows_ >= kExcessiveWindows && fast_.Allow(now_ns)) {
                lines.push_back(ExcessiveLine(sample, fast_.TakeSuppressed()));
            }
        }
        StartWindow(now_ns, cpu_ns);
        return lines;
    }

    std::string SummaryLine(const PerformanceSample& s) const {
        const auto average = [&](int64_t total_ns) { return s.frames > 0 ? double(total_ns) / double(s.frames) / double(kNsPerMs) : 0.0; };
        char hz[24], cpu[24];
        if (s.hz_x10 >= 0) {
            std::snprintf(hz, sizeof(hz), "%d.%d", s.hz_x10 / 10, s.hz_x10 % 10);
        } else {
            std::snprintf(hz, sizeof(hz), "0.0");
        }
        if (s.cpu_permille >= 0) {
            std::snprintf(cpu, sizeof(cpu), "%d.%d%%", s.cpu_permille / 10, s.cpu_permille % 10);
        } else {
            std::snprintf(cpu, sizeof(cpu), "n/a");
        }
        const int minimized = s.elapsed_ns > 0 ? int(std::clamp<int64_t>(s.minimized_ns * 100 / s.elapsed_ns, 0, 100)) : 0;
        char line[320];
        std::snprintf(line, sizeof(line),
                      "Launcher performance: renderer=%s render_hz=%s cpu=%s build=%.1fms submit=%.1fms present=%.1fms "
                      "frame=%.1fms slow_presents=%lld minimized=%d%% wakes=%lld",
                      renderer_.c_str(), hz, cpu, average(s.total.build_ns), average(s.total.submit_ns),
                      average(s.total.present_ns),
                      average(s.total.build_ns + s.total.submit_ns + s.total.present_ns),
                      static_cast<long long>(s.slow_presents), minimized, static_cast<long long>(s.wakes));
        return line;
    }

    std::string StallLine(const StallEvent& event, bool focused) const {
        char line[224];
        std::snprintf(line, sizeof(line),
                      "Launcher performance warning: present stall renderer=%s present=%lldms threshold=%lldms "
                      "frame=%lld focused=%d suppressed=%d",
                      renderer_.c_str(), static_cast<long long>(event.present_ns / kNsPerMs),
                      static_cast<long long>(kStallPresentNs / kNsPerMs), static_cast<long long>(event.frame), focused ? 1 : 0,
                      event.suppressed);
        return line;
    }

    std::string ExcessiveLine(const PerformanceSample& s, int suppressed) const {
        char line[224];
        std::snprintf(line, sizeof(line),
                      "Launcher performance warning: running too fast renderer=%s render_hz=%d.%d target_hz=%d windows=%d "
                      "suppressed=%d",
                      renderer_.c_str(), s.hz_x10 / 10, s.hz_x10 % 10, kLauncherTargetHz, fast_windows_, suppressed);
        return line;
    }

private:
    PerformanceSample Sample(int64_t now_ns, int64_t elapsed, int64_t cpu_ns) {
        if (minimized_) {
            minimized_ns_ += std::max<int64_t>(now_ns - minimized_since_, 0);
        }
        PerformanceSample sample;
        sample.elapsed_ns = elapsed;
        sample.minimized_ns = std::clamp<int64_t>(minimized_ns_, 0, elapsed);
        sample.frames = frames_;
        sample.wakes = wakes_;
        sample.total = total_;
        sample.slow_presents = slow_presents_;
        const int64_t visible = elapsed - sample.minimized_ns;
        if (visible >= kMinVisibleNs) {
            sample.hz_x10 = static_cast<int>(frames_ * 10 * kNsPerSecond / visible);
        }
        sample.cpu_permille = CpuPermille(window_cpu_, cpu_ns, elapsed);
        return sample;
    }

    void StartWindow(int64_t now_ns, int64_t cpu_ns) {
        windowed_ = true;
        window_start_ = now_ns;
        window_cpu_ = cpu_ns;
        frames_ = 0;
        wakes_ = 0;
        total_ = {};
        slow_presents_ = 0;
        minimized_ns_ = 0;
        minimized_since_ = now_ns;
    }

    std::string renderer_;
    EventLimiter stalls_;
    EventLimiter fast_;
    bool started_ = false;
    bool windowed_ = false;
    bool minimized_ = false;
    bool summarized_ = false;
    int64_t started_at_ = 0;
    int64_t window_start_ = 0;
    int64_t window_cpu_ = -1;
    int64_t last_summary_ = 0;
    int64_t minimized_since_ = 0;
    int64_t minimized_ns_ = 0;
    int64_t frames_ = 0;
    int64_t wakes_ = 0;
    int64_t frame_index_ = 0;
    int64_t slow_presents_ = 0;
    int fast_windows_ = 0;
    FrameTimes total_;
};

// Pumps events and returns once a frame is due (true) or the window was closed (false). The environment supplies the
// clock, the events and the sleeping: PumpEvents() -> bool, Minimized() -> bool, NowNs(), SleepNs(ns),
// WaitForEventNs(ns), ThreadCpuNs() and Emit(line).
template <class Env>
bool WaitForFrame(Env& env, LoopPacer& pacer, PerformanceMonitor& monitor) {
    for (;;) {
        if (!env.PumpEvents()) {
            return false;
        }
        const int64_t now = env.NowNs();
        const bool minimized = env.Minimized();
        if (pacer.capped()) {
            monitor.SetMinimized(minimized, now);
            if (monitor.Due(now)) {
                for (const std::string& line : monitor.Close(now, env.ThreadCpuNs())) {
                    env.Emit(line);
                }
            }
        }
        const LoopStep step = pacer.Next(now, minimized);
        switch (step.kind) {
        case StepKind::kDraw:
            return true;
        case StepKind::kSleep:
            env.SleepNs(step.wait_ns);
            break;
        case StepKind::kIdle:
            monitor.Wake();
            env.WaitForEventNs(step.wait_ns);
            break;
        }
    }
}

} // namespace sos
