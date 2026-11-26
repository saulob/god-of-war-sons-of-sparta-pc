# Headless proof of the Vulkan GPU compatibility routes on the production headers: VkAttachmentSampleCountInfoAMD only on
# genuinely mixed-sample pipelines (Steam Fossilize on SteamOS), the interpolation route and integer fix per driver
# (Steam Deck and NVIDIA unchanged, Intel/NVK get the fix), the integer nudge, the sparse buffer requirement, heap labels
# and the once-per-heap budget warning, plus the wiring (provokingVertexLast queried, surface entry points checked,
# SetObjectName and debug labels guarded, shader binary version bumped) and mutated copies that must fail.
# No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$vk = 'gpu/shadps4/video_core/renderer_vulkan'
$compat = "$vk/vk_gpu_compat.h"
$pipeline = "$vk/vk_graphics_pipeline.cpp"
$instance = "$vk/vk_instance.cpp"
$instanceH = "$vk/vk_instance.h"
$cache = "$vk/vk_pipeline_cache.cpp"
$serialization = "$vk/vk_pipeline_serialization.cpp"
$platform = "$vk/vk_platform.cpp"
$platformH = "$vk/vk_platform.h"
$rasterizer = "$vk/vk_rasterizer.cpp"
$fsr = "$vk/host_passes/fsr_pass.cpp"
$pp = "$vk/host_passes/pp_pass.cpp"
$smaa = "$vk/host_passes/smaa_pass.cpp"
$nudge = 'gpu/shadps4/shader_recompiler/ir/passes/interpolated_integer.h'
$pass = 'gpu/shadps4/shader_recompiler/ir/passes/interpolated_integer_pass.cpp'
$bufferCache = 'gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp'
$fallback = 'gpu/shadps4/video_core/allocation_fallback.h'

