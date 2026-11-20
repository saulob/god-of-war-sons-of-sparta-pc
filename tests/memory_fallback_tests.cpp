// Headless proof of the device-memory fallback policy on the production header: which memory types an allocation may
// try and in what order, that only a full device memory moves on to the next type, that no type outside memoryTypeBits
// or without the required properties is ever used, and that telemetry exists only for a fallback that really happened.
// No window, GPU, launcher or game.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "video_core/allocation_fallback.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace fs = std::filesystem;
using Flag = vk::MemoryPropertyFlagBits;
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

// A discrete card as drivers report it: device-local VRAM, system memory, and the small host-visible VRAM window.
vk::PhysicalDeviceMemoryProperties DiscreteCard() {
    vk::PhysicalDeviceMemoryProperties props{};
    props.memoryTypeCount = 8;
    props.memoryTypes[0] = vk::MemoryType{{}, 1};
    props.memoryTypes[1] = vk::MemoryType{Flag::eDeviceLocal, 0};
    props.memoryTypes[2] = vk::MemoryType{Flag::eHostVisible | Flag::eHostCoherent, 1};
    props.memoryTypes[3] = vk::MemoryType{Flag::eHostVisible | Flag::eHostCoherent | Flag::eHostCached, 1};
    props.memoryTypes[4] = vk::MemoryType{Flag::eDeviceLocal | Flag::eHostVisible | Flag::eHostCoherent, 0};
    props.memoryTypes[5] = vk::MemoryType{Flag::eDeviceLocal | Flag::eLazilyAllocated, 0};
    props.memoryTypes[6] = vk::MemoryType{Flag::eDeviceLocal | Flag::eProtected, 0};
    props.memoryTypes[7] = vk::MemoryType{Flag::eDeviceLocal | Flag::eDeviceCoherentAMD, 0};
    return props;
}

bool IsDeviceLocal(const vk::PhysicalDeviceMemoryProperties& props, u32 type) {
    return bool(props.memoryTypes[type].propertyFlags & Flag::eDeviceLocal);
}

struct Attempts {
    std::vector<u32> tried;
};

void TestCandidateOrder() {
    const auto props = DiscreteCard();
    const auto types = FallbackMemoryTypes(props, 0xFF, 1);
    Check((types == std::vector<u32>{1, 4, 0, 2, 3}), "preferred first, other device-local types next, system memory last");
    Check(FallbackMemoryTypes(props, 0xFF, 1) == types, "the candidate order is deterministic");
    Check((FallbackMemoryTypes(props, 0b00010, 1) == std::vector<u32>{1}), "a buffer that accepts one type gets one candidate");
    Check((FallbackMemoryTypes(props, 0b10111, 1) == std::vector<u32>{1, 4, 0, 2}),
          "types whose memoryTypeBits bit is clear are skipped");
    Check((FallbackMemoryTypes(props, 0b11110, 1) == std::vector<u32>{1, 4, 2, 3}), "type 0 is skipped when its bit is clear");
    for (const u32 type : FallbackMemoryTypes(props, 0xFF, 1)) {
        const auto flags = props.memoryTypes[type].propertyFlags;
        Check(!(flags & (Flag::eProtected | Flag::eLazilyAllocated | Flag::eDeviceCoherentAMD)),
              "protected, lazily allocated and device-coherent types never stand in for another type");
    }
    Check((FallbackMemoryTypes(props, 0xFF, 7).front() != 7), "a preferred type that is never a fallback type is not listed");
}

void TestRequiredProperties() {
    const auto props = DiscreteCard();
    const auto host = FallbackMemoryTypes(props, 0xFF, 2, Flag::eHostVisible | Flag::eHostCoherent);
    Check((host == std::vector<u32>{2, 4, 3}), "required properties keep only types that have them");
    for (const u32 type : host) {
        Check((props.memoryTypes[type].propertyFlags & (Flag::eHostVisible | Flag::eHostCoherent)) ==
                  (Flag::eHostVisible | Flag::eHostCoherent),
              "no listed type lacks a required property");
    }
    Check(FallbackMemoryTypes(props, 0xFF, 1, Flag::eHostVisible).front() == 4,
          "a preferred type without a required property is replaced, not used");
    Check(FallbackMemoryTypes(props, 0xFF, 1, Flag::eHostVisible | Flag::eDeviceLocal | Flag::eHostCached).empty(),
          "no candidate when nothing has every required property");
    Check(FallbackMemoryTypes(props, 0xFF, 40).size() == 5, "a preferred index past the type count is ignored");
    Check(FallbackMemoryTypes(props, 0, 1).empty(), "no candidate when memoryTypeBits allows none");
}

