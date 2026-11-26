// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: the bounded queue behind the pipeline cache's IO thread (cache_storage.cpp). One mutex
// guards the tasks and the counts, so they cannot disagree: once the worker has ended,
// Completed() + Failed() + Dropped() == Submitted(), and Overflowed() counts what was refused.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <utility>

#include "common/polyfill_thread.h"

namespace Storage {

class RequestQueue {
public:
    using Task = std::function<void()>;

    /// A cache write is optional: past these, new ones are refused (and counted), so a burst of
    /// new pipelines cannot grow the queue, or the memory its blobs hold, without limit.
    static constexpr std::size_t DefaultMaxTasks = 8192;
    static constexpr std::size_t DefaultMaxBytes = std::size_t{256} << 20;

    explicit RequestQueue(std::size_t max_tasks_ = DefaultMaxTasks,
                          std::size_t max_bytes_ = DefaultMaxBytes)
        : max_tasks{max_tasks_}, max_bytes{max_bytes_} {}

    /// False once the queue is closed, or when it is full; the task is then not kept. `bytes`
    /// is what the task holds (a blob), counted against the byte limit until it has run.
    bool Submit(Task task, std::size_t bytes = 0) {
        {
            std::scoped_lock lock{mutex};
            if (closed) {
                return false;
            }
            if (tasks.size() >= max_tasks || bytes > max_bytes - queued_bytes) {
                ++overflowed;
                return false;
            }
            tasks.emplace_back(std::move(task), bytes);
            queued_bytes += bytes;
            ++submitted;
        }
        cv.notify_one();
        return true;
    }

    /// The worker loop: runs tasks until stopped or closed. A stop request (the owner going away)
    /// abandons what is queued; Close finishes what fits in its time. A task that throws is
    /// counted as failed and the loop goes on.
    void Run(const std::stop_token& stoken) {
        for (;;) {
            Task task;
            {
                std::unique_lock lock{mutex};
                Common::CondvarWait(cv, lock, stoken, [this] { return closed || !tasks.empty(); });
                const bool expired = closed && Clock::now() >= deadline;
                if (stoken.stop_requested() || expired || (closed && tasks.empty())) {
                    dropped += tasks.size();
                    tasks.clear();
                    queued_bytes = 0;
                    return;
                }
                task = std::move(tasks.front().first);
                queued_bytes -= tasks.front().second;
                tasks.pop_front();
            }
            try {
                task();
                ++completed;
            } catch (...) {
                ++failed;
            }
        }
    }

    /// No more tasks are accepted; the worker finishes the queued ones for at most `drain`
    /// (one task that is already running is not interrupted).
    void Close(std::chrono::milliseconds drain) {
        {
            std::scoped_lock lock{mutex};
            if (!closed) {
                closed = true;
                deadline = Clock::now() + drain;
            }
        }
        cv.notify_all();
    }

    [[nodiscard]] std::size_t Pending() const {
        std::scoped_lock lock{mutex};
        return tasks.size();
    }
    [[nodiscard]] std::size_t PendingBytes() const {
        std::scoped_lock lock{mutex};
        return queued_bytes;
    }
    [[nodiscard]] std::uint64_t Submitted() const {
        std::scoped_lock lock{mutex};
        return submitted;
    }
    [[nodiscard]] std::uint64_t Completed() const {
        return completed;
    }
    [[nodiscard]] std::uint64_t Failed() const {
        return failed;
    }
    /// Accepted but never run: abandoned by a stop request or left when the drain time ran out.
    [[nodiscard]] std::uint64_t Dropped() const {
        std::scoped_lock lock{mutex};
        return dropped;
    }
    /// Refused by Submit because the queue was full.
    [[nodiscard]] std::uint64_t Overflowed() const {
        std::scoped_lock lock{mutex};
        return overflowed;
    }

private:
    using Clock = std::chrono::steady_clock;

    const std::size_t max_tasks;
    const std::size_t max_bytes;
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::pair<Task, std::size_t>> tasks;
    std::size_t queued_bytes{};
    Clock::time_point deadline{};
    std::uint64_t submitted{};
    std::uint64_t dropped{};
    std::uint64_t overflowed{};
    bool closed{};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> failed{0};
};

} // namespace Storage
