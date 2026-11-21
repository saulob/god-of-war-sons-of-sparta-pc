// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

namespace Common {

// Sleep balance of a fixed-period loop. Lateness is carried so a late wake never shifts later frames, but a stall past
// kMaxLateIntervals reanchors instead of being repaid as a burst of immediate frames.
class PacingBalance {
public:
    static constexpr int kMaxLateIntervals = 2;

    explicit PacingBalance(std::chrono::nanoseconds interval) : interval(interval) {}

    std::chrono::nanoseconds SleepNeeded() const {
        return balance > std::chrono::nanoseconds::zero() ? balance : std::chrono::nanoseconds::zero();
    }

    void Slept(std::chrono::nanoseconds elapsed) {
        balance -= elapsed;
    }

    void FrameFinished(std::chrono::nanoseconds frame_time) {
        balance += interval - frame_time;
        if (balance < -kMaxLateIntervals * interval) {
            balance = std::chrono::nanoseconds::zero();
        }
    }

    std::chrono::nanoseconds Balance() const {
        return balance;
    }

private:
    std::chrono::nanoseconds interval;
    std::chrono::nanoseconds balance{};
};

} // namespace Common