void TestAllocationAlgorithm() {
    const auto props = DiscreteCard();
    const auto types = FallbackMemoryTypes(props, 0xFF, 1);
    Attempts attempts;

    const auto preferred = AllocateWithFallback(types, [&](u32 type) {
        attempts.tried.push_back(type);
        return vk::Result::eSuccess;
    });
    Check(preferred.result == vk::Result::eSuccess && preferred.candidate == 1 && preferred.attempts == 1 &&
              attempts.tried == std::vector<u32>{1},
          "the preferred type succeeds with one attempt");

    attempts.tried.clear();
    const auto second_local = AllocateWithFallback(types, [&](u32 type) {
        attempts.tried.push_back(type);
        return type == 1 ? vk::Result::eErrorOutOfDeviceMemory : vk::Result::eSuccess;
    });
    Check(second_local.result == vk::Result::eSuccess && second_local.candidate == 4 && second_local.attempts == 2,
          "a full preferred type moves to the second device-local type");

    attempts.tried.clear();
    const auto system = AllocateWithFallback(types, [&](u32 type) {
        attempts.tried.push_back(type);
        return IsDeviceLocal(props, type) ? vk::Result::eErrorOutOfDeviceMemory : vk::Result::eSuccess;
    });
    Check(system.result == vk::Result::eSuccess && system.candidate == 0 && system.attempts == 3 &&
              attempts.tried == (std::vector<u32>{1, 4, 0}),
          "every device-local type full, then system memory takes it");

    attempts.tried.clear();
    const auto lost = AllocateWithFallback(types, [&](u32 type) {
        attempts.tried.push_back(type);
        return vk::Result::eErrorDeviceLost;
    });
    Check(lost.result == vk::Result::eErrorDeviceLost && lost.attempts == 1 && attempts.tried == std::vector<u32>{1},
          "a device loss on the preferred type is the final result and no fallback is tried");

    attempts.tried.clear();
    const auto host = AllocateWithFallback(types, [&](u32 type) {
        attempts.tried.push_back(type);
        return vk::Result::eErrorOutOfHostMemory;
    });
    Check(host.result == vk::Result::eErrorOutOfHostMemory && host.attempts == 1,
          "out of host memory is not the device-memory fallback case");

    for (const vk::Result other : {vk::Result::eErrorMemoryMapFailed, vk::Result::eErrorTooManyObjects,
                                   vk::Result::eErrorInitializationFailed, vk::Result::eErrorUnknown,
                                   vk::Result::eErrorFragmentedPool}) {
        const auto unrelated = AllocateWithFallback(types, [&](u32) { return other; });
        Check(unrelated.result == other && unrelated.attempts == 1, "an unrelated error keeps its own result");
    }

    attempts.tried.clear();
    const auto all_full = AllocateWithFallback(types, [&](u32 type) {
        attempts.tried.push_back(type);
        return vk::Result::eErrorOutOfDeviceMemory;
    });
    Check(all_full.result == vk::Result::eErrorOutOfDeviceMemory && all_full.attempts == types.size() &&
              attempts.tried == types,
          "every candidate full returns the final out of device memory after trying each once");

    const auto none = AllocateWithFallback(std::vector<u32>{}, [&](u32) { return vk::Result::eSuccess; });
    Check(none.result == vk::Result::eErrorOutOfDeviceMemory && none.attempts == 0, "no candidates is an out of device memory");

    attempts.tried.clear();
    const auto restricted = FallbackMemoryTypes(props, 0b00110, 1);
    AllocateWithFallback(restricted, [&](u32 type) {
        attempts.tried.push_back(type);
        return vk::Result::eErrorOutOfDeviceMemory;
    });
    bool only_allowed = true;
    for (const u32 type : attempts.tried) {
        only_allowed &= ((0b00110u >> type) & 1) != 0;
    }
    Check(only_allowed && !attempts.tried.empty(), "no attempt ever uses a type whose memoryTypeBits bit is clear");
}

