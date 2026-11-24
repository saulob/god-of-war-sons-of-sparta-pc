// DEBUG_SAULO: minimal F5 capture gate (ignores repeat, caps at 8 per process).
#pragma once

#include <atomic>
#include <cstdint>

namespace DcpF5Capture {

inline constexpr uint32_t kMaxCaptures = 8;

struct Gate {
    std::atomic<uint32_t> count{0};

    template <typename RequestFn>
    bool TryRequest(bool repeat, RequestFn&& request) {
        if (repeat) {
            return false;
        }
        uint32_t current = count.load(std::memory_order_relaxed);
        if (current >= kMaxCaptures) {
            return false;
        }
        if (!request()) {
            return false;
        }
        while (current < kMaxCaptures &&
               !count.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
        }
        return true;
    }

    uint32_t Count() const {
        return count.load(std::memory_order_relaxed);
    }
};

} // namespace DcpF5Capture