$mutations = @(
    @{ Name = 'the mixed samples struct is chained on support alone again'; File = $pipeline; Old = '.pNext = attach_mixed_samples ? &mixed_samples : nullptr,'; New = '.pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,' },
    @{ Name = 'NeedsMixedSamplesInfo attaches whenever supported'; File = $compat; Old = "    if (!supported) {`n        return false;`n    }"; New = "    if (supported) {`n        return true;`n    }" },
    @{ Name = 'a 0 color sample count is not read as 1x'; File = $compat; Old = 'const std::uint32_t samples = color_samples[i] ? color_samples[i] : 1u;'; New = 'const std::uint32_t samples = color_samples[i];' },
    @{ Name = 'the depth count is ignored'; File = $compat; Old = 'if (depth != raster) {'; New = 'if (false) {' },
    @{ Name = 'provokingVertexLast is not checked'; File = $instance; Old = "    provoking_vertex =`n        feature_chain.get<vk::PhysicalDeviceProvokingVertexFeaturesEXT>().provokingVertexLast &&`n        add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);"; New = '    provoking_vertex = add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);' },
    @{ Name = 'the provoking vertex features are not queried'; File = $instance; Old = "        vk::PhysicalDeviceProvokingVertexFeaturesEXT,`n        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,"; New = '        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,' },
    @{ Name = 'the Xlib surface null check is removed'; File = $platform; Old = 'if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateXlibSurfaceKHR) {'; New = 'if (false) {' },
    @{ Name = 'the Wayland surface null check is removed'; File = $platform; Old = 'if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateWaylandSurfaceKHR) {'; New = 'if (false) {' },
    @{ Name = 'the Win32 surface null check is removed'; File = $platform; Old = 'if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateWin32SurfaceKHR) {'; New = 'if (false) {' },
    @{ Name = 'SetObjectName calls a null entry point'; File = $platformH; Old = "    // dcp: without VK_EXT_debug_utils the entry point is null; naming is optional.`n    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkSetDebugUtilsObjectNameEXT == nullptr) {`n        return;`n    }"; New = '' },
    @{ Name = 'a rasterizer debug label ignores DebugLabelsAvailable'; File = $rasterizer; Old = '|| !DebugLabelsAvailable()) {'; New = ') {' },
    @{ Name = 'an SMAA debug label ignores DebugLabelsAvailable'; File = $smaa; Old = 'IsVkHostMarkersEnabled() && Vulkan::DebugLabelsAvailable()) {'; New = 'IsVkHostMarkersEnabled()) {' },
    @{ Name = 'the integer fix reverts to the NVIDIA-only rule'; File = $compat; Old = "    route.integer_fix =`n        route.interpolation != Interpolation::AmdExplicit && !route.manual_interpolation;"; New = "    route.integer_fix = !caps.fragment_shader_barycentric && caps.nvidia_proprietary;" },
    @{ Name = 'the pipeline cache reverts to the NVIDIA-only rule'; File = $cache; Old = '.needs_integer_interpolation_fix = interpolation.integer_fix,'; New = ".needs_integer_interpolation_fix = !instance.IsFragmentShaderBarycentricSupported() &&`n                                           instance.GetDriverID() == vk::DriverId::eNvidiaProprietary," },
    @{ Name = 'barycentrics win over AMD explicit (Steam Deck changes path)'; File = $compat; Old = "    if (caps.amd_explicit_vertex_parameter) {`n        route.interpolation = Interpolation::AmdExplicit;`n    } else if (caps.fragment_shader_barycentric) {`n        route.interpolation = Interpolation::NativeBarycentric;"; New = "    if (caps.fragment_shader_barycentric) {`n        route.interpolation = Interpolation::NativeBarycentric;`n    } else if (caps.amd_explicit_vertex_parameter) {`n        route.interpolation = Interpolation::AmdExplicit;" },
    @{ Name = 'manual interpolation is not limited to NVIDIA proprietary'; File = $compat; Old = 'route.interpolation == Interpolation::NativeBarycentric && caps.nvidia_proprietary;'; New = 'route.interpolation == Interpolation::NativeBarycentric;' },
    @{ Name = 'flat inputs are nudged'; File = $pass; Old = 'return primary != Qualifier::Flat && primary != Qualifier::PerVertex;'; New = 'return true;' },
    @{ Name = 'the nudge scale is lost'; File = $nudge; Old = 'constexpr float InterpolatedIntegerScale = 1.0f + 1.0f / 65536.0f;'; New = 'constexpr float InterpolatedIntegerScale = 1.0f;' },
    @{ Name = 'ShaderBinaryVersion is not bumped'; File = $serialization; Old = 'static constexpr u32 ShaderBinaryVersion = 9u;'; New = 'static constexpr u32 ShaderBinaryVersion = 8u;' },
    @{ Name = 'the sparse check ignores the queue'; File = $compat; Old = 'return sparse_binding && sparse_residency_buffer && queue_sparse_binding;'; New = 'return sparse_binding && sparse_residency_buffer;' },
    @{ Name = 'the buffer cache ignores missing sparse support'; File = $bufferCache; Old = 'ArenaLayout::SelectArenaModeFromEnvironment(instance.IsSparseBufferBindingSupported())'; New = 'ArenaLayout::SelectArenaModeFromEnvironment(true)' },
    @{ Name = 'the queue sparse flag is not recorded'; File = $instance; Old = "queue_sparse_binding = static_cast<bool>(family_properties[queue_family_index].queueFlags &`n                                             vk::QueueFlagBits::eSparseBinding);"; New = 'queue_sparse_binding = true;' },
    @{ Name = 'every budget crossing warns'; File = $compat; Old = 'return now_over && !reported[heap].exchange(true, std::memory_order_relaxed);'; New = 'return now_over;' },
    @{ Name = 'the budget hysteresis is lost'; File = $compat; Old = 'const bool now_over = was_over ? used > budget - budget / 10 : used > budget;'; New = 'const bool now_over = used > budget;' },
    @{ Name = 'a UMA heap is labelled plain device-local'; File = $compat; Old = 'return has_host_visible_type ? "device-local, host-visible" : "device-local";'; New = 'return "device-local";' }
)
$code = Invoke-HostMutationTest -Name 'gpu_compat' -Root $root -Test 'tests/gpu_compat_tests.cpp' `
    -Copy @($compat, $pipeline, $instance, $instanceH, $cache, $serialization, $platform, $platformH, $rasterizer, $fsr, $pp, $smaa,
        $nudge, $pass, $bufferCache, $fallback) -Mutations $mutations
exit $code
