// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: capability-driven renderer routes, kept free of Vulkan headers so they can be tested headless
// (tests/gpu_compat_tests.cpp). Follows Dragon's Crown Pro's vk_gpu_compat.h.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan::GpuCompat {

struct Capabilities {
    std::uint32_t vendor_id{};
    bool nvidia_proprietary{};
    bool amd_explicit_vertex_parameter{};
    bool fragment_shader_barycentric{};
    bool mixed_depth_samples{};
    bool provoking_vertex{};
};

enum class Interpolation { AmdExplicit, NativeBarycentric, Fallback };

struct InterpolationRoute {
    Interpolation interpolation{};
    bool manual_interpolation{};
    bool integer_fix{};
};

/// AMD explicit vertex parameter first (Steam Deck / RADV, AMD Windows), then native barycentrics,
/// else the driver's own interpolation. NVIDIA proprietary with barycentrics interpolates manually.
constexpr InterpolationRoute ChooseInterpolation(const Capabilities& caps) {
    InterpolationRoute route{};
    if (caps.amd_explicit_vertex_parameter) {
        route.interpolation = Interpolation::AmdExplicit;
    } else if (caps.fragment_shader_barycentric) {
        route.interpolation = Interpolation::NativeBarycentric;
    } else {
        route.interpolation = Interpolation::Fallback;
    }
    route.manual_interpolation =
        route.interpolation == Interpolation::NativeBarycentric && caps.nvidia_proprietary;
    // Only GCN-style interpolation (AMD) and the manual FMA path reproduce equal vertex values
    // exactly; Vulkan leaves driver interpolation precision implementation-defined.
    route.integer_fix =
        route.interpolation != Interpolation::AmdExplicit && !route.manual_interpolation;
    return route;
}

/// VkAttachmentSampleCountInfoAMD is only chained when some attachment really differs from the
/// rasterization sample count (0 counts as 1x). Uniform pipelines must not carry it: Steam's
/// Fossilize layer on SteamOS (RADV) rejects the struct.
constexpr bool NeedsMixedSamplesInfo(bool supported, std::uint32_t rasterization_samples,
                                     const std::uint32_t* color_samples,
                                     std::uint32_t color_attachment_count, bool has_depth_stencil,
                                     std::uint32_t depth_samples) {
    if (!supported) {
        return false;
    }
    const std::uint32_t raster = rasterization_samples ? rasterization_samples : 1u;
    for (std::uint32_t i = 0; i < color_attachment_count; ++i) {
        const std::uint32_t samples = color_samples[i] ? color_samples[i] : 1u;
        if (samples != raster) {
            return true;
        }
    }
    if (has_depth_stencil) {
        const std::uint32_t depth = depth_samples ? depth_samples : 1u;
        if (depth != raster) {
            return true;
        }
    }
    return false;
}

constexpr const char* InterpolationName(Interpolation interpolation) {
    switch (interpolation) {
    case Interpolation::AmdExplicit:
        return "amd_explicit";
    case Interpolation::NativeBarycentric:
        return "native_barycentric";
    case Interpolation::Fallback:
        return "fallback";
    }
    return "unknown";
}

inline std::string Summary(const Capabilities& caps) {
    const InterpolationRoute route = ChooseInterpolation(caps);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "GPU: compatibility vendor=%#06x interpolation=%s manual_interpolation=%s "
                  "integer_fix=%s mixed_samples=%s provoking_vertex=%s",
                  static_cast<unsigned>(caps.vendor_id), InterpolationName(route.interpolation),
                  route.manual_interpolation ? "on" : "off", route.integer_fix ? "on" : "off",
                  caps.mixed_depth_samples ? "yes" : "no", caps.provoking_vertex ? "yes" : "no");
    return line;
}

/// VK_KHR_pipeline_binary is used only with VK_KHR_maintenance5 and both features reported.
constexpr bool PipelineBinariesUsable(bool maintenance5_extension, bool maintenance5_feature,
                                      bool pipeline_binaries_feature) {
    return maintenance5_extension && maintenance5_feature && pipeline_binaries_feature;
}

/// The buffer cache binds its arenas with sparse residency: the device needs sparseBinding and
/// sparseResidencyBuffer, and the queue it submits on needs VK_QUEUE_SPARSE_BINDING_BIT.
constexpr bool SparseBuffersSupported(bool sparse_binding, bool sparse_residency_buffer,
                                      bool queue_sparse_binding) {
    return sparse_binding && sparse_residency_buffer && queue_sparse_binding;
}

/// What is missing for sparse buffers, "" when nothing is.
inline std::string MissingSparseRequirements(bool sparse_binding, bool sparse_residency_buffer,
                                             bool queue_sparse_binding) {
    std::string missing;
    const auto add = [&](const char* what) {
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += what;
    };
    if (!sparse_binding) {
        add("feature sparseBinding");
    }
    if (!sparse_residency_buffer) {
        add("feature sparseResidencyBuffer");
    }
    if (!queue_sparse_binding) {
        add("VK_QUEUE_SPARSE_BINDING_BIT on the graphics queue");
    }
    return missing;
}

/// A heap's label in memory diagnostics. On a UMA device (Steam Deck) the device-local heap is a
/// carve-out of system RAM and often host-visible; "system" is a heap without DEVICE_LOCAL.
constexpr const char* HeapKindName(bool device_local, bool has_host_visible_type) {
    if (!device_local) {
        return "system";
    }
    return has_host_visible_type ? "device-local, host-visible" : "device-local";
}

/// Remembers which heaps are over their budget. Note() is true once per heap, the first time it
/// crosses its budget: a heap hovering at the budget does not flood the log. A heap counts as back
/// within budget only below 90% of it.
class HeapBudgetTracker {
public:
    static constexpr std::uint32_t MaxHeaps = 16; // VK_MAX_MEMORY_HEAPS

    bool Note(std::uint32_t heap, std::uint64_t used, std::uint64_t budget) noexcept {
        if (heap >= MaxHeaps || budget == 0) {
            return false;
        }
        const bool was_over = over[heap].load(std::memory_order_relaxed);
        const bool now_over = was_over ? used > budget - budget / 10 : used > budget;
        if (now_over == was_over) {
            return false;
        }
        over[heap].store(now_over, std::memory_order_relaxed);
        return now_over && !reported[heap].exchange(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsOver(std::uint32_t heap) const noexcept {
        return heap < MaxHeaps && over[heap].load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> over[MaxHeaps]{};
    std::atomic<bool> reported[MaxHeaps]{};
};

} // namespace Vulkan::GpuCompat
