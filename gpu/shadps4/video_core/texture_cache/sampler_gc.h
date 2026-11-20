// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: when the sampler cache may destroy a sampler.
#pragma once

#include <algorithm>

#include "common/types.h"

namespace VideoCore {

struct SamplerGcLimits {
    u64 pressure;
    u64 critical;
};

struct SamplerGcRun {
    size_t evicted = 0;
    size_t pinned = 0;
};

/// A sampler a recorded draw uses stays alive until the GPU has finished that draw, however old its last use is.
constexpr bool SamplerInUseByGpu(u64 last_use_tick, u64 completed_tick) noexcept {
    return last_use_tick > completed_tick;
}

/// One collection over `lru`, oldest first: what is old enough and idle on the GPU goes, up to the pass's budget.
/// Only evictions spend the budget, so samplers the GPU still needs cannot hide idle ones behind them.
/// `count()` is read again after every eviction, `pinned(key)` says the GPU still needs the sampler and `evict(key)`
/// destroys it.
template <typename Lru, typename Count, typename Pinned, typename Evict>
SamplerGcRun CollectSamplers(Lru& lru, u64 gc_tick, const SamplerGcLimits& limits, Count&& count, Pinned&& pinned,
                             Evict&& evict) {
    SamplerGcRun run{};
    const auto pass = [&](bool allow_aggressive) {
        const bool pressured = count() >= limits.pressure;
        const bool aggressive = allow_aggressive && count() >= limits.critical;
        const u64 ticks_to_destroy = std::min<u64>(aggressive ? 160 : pressured ? 80 : 16, gc_tick);
        size_t deletions = aggressive ? 40 : pressured ? 20 : 10;
        lru.ForEachItemBelow(gc_tick - ticks_to_destroy, [&](u64 key) {
            if (deletions == 0) {
                return true;
            }
            if (pinned(key)) {
                ++run.pinned;
                return false;
            }
            --deletions;
            ++run.evicted;
            evict(key);
            return false;
        });
    };
    pass(false);
    if (count() >= limits.critical) {
        pass(true);
    }
    return run;
}

} // namespace VideoCore
