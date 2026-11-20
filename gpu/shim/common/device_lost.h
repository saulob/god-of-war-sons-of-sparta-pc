// dcp: the Vulkan device is dead after its first VK_ERROR_DEVICE_LOST. The first report owns the root cause, asks the
// driver for its fault once and ends the process. A thread that meets the lost device stops there: it never submits to
// it, waits on it or takes its work as completed.
#pragma once
#include <atomic>
#include <cstddef>
#include <string_view>
#include <utility>
#include "common/types.h"

namespace Common::DeviceLost {

inline constexpr s32 Success = 0;
inline constexpr s32 Timeout = 2;
inline constexpr s32 DeviceLostResult = -4;
inline constexpr int ExitStatus = 25;
inline constexpr u32 MaxSecondaryLines = 4;
inline constexpr u32 MaxFaultAddresses = 8;
inline constexpr u32 MaxVendorFaults = 4;
inline constexpr u64 WaitSliceNanoseconds = 100'000'000;

enum class Phase : u32 { Healthy, Lost, Reported };

// Each timeline semaphore counts its own ticks: a tick only means something with its timeline.
enum class Timeline : u32 { Draw, Present, Flip, SparseBind, Count };
inline constexpr u32 TimelineCount = u32(Timeline::Count);
const char* TimelineName(Timeline timeline) noexcept;

enum class Operation : u32 {
    QueueSubmit,
    QueueBindSparse,
    WaitSemaphores,
    GetSemaphoreCounterValue,
    WaitForFences,
    AcquireNextImage,
    QueuePresent,
    DeviceWaitIdle,
    QueryPoolResults,
    Checked,
    Count,
};

const char* OperationName(Operation operation) noexcept;
const char* ResultName(s32 result) noexcept;

enum class FaultStatus : u32 { NotQueried, Unsupported, Failed, Captured, Skipped };

struct FaultInfo {
    FaultStatus status = FaultStatus::NotQueried;
    s32 result = 0;
    char description[256]{};
    char heaps[192]{};
    // Counts as the driver reported them; only the first entries are kept.
    u32 address_count = 0;
    u32 vendor_count = 0;
    u64 vendor_binary_size = 0;
    struct Address {
        u32 type;
        u64 address;
        u64 precision;
    } addresses[MaxFaultAddresses]{};
    struct Vendor {
        u64 code;
        u64 data;
        char description[128];
    } vendors[MaxVendorFaults]{};
};

const char* FaultAddressTypeName(u32 type) noexcept;
// Driver text without line breaks or control characters, always terminated.
void CopyDriverText(char* out, size_t size, const char* text, size_t text_size) noexcept;

struct TimelineState {
    u64 submitted = 0;
    u64 completed = 0;
};

struct RootCause {
    Operation operation{};
    char site[32]{};
    s32 result = 0;
    u64 tick = 0;
    Timeline timeline = Timeline::Draw;
    u64 last_submitted_tick = 0;
    TimelineState timelines[TimelineCount]{};
    u32 thread_id = 0;
    bool simulated = false;
};

struct Hooks {
    void (*print)(const char* line);
    void (*query_fault)(FaultInfo& info);
    // Ends the process with `summary` as the reason; tests return from it.
    void (*shutdown)(const char* summary, int status);
    // Where a thread stays once shutdown is requested; tests return from it.
    void (*park)();
};

class Monitor {
public:
    constexpr explicit Monitor(const Hooks& hooks_) : hooks{hooks_}, query_fault{hooks_.query_fault} {}

    [[nodiscard]] bool IsLost() const noexcept {
        return phase.load(std::memory_order_acquire) != Phase::Healthy;
    }
    [[nodiscard]] Phase CurrentPhase() const noexcept {
        return phase.load(std::memory_order_acquire);
    }

    // True when this call became the owner of the root cause; a second fault is a consequence and changes nothing.
    bool Report(Operation operation, const char* site, s32 result, u64 tick, Timeline timeline = Timeline::Draw);
    void OnResult(Operation operation, const char* site, s32 result, u64 tick, Timeline timeline = Timeline::Draw) {
        if (result == DeviceLostResult) {
            Report(operation, site, result, tick, timeline);
        }
    }
    // A thread that found the device already lost.
    void Observe(const char* where);

    void SetFaultQuery(void (*query)(FaultInfo&)) noexcept {
        query_fault.store(query, std::memory_order_release);
    }
    [[nodiscard]] u32 FaultQueries() const noexcept {
        return fault_claimed.load(std::memory_order_acquire) ? 1u : 0u;
    }

