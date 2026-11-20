// Headless proof of the sampler cache lifetime on the production collector and the production LRU list, with stand-in
// samplers: a sampler is stamped with the tick of the draw that records it and is destroyed only after the GPU has
// completed that tick, however old its last use is. No window, GPU, launcher or game.
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/lru_cache.h"
#include "video_core/texture_cache/sampler_gc.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace fs = std::filesystem;
using namespace VideoCore;

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
    std::ifstream in(fs::path(SOS_STRING(SOS_SOURCE_DIR)) / path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::erase(text, '\r');
    return text;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// TextureCache::GetSampler and GarbageCollectSamplers with a counter for the Vulkan samplers.
struct Cache {
    struct Entry {
        size_t lru_id{};
        u64 last_use_tick{};
        u32 handle{};
    };

    Common::LeastRecentlyUsedCache<u64, u64> lru;
    std::unordered_map<u64, Entry> samplers;
    std::map<u32, u32> destroyed;
    u32 created = 0;
    u64 gc_tick = 0;
    u64 current_tick = 1;
    u64 completed_tick = 0;
    size_t destroyed_in_flight = 0;
    size_t pinned_seen = 0;

    u32 Get(u64 key) {
        const auto [it, added] = samplers.try_emplace(key);
        if (added) {
            it->second.lru_id = lru.Insert(key, gc_tick);
            it->second.handle = ++created;
        } else {
            lru.Touch(it->second.lru_id, gc_tick);
        }
        it->second.last_use_tick = current_tick;
        return it->second.handle;
    }

    SamplerGcRun Collect(const SamplerGcLimits& limits) {
        const SamplerGcRun run = CollectSamplers(
            lru, gc_tick, limits, [this] { return samplers.size(); },
            [this](u64 key) { return SamplerInUseByGpu(samplers.at(key).last_use_tick, completed_tick); },
            [this](u64 key) {
                const Entry& entry = samplers.at(key);
                destroyed_in_flight += entry.last_use_tick > completed_tick;
                ++destroyed[entry.handle];
                lru.Free(entry.lru_id);
                samplers.erase(key);
            });
        pinned_seen += run.pinned;
        return run;
    }

    bool Alive(u64 key) const {
        return samplers.contains(key);
    }
};

constexpr SamplerGcLimits Limits{.pressure = 1000, .critical = 2000};

void TestTracking() {
    Cache cache;
    cache.current_tick = 7;
    cache.Get(1);
    Check(cache.samplers.at(1).last_use_tick == 7, "a new sampler is stamped with the tick of the draw that records it");
    cache.current_tick = 12;
    cache.gc_tick = 3;
    const u32 again = cache.Get(1);
    Check(cache.samplers.at(1).last_use_tick == 12 && again == 1, "a cache hit updates the last use tick");
    cache.Get(2);
    cache.gc_tick = 9;
    cache.Get(1);
    Check(cache.samplers.size() == 2 && cache.created == 2, "repeated hits create no second Vulkan sampler");
    int order = 0;
    u64 first = 0;
    cache.lru.ForEachItemBelow(100, [&](u64 key) {
        if (order++ == 0) {
            first = key;
        }
    });
    Check(first == 2, "a reused sampler moves behind the older one in the LRU list");
    for (int i = 0; i < 500; ++i) {
        cache.Get(1);
    }
    Check(cache.created == 2, "five hundred hits still leave two Vulkan samplers");
}

void TestOldSamplersFollowTheGpu() {
    Cache cache;
    cache.current_tick = 100;
    cache.Get(1);
    cache.current_tick = 50;
    for (u64 key = 10; key < 19; ++key) {
        cache.Get(key);
    }
    cache.gc_tick = 1000;
    cache.completed_tick = 99;
    cache.Collect({.pressure = 5, .critical = 8});
    Check(cache.Alive(1) && cache.destroyed_in_flight == 0, "an old sampler with pending GPU work stays alive");
    Check(!cache.Alive(10) || !cache.Alive(11), "samplers the GPU is done with are collected");

    Cache lagging;
    lagging.current_tick = 100;
    lagging.Get(1);
    lagging.gc_tick = 1000;
    lagging.completed_tick = 99;
    lagging.Collect(Limits);
    Check(lagging.Alive(1), "one old sampler with pending GPU work is not collected");
    lagging.completed_tick = 100;
    lagging.gc_tick = 1001;
    lagging.Collect(Limits);
    Check(!lagging.Alive(1), "the sampler is collectable at once when the completed tick reaches its last use");

    Cache fresh;
    fresh.current_tick = 5;
    fresh.completed_tick = 5;
    fresh.gc_tick = 30;
    fresh.Get(1);
    fresh.gc_tick = 31;
    fresh.Collect(Limits);
    Check(fresh.Alive(1), "a sampler the GPU finished with but the CPU used a moment ago is kept: both rules must allow it");
    fresh.gc_tick = 31 + 16;
    fresh.Collect(Limits);
    Check(!fresh.Alive(1), "and it goes once it is old enough");

    Cache reused;
    reused.current_tick = 10;
    reused.Get(1);
    reused.completed_tick = 10;
    reused.current_tick = 40;
    reused.gc_tick = 5;
    reused.Get(1);
    reused.gc_tick = 500;
    reused.completed_tick = 30;
    reused.Collect(Limits);
    Check(reused.Alive(1) && reused.destroyed_in_flight == 0,
          "a sampler reused by a later draw is pinned by that draw's tick, not by its first use");
    reused.completed_tick = 40;
    reused.Collect(Limits);
    Check(!reused.Alive(1), "and goes after that draw completes");
}

void TestPinnedSamplersDoNotHideIdleOnes() {
    Cache cache;
    cache.current_tick = 900;
    for (u64 key = 0; key < 300; ++key) {
        cache.Get(key);
    }
    cache.current_tick = 10;
    for (u64 key = 1000; key < 1030; ++key) {
        cache.gc_tick = 1;
        cache.Get(key);
    }
    cache.gc_tick = 5000;
    cache.completed_tick = 100;
    const SamplerGcRun run = cache.Collect({.pressure = 100000, .critical = 200000});
    Check(run.evicted == 10, "three hundred pinned old samplers do not use up or end the pass budget");
    Check(cache.destroyed_in_flight == 0 && run.pinned >= 300, "the pinned ones were skipped, not destroyed");
    for (u64 key = 0; key < 300; ++key) {
        if (!cache.Alive(key)) {
            Check(false, "a pinned sampler was destroyed");
            break;
        }
    }
}

void TestDestructionExactlyOnce() {
    Cache cache;
    std::mt19937 random{11};
    for (u64 tick = 1; tick <= 3000; ++tick) {
        cache.gc_tick = tick;
        cache.current_tick = tick;
        cache.completed_tick = tick > 2 ? tick - 2 : 0;
        for (int i = 0; i < 6; ++i) {
            cache.Get(random() % 400);
        }
        cache.Collect({.pressure = 150, .critical = 250});
    }
    bool once = true;
    for (const auto& [handle, count] : cache.destroyed) {
        once &= count == 1;
    }
    Check(once && !cache.destroyed.empty(), "every destroyed Vulkan sampler is destroyed exactly once");
    Check(cache.destroyed_in_flight == 0, "no sampler is destroyed before the GPU completed its last draw");
    Check(cache.samplers.size() < 400 + 64, "the cache stays bounded");
}

void TestLaggingGpuStaysBounded() {
    Cache cache;
    constexpr SamplerGcLimits limits{.pressure = 3500, .critical = 3750};
    constexpr u64 trigger = 3000;
    std::mt19937 random{3};
    std::vector<u64> hot;
    for (u64 i = 0; i < 64; ++i) {
        hot.push_back(i);
    }
    size_t worst = 0;
    u64 next_key = 1000;
    for (u64 gc_tick = 1; gc_tick <= 6000; ++gc_tick) {
        cache.gc_tick = gc_tick;
        cache.current_tick = gc_tick * 3;
        for (const u64 key : hot) {
            cache.Get(key);
        }
        for (int i = 0; i < 8; ++i) {
            cache.current_tick = gc_tick * 3 - random() % 3;
            cache.Get(next_key++);
        }
        cache.current_tick = gc_tick * 3;
        cache.completed_tick = gc_tick * 3 - 4;
        if (cache.samplers.size() >= trigger) {
            cache.Collect(limits);
        }
        worst = std::max(worst, cache.samplers.size());
    }
    std::printf("  samplers: %u created, %zu live at most, %zu destroyed while in use\n", cache.created, worst,
                cache.destroyed_in_flight);
    Check(cache.destroyed_in_flight == 0, "a lagging GPU never loses a sampler it still uses");
    Check(worst < 4000, "the cache stays under the device sampler limit while the GPU lags");
    for (const u64 key : hot) {
        Check(cache.Alive(key), "a sampler used every tick is never collected");
    }

    Cache stalled;
    for (u64 key = 0; key < 3500; ++key) {
        stalled.gc_tick = 1;
        stalled.current_tick = 10;
        stalled.Get(key);
    }
    stalled.gc_tick = 500;
    stalled.completed_tick = 0;
    const SamplerGcRun run = stalled.Collect(limits);
    Check(run.evicted == 0 && stalled.samplers.size() == 3500 && run.pinned <= 2 * 3500,
          "a GPU that completes nothing (device lost) pins everything and the pass still returns, bounded");
}

void TestProductionWiring() {
    const std::string cache = Source("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");
    const auto get = cache.find("vk::Sampler TextureCache::GetSampler(");
    const auto get_end = cache.find("void TextureCache::RegisterImage", get);
    const std::string body = get == std::string::npos ? std::string{} : cache.substr(get, get_end - get);
    Check(Contains(body, "it.value().last_use_tick = scheduler.CurrentTick();"),
          "GetSampler stamps new and reused samplers with the scheduler tick of the draw being recorded");
    Check(Contains(body, "sampler_lru_cache.Touch(it->second.lru_id, gc_tick);"), "a hit still touches the LRU list");
    const auto gc = cache.find("void TextureCache::GarbageCollectSamplers()");
    const auto gc_end = cache.find("void TextureCache::RunGarbageCollector()", gc);
    const std::string collector = gc == std::string::npos ? std::string{} : cache.substr(gc, gc_end - gc);
    Check(Contains(collector, "SamplerInUseByGpu(samplers.at(hash).last_use_tick, completed_tick)") &&
              Contains(collector, "KnownGpuTick()") && Contains(collector, "CollectSamplers("),
          "the collector keeps a sampler until the known GPU tick reaches its last use");
    Check(!Contains(collector, "Wait(") && !Contains(collector, "waitIdle") && !Contains(collector, "WaitIdle"),
          "the collector never waits for the GPU");
    Check(Contains(collector, "std::scoped_lock lock{samplers_mutex};"), "the collector runs under the sampler cache lock");
    Check(Contains(Source("gpu/shadps4/video_core/texture_cache/sampler.h"), "u64 last_use_tick{};"),
          "the sampler record has its last use tick");
    Check(!Contains(collector, "ticks_to_destroy") && !Contains(collector, "num_deletions"),
          "the age-only collection is gone from the texture cache");
}

} // namespace

int main() {
    TestTracking();
    TestOldSamplersFollowTheGpu();
    TestPinnedSamplersDoNotHideIdleOnes();
    TestDestructionExactlyOnce();
    TestLaggingGpuStaysBounded();
    TestProductionWiring();
    std::printf("sampler lifetime tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
