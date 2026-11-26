// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: what a device allocation does when the preferred memory is full.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

namespace VideoCore {

/// A heap's label in memory diagnostics: "device-local", "device-local, host-visible" (a UMA
/// carve-out such as the Steam Deck's, or ReBAR) or "system".
inline const char* MemoryHeapKind(const vk::PhysicalDeviceMemoryProperties& properties, u32 heap) {
    if (heap >= properties.memoryHeapCount) {
        return "unknown";
    }
    const bool device_local =
        static_cast<bool>(properties.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal);
    bool host_visible = false;
    for (u32 type = 0; type < properties.memoryTypeCount; ++type) {
        if (properties.memoryTypes[type].heapIndex == heap &&
            (properties.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)) {
            host_visible = true;
        }
    }
    return Vulkan::GpuCompat::HeapKindName(device_local, host_visible);
}

/// "type 3 (heap 1, system)": the memory type an allocation really used, for fallback warnings.
inline std::string MemoryTypeLabel(const vk::PhysicalDeviceMemoryProperties& properties, u32 type) {
    if (type >= properties.memoryTypeCount) {
        return "type " + std::to_string(type);
    }
    const u32 heap = properties.memoryTypes[type].heapIndex;
    return "type " + std::to_string(type) + " (heap " + std::to_string(heap) + ", " +
           MemoryHeapKind(properties, heap) + ")";
}

/// Memory types an allocation may use, in the order they are tried: the preferred type, the other device-local types,
/// then the system memory types. A type needs its bit in `allowed_bits` and every `required` property, and protected,
/// lazily allocated and AMD device-coherent types never stand in for another one.
inline std::vector<u32> FallbackMemoryTypes(const vk::PhysicalDeviceMemoryProperties& properties,
                                            u32 allowed_bits, u32 preferred,
                                            vk::MemoryPropertyFlags required = {}) {
    using Flag = vk::MemoryPropertyFlagBits;
    constexpr vk::MemoryPropertyFlags never = Flag::eProtected | Flag::eLazilyAllocated |
                                              Flag::eDeviceCoherentAMD | Flag::eDeviceUncachedAMD;
    const u32 count = std::min<u32>(properties.memoryTypeCount, 32);
    const auto usable = [&](u32 type) {
        if (type >= count || ((allowed_bits >> type) & 1) == 0) {
            return false;
        }
        const auto flags = properties.memoryTypes[type].propertyFlags;
        return (flags & required) == required && !(flags & never);
    };
    std::vector<u32> types;
    if (usable(preferred)) {
        types.push_back(preferred);
    }
    for (const bool device_local : {true, false}) {
        for (u32 type = 0; type < count; ++type) {
            const bool is_local =
                static_cast<bool>(properties.memoryTypes[type].propertyFlags & Flag::eDeviceLocal);
            if (type != preferred && usable(type) && is_local == device_local) {
                types.push_back(type);
            }
        }
    }
    return types;
}

struct FallbackAllocation {
    vk::Result result;
    u32 candidate;
    u32 attempts;
};

/// Each candidate is tried once, in order; only a full device memory moves on to the next one, any other result
/// (device loss, host memory, a failed bind) is the caller's as it came.
template <typename Allocate>
FallbackAllocation AllocateWithFallback(std::span<const u32> candidates, Allocate&& allocate) {
    FallbackAllocation allocation{vk::Result::eErrorOutOfDeviceMemory, 0, 0};
    for (const u32 candidate : candidates) {
        ++allocation.attempts;
        allocation.candidate = candidate;
        allocation.result = allocate(candidate);
        if (allocation.result != vk::Result::eErrorOutOfDeviceMemory) {
            break;
        }
    }
    return allocation;
}

enum class AllocationCategory : u32 { SparseBacking, FrameImage };

inline const char* AllocationCategoryName(AllocationCategory category) noexcept {
    return category == AllocationCategory::SparseBacking ? "buffer arena backing" : "frame image";
}

struct AllocationFallbackEvent {
    u64 size;
    AllocationCategory category;
    u32 original;
    u32 fallback;
    bool device_local;
};

/// How often the preferred memory was full, kept for the logs and the device-lost summary.
class AllocationTelemetry {
public:
    static constexpr u32 MaxLoggedEvents = 16;

    /// True when the event is worth a log line: the first few only, a game that stays short of memory does not spam.
    bool Note(const AllocationFallbackEvent& event) noexcept {
        const u32 seen = events.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(event.size, std::memory_order_relaxed);
        if (!event.device_local) {
            system_events.fetch_add(1, std::memory_order_relaxed);
            system_bytes.fetch_add(event.size, std::memory_order_relaxed);
        }
        return seen < MaxLoggedEvents;
    }

    [[nodiscard]] u32 Events() const noexcept {
        return events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u32 SystemEvents() const noexcept {
        return system_events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 Bytes() const noexcept {
        return bytes.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 SystemBytes() const noexcept {
        return system_bytes.load(std::memory_order_relaxed);
    }

    /// "" when the preferred memory was never full.
    size_t Describe(char* out, size_t size) const noexcept {
        if (size == 0) {
            return 0;
        }
        out[0] = '\0';
        if (Events() == 0) {
            return 0;
        }
        const int written =
            std::snprintf(out, size, "memory fallbacks: %u allocations (%llu MiB), %u in system memory (%llu MiB)",
                          Events(), (unsigned long long)(Bytes() >> 20), SystemEvents(),
                          (unsigned long long)(SystemBytes() >> 20));
        return written > 0 ? size_t(written) : 0;
    }

private:
    std::atomic<u32> events{};
    std::atomic<u32> system_events{};
    std::atomic<u64> bytes{};
    std::atomic<u64> system_bytes{};
};

inline AllocationTelemetry& Telemetry() noexcept {
    static AllocationTelemetry telemetry;
    return telemetry;
}

/// Records a fallback that really happened: the allocation succeeded with another candidate than the first.
/// `device_local(candidate)` says whether that candidate is device memory.
template <typename DeviceLocal>
bool RecordFallback(AllocationTelemetry& telemetry, AllocationCategory category, u64 size, u32 original,
                    const FallbackAllocation& allocation, DeviceLocal&& device_local) {
    if (allocation.result != vk::Result::eSuccess || allocation.candidate == original) {
        return false;
    }
    return telemetry.Note({size, category, original, allocation.candidate, device_local(allocation.candidate)});
}

} // namespace VideoCore