    void NoteSubmitted(Timeline timeline, u64 tick) noexcept {
        submitted_ticks[u32(timeline)].store(tick, std::memory_order_relaxed);
    }
    void NoteCompleted(Timeline timeline, u64 tick) noexcept {
        completed_ticks[u32(timeline)].store(tick, std::memory_order_relaxed);
    }
    [[nodiscard]] TimelineState State(Timeline timeline) const noexcept {
        return {submitted_ticks[u32(timeline)].load(std::memory_order_relaxed),
                completed_ticks[u32(timeline)].load(std::memory_order_relaxed)};
    }

    // SOS_SIMULATE_DEVICE_LOST: the `calls`th call of `operation` reports a loss without reaching Vulkan.
    void ArmSimulation(Operation operation, u64 calls) noexcept;
    [[nodiscard]] bool Simulate(Operation operation) noexcept {
        if (simulated_operation.load(std::memory_order_relaxed) != u32(operation)) {
            return false;
        }
        if (simulated_calls.fetch_sub(1, std::memory_order_relaxed) != 1) {
            return false;
        }
        simulated_hit.store(true, std::memory_order_relaxed);
        return true;
    }

    // Valid once the phase is Reported.
    [[nodiscard]] const RootCause& Root() const noexcept {
        return root;
    }
    [[nodiscard]] const FaultInfo& Fault() const noexcept {
        return fault;
    }
    [[nodiscard]] const char* RootLine() const noexcept {
        return root_line;
    }
    [[nodiscard]] const char* TimelineLine() const noexcept {
        return timeline_line;
    }
    [[nodiscard]] const char* Summary() const noexcept {
        return summary;
    }
    [[nodiscard]] u32 SecondaryReports() const noexcept {
        return secondary_reports.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u32 ShutdownRequests() const noexcept {
        return shutdown_requests.load(std::memory_order_relaxed);
    }

private:
    void NoteSecondary(const char* where);
    void WaitForOwner();
    void RequestShutdown();
    void Park();
    void Print(const char* line);
    void QueryFaultOnce();
    void PrintFault();
    void BuildRootLine();
    void BuildTimelineLine();
    void BuildSummary();

    Hooks hooks;
    std::atomic<void (*)(FaultInfo&)> query_fault;
    std::atomic<Phase> phase{Phase::Healthy};
    std::atomic<u32> owner_thread{};
    std::atomic<bool> fault_claimed{};
    std::atomic<bool> shutdown_issued{};
    std::atomic<u32> shutdown_requests{};
    std::atomic<u32> secondary_reports{};
    std::atomic<u64> submitted_ticks[TimelineCount]{};
    std::atomic<u64> completed_ticks[TimelineCount]{};
    std::atomic<u32> simulated_operation{u32(Operation::Count)};
    std::atomic<u64> simulated_calls{};
    std::atomic<bool> simulated_hit{};
    RootCause root{};
    FaultInfo fault{};
    char root_line[320]{};
    char timeline_line[288]{};
    char summary[1024]{};
};

// The renderer's monitor: the log, the driver's fault report, the process exit.
Monitor& Global() noexcept;

[[nodiscard]] inline bool IsLost() noexcept {
    return Global().IsLost();
}

[[nodiscard]] u32 CurrentThreadId() noexcept;

// "submit:600" arms the 600th queue submission; false for anything else.
bool ParseSimulation(std::string_view value, Operation& operation, u64& calls) noexcept;
void ArmSimulationFromEnvironment();

// A Vulkan call that must not reach a lost device; a loss it returns is reported first.
template <typename Call>
s32 CallWhileHealthy(Monitor& monitor, Operation operation, const char* site, u64 tick, Timeline timeline,
                     Call&& call) {
    if (monitor.IsLost()) {
        monitor.Observe(OperationName(operation));
        return DeviceLostResult;
    }
    const s32 result = monitor.Simulate(operation) ? DeviceLostResult : static_cast<s32>(call());
    monitor.OnResult(operation, site, result, tick, timeline);
    return result;
}

// A wait made of bounded slices, so a loss seen by another thread ends it within one slice.
template <typename WaitOnce>
s32 WaitWhileHealthy(Monitor& monitor, Operation operation, const char* site, u64 tick, Timeline timeline,
                     WaitOnce&& wait_once) {
    bool first = true;
    for (;;) {
        if (monitor.IsLost()) {
            monitor.Observe(OperationName(operation));
            return DeviceLostResult;
        }
        const bool simulated = std::exchange(first, false) && monitor.Simulate(operation);
        const s32 result = simulated ? DeviceLostResult : static_cast<s32>(wait_once());
        if (result != Timeout) {
            monitor.OnResult(operation, site, result, tick, timeline);
            return result;
        }
    }
}

// The command processor takes no more guest GPU work once the device is lost.
template <typename StopToken>
[[nodiscard]] bool ShouldStop(const Monitor& monitor, const StopToken& stop) noexcept {
    return stop.stop_requested() || monitor.IsLost();
}

} // namespace Common::DeviceLost