void TestTelemetry() {
    const auto props = DiscreteCard();
    const auto device_local = [&](u32 type) { return IsDeviceLocal(props, type); };
    AllocationTelemetry telemetry;
    const auto types = FallbackMemoryTypes(props, 0xFF, 1);

    const auto preferred = AllocateWithFallback(types, [](u32) { return vk::Result::eSuccess; });
    Check(!RecordFallback(telemetry, AllocationCategory::SparseBacking, 64 << 20, 1, preferred, device_local) &&
              telemetry.Events() == 0,
          "no telemetry when the preferred type succeeds");

    const auto all_full = AllocateWithFallback(types, [](u32) { return vk::Result::eErrorOutOfDeviceMemory; });
    Check(!RecordFallback(telemetry, AllocationCategory::SparseBacking, 64 << 20, 1, all_full, device_local) &&
              telemetry.Events() == 0,
          "no telemetry when every candidate failed: nothing was placed");

    const auto lost = AllocateWithFallback(types, [](u32) { return vk::Result::eErrorDeviceLost; });
    Check(!RecordFallback(telemetry, AllocationCategory::SparseBacking, 64 << 20, 1, lost, device_local) &&
              telemetry.Events() == 0,
          "no telemetry for a device loss");

    const auto second_local = AllocateWithFallback(
        types, [](u32 type) { return type == 1 ? vk::Result::eErrorOutOfDeviceMemory : vk::Result::eSuccess; });
    Check(RecordFallback(telemetry, AllocationCategory::SparseBacking, 64 << 20, 1, second_local, device_local) &&
              telemetry.Events() == 1 && telemetry.SystemEvents() == 0 && telemetry.Bytes() == (64u << 20),
          "a fallback to another device-local type is recorded as device memory");

    const auto system = AllocateWithFallback(types, [&](u32 type) {
        return IsDeviceLocal(props, type) ? vk::Result::eErrorOutOfDeviceMemory : vk::Result::eSuccess;
    });
    Check(RecordFallback(telemetry, AllocationCategory::SparseBacking, 256 << 20, 1, system, device_local) &&
              telemetry.Events() == 2 && telemetry.SystemEvents() == 1 && telemetry.SystemBytes() == (256u << 20),
          "a fallback to system memory is recorded with its size");

    char text[160];
    telemetry.Describe(text, sizeof(text));
    Check(Contains(text, "2 allocations") && Contains(text, "1 in system memory") && Contains(text, "256 MiB"),
          "the summary says that the preferred memory was exhausted and system memory was used");
    AllocationTelemetry quiet;
    quiet.Describe(text, sizeof(text));
    Check(text[0] == '\0', "no summary text when nothing fell back");

    AllocationTelemetry chatty;
    int logged = 0;
    for (int i = 0; i < 1000; ++i) {
        logged += chatty.Note({1 << 20, AllocationCategory::FrameImage, 0, 2, false}) ? 1 : 0;
    }
    Check(logged == int(AllocationTelemetry::MaxLoggedEvents) && chatty.Events() == 1000,
          "a game that stays short of memory logs a few events and counts the rest");
}

void TestProductionWiring() {
    const std::string cache = Source("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    Check(Contains(cache, "AllocateWithFallback(backing_memory_types") &&
              Contains(cache, "FallbackMemoryTypes(instance.GetMemoryProperties(), reqs.memoryTypeBits"),
          "the sparse arena backing allocates through the fallback over the buffer's own memoryTypeBits");
    Check(Contains(cache, "MemoryAllocateFlagBits::eDeviceAddress") && Contains(cache, "alloc_info.memoryTypeIndex = type;"),
          "the device-address allocate flag stays in every attempt and only the memory type changes");
    Check(Contains(cache, "Vulkan::Check(allocation.result)"), "a final out of device memory still ends through the checked path");

    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Contains(presenter, "VideoCore::AllocateWithFallback(stages") && Contains(presenter, "AllocationCategory::FrameImage"),
          "the frame image falls back over budget and then to system memory");

    const std::string image = Source("gpu/shadps4/video_core/texture_cache/image.cpp");
    const std::string buffer = Source("gpu/shadps4/video_core/buffer_cache/buffer.cpp");
    Check(Contains(image, "VMA_MEMORY_USAGE_AUTO_PREFER_HOST") && Contains(image, "VK_ERROR_OUT_OF_DEVICE_MEMORY"),
          "images already fall back over budget and to system memory on a full device memory only");
    Check(Contains(buffer, "== VK_ERROR_OUT_OF_DEVICE_MEMORY") && Contains(buffer, "~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT"),
          "VMA buffers already retry over budget on a full device memory only");

    const std::string header = Source("gpu/shadps4/video_core/allocation_fallback.h");
    Check(Contains(header, "!= vk::Result::eErrorOutOfDeviceMemory") && !Contains(header, "eErrorOutOfHostMemory") &&
              !Contains(header, "eErrorDeviceLost"),
          "the policy names only out of device memory as the fallback trigger");
}

} // namespace

int main() {
    TestCandidateOrder();
    TestRequiredProperties();
    TestAllocationAlgorithm();
    TestTelemetry();
    TestProductionWiring();
    std::printf("memory fallback tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
