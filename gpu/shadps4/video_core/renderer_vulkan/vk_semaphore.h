// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include "common/device_lost.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct TimelineTicks {
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.
};

/// The known GPU tick only takes values the device reported, so nothing counts as completed on a lost device.
/// `read_counter(u64&)` is the Vulkan query and `fail(site, result, tick)` the caller's fatal path.
template <typename ReadCounter, typename Fail>
bool RefreshKnownTick(TimelineTicks& ticks, Common::DeviceLost::Monitor& monitor,
                      ReadCounter&& read_counter, Fail&& fail) {
    using namespace Common::DeviceLost;
    u64 known{};
    u64 counter{};
    do {
        known = ticks.gpu_tick.load(std::memory_order_acquire);
        const s32 result =
            CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue, "semread", known,
                             [&] { return read_counter(counter); });
        if (result != Success) {
            fail("semread", result, known);
            return false;
        }
        if (counter < known) {
            return true;
        }
    } while (!ticks.gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
                                                   std::memory_order_relaxed));
    return true;
}

/// Returns once the device has reached `tick`. `wait_once()` is one bounded wait for it.
template <typename WaitOnce, typename ReadCounter, typename Fail>
void WaitForTick(TimelineTicks& ticks, Common::DeviceLost::Monitor& monitor, u64 tick,
                 WaitOnce&& wait_once, ReadCounter&& read_counter, Fail&& fail) {
    using namespace Common::DeviceLost;
    if (ticks.gpu_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    if (!RefreshKnownTick(ticks, monitor, read_counter, fail) ||
        ticks.gpu_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    const s32 result = WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", tick, wait_once);
    if (result != Success) {
        fail("semwait", result, tick);
        return;
    }
    RefreshKnownTick(ticks, monitor, read_counter, fail);
}

class Semaphore {
public:
    explicit Semaphore(const Instance& instance_);
    ~Semaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return ticks.current_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return ticks.gpu_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return ticks.current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }

    /// Refresh the known GPU tick
    void Refresh();

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);

protected:
    vk::Result ReadCounter(u64& counter) const;
    [[noreturn]] void Fail(const char* site, s32 result, u64 tick) const;

    const Instance& instance;
    vk::UniqueSemaphore semaphore; ///< Timeline semaphore.
    TimelineTicks ticks;
};

} // namespace Vulkan
