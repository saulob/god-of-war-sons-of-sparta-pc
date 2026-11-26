// Vulkan GPU compatibility routes on the production headers (vk_gpu_compat.h, interpolated_integer.h): when
// VkAttachmentSampleCountInfoAMD is chained (Steam Fossilize on SteamOS rejects it on uniform pipelines), which
// interpolation path a driver takes and whether the integer fix is on, the integer nudge, the sparse buffer requirement,
// heap labels and the once-per-heap budget warning; plus source contracts of their wiring (provokingVertexLast queried,
// surface entry points checked before use, SetObjectName and debug labels guarded, shader binary version bumped).
// No window, GPU, launcher or game.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "shader_recompiler/ir/passes/interpolated_integer.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

using namespace Vulkan::GpuCompat;
using Shader::Optimization::NudgeInterpolatedInteger;

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
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    std::string unix_text;
    for (const char c : text) {
        if (c != '\r') {
            unix_text += c;
        }
    }
    Check(!unix_text.empty(), std::string("source readable: ") + path);
    return unix_text;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int Count(const std::string& text, const std::string& part) {
    int count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// `first` occurs, and before `second`.
bool Before(const std::string& text, const std::string& first, const std::string& second) {
    const size_t a = text.find(first);
    const size_t b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

constexpr std::uint32_t AmdVendor = 0x1002;
constexpr std::uint32_t NvidiaVendor = 0x10de;
constexpr std::uint32_t IntelVendor = 0x8086;

Capabilities SteamDeckRadv() {
    return {.vendor_id = AmdVendor, .amd_explicit_vertex_parameter = true, .mixed_depth_samples = true,
            .provoking_vertex = true};
}
Capabilities NvidiaTuring() {
    return {.vendor_id = NvidiaVendor, .nvidia_proprietary = true, .fragment_shader_barycentric = true,
            .provoking_vertex = true};
}
Capabilities NvidiaPascal() {
    return {.vendor_id = NvidiaVendor, .nvidia_proprietary = true, .provoking_vertex = true};
}
Capabilities IntelAnv() {
    return {.vendor_id = IntelVendor, .fragment_shader_barycentric = true, .provoking_vertex = true};
}
Capabilities IntelNoBarycentric() {
    return {.vendor_id = IntelVendor};
}
Capabilities Nvk() {
    return {.vendor_id = NvidiaVendor, .provoking_vertex = true};
}
// SOS-BXG9UT, Intel Arc 140T / Graphics ARL, Mesa ANV 26.2.7 on Xe KMD: no VK_AMD_shader_explicit_vertex_parameter, no
// VK_KHR_fragment_shader_barycentric, no VK_AMD_mixed_attachment_samples / VK_NV_framebuffer_mixed_samples; with
// VK_EXT_provoking_vertex (and VK_KHR_pipeline_binary, which the routes do not depend on).
Capabilities IntelArl() {
    return {.vendor_id = IntelVendor, .nvidia_proprietary = false, .amd_explicit_vertex_parameter = false,
            .fragment_shader_barycentric = false, .mixed_depth_samples = false, .provoking_vertex = true};
}

void TestIntelArlProfile() {
    const Capabilities arl = IntelArl();
    const InterpolationRoute route = ChooseInterpolation(arl);
    Check(route.interpolation == Interpolation::Fallback, "SOS-BXG9UT ARL: driver interpolation (fallback)");
    Check(!route.manual_interpolation, "SOS-BXG9UT ARL: no manual interpolation (it would need barycentrics)");
    Check(route.integer_fix, "SOS-BXG9UT ARL: interpolated-integer fix on");
    const std::uint32_t mixed[2] = {4, 1};
    Check(!NeedsMixedSamplesInfo(arl.mixed_depth_samples, 4, mixed, 2, true, 1),
          "SOS-BXG9UT ARL: no VkAttachmentSampleCountInfoAMD without mixed-sample support");
    Check(Summary(arl) == "GPU: compatibility vendor=0x8086 interpolation=fallback manual_interpolation=off "
                          "integer_fix=on mixed_samples=no provoking_vertex=yes",
          "SOS-BXG9UT ARL: the log's compatibility line: " + Summary(arl));
    // The profile wiring: the shader profile takes the route's flags, never the raw extension list.
    const std::string cache = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    Check(Contains(cache, ".needs_manual_interpolation = interpolation.manual_interpolation,") &&
              Contains(cache, ".needs_integer_interpolation_fix = interpolation.integer_fix,") &&
              Contains(cache, ".supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),"),
          "the shader profile follows the interpolation route");
    // Barycentrics are only asked for when AMD explicit is absent, and their feature struct is unlinked without them.
    const std::string instance = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    Check(Contains(instance, "    if (!fragment_shader_barycentric) {\n        device_chain.unlink<vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR>();"),
          "no barycentric feature struct in pNext when the extension is absent");
    Check(Contains(instance, "    if (!pipeline_binaries) {\n        device_chain.unlink<vk::PhysicalDevicePipelineBinaryFeaturesKHR>();") &&
              Contains(instance, "feature_chain.get<vk::PhysicalDevicePipelineBinaryFeaturesKHR>().pipelineBinaries &&"),
          "VK_KHR_pipeline_binary only with its feature, its struct unlinked otherwise");
}

void TestMixedSamples() {
    const std::uint32_t one[2] = {1, 1};
    const std::uint32_t zero[2] = {0, 0};
    const std::uint32_t four[2] = {4, 4};
    const std::uint32_t mixed_colors[2] = {4, 1};

    Check(!NeedsMixedSamplesInfo(true, 1, one, 2, true, 1), "uniform 1x color and depth: no struct");
    Check(!NeedsMixedSamplesInfo(true, 0, zero, 2, true, 0), "0 counts as 1x everywhere: no struct");
    Check(!NeedsMixedSamplesInfo(true, 1, zero, 2, false, 0), "0x colors against 1x raster: no struct");
    Check(!NeedsMixedSamplesInfo(true, 4, four, 2, true, 4), "uniform 4x: no struct");
    Check(!NeedsMixedSamplesInfo(true, 1, one, 0, true, 1), "depth-only 1x: no struct");
    Check(NeedsMixedSamplesInfo(true, 4, one, 0, true, 1), "depth-only, depth differs from raster: struct");
    Check(NeedsMixedSamplesInfo(true, 4, four, 2, true, 1), "4x colors with 1x depth: struct");
    Check(NeedsMixedSamplesInfo(true, 1, one, 1, true, 4), "1x color with 4x depth: struct");
    Check(NeedsMixedSamplesInfo(true, 4, mixed_colors, 2, false, 0), "mixed color counts: struct");
    Check(!NeedsMixedSamplesInfo(true, 4, four, 2, false, 1), "no depth attachment: its count is ignored");
    Check(!NeedsMixedSamplesInfo(true, 4, mixed_colors, 1, false, 0), "only the bound color attachments count");
    for (const std::uint32_t* colors : {one, four, mixed_colors}) {
        Check(!NeedsMixedSamplesInfo(false, 4, colors, 2, true, 1), "unsupported: never a struct");
    }
}

// Dragon's Crown Pro DCP-TKXB2V: SteamOS (RADV, Rembrandt) gameplay pipelines are uniform; attaching
// VkAttachmentSampleCountInfoAMD (sType 1000044008) made Steam Fossilize refuse to record them.
void TestTkxb2vRegression() {
    const Capabilities radv = SteamDeckRadv();
    Check(radv.mixed_depth_samples, "RADV reports mixed attachment samples");
    const std::uint32_t uniform_1x[2] = {1, 1};
    const std::uint32_t uniform_4x[1] = {4};
    const std::uint32_t color_1x[1] = {1};
    Check(!NeedsMixedSamplesInfo(radv.mixed_depth_samples, 1, uniform_1x, 1, true, 1), "TKXB2V: 1x color + depth");
    Check(!NeedsMixedSamplesInfo(radv.mixed_depth_samples, 1, uniform_1x, 2, false, 1), "TKXB2V: 2 x 1x color");
    Check(!NeedsMixedSamplesInfo(radv.mixed_depth_samples, 4, uniform_4x, 1, true, 4), "TKXB2V: 4x color + depth");
    Check(NeedsMixedSamplesInfo(radv.mixed_depth_samples, 4, color_1x, 1, true, 4),
          "TKXB2V: a genuinely mixed pipeline keeps VK_AMD_mixed_attachment_samples");
}

void TestInterpolationRoutes() {
    const InterpolationRoute deck = ChooseInterpolation(SteamDeckRadv());
    Check(deck.interpolation == Interpolation::AmdExplicit, "Steam Deck: AMD explicit vertex parameter");
    Check(!deck.manual_interpolation && !deck.integer_fix, "Steam Deck: no manual path, no integer fix (unchanged)");

    Capabilities amd_with_bary = SteamDeckRadv();
    amd_with_bary.fragment_shader_barycentric = true;
    Check(ChooseInterpolation(amd_with_bary).interpolation == Interpolation::AmdExplicit,
          "AMD explicit wins over barycentrics");

    const InterpolationRoute turing = ChooseInterpolation(NvidiaTuring());
    Check(turing.interpolation == Interpolation::NativeBarycentric, "NVIDIA Turing: native barycentric");
    Check(turing.manual_interpolation && !turing.integer_fix, "NVIDIA Turing: manual interpolation, no fix (unchanged)");

    const InterpolationRoute pascal = ChooseInterpolation(NvidiaPascal());
    Check(pascal.interpolation == Interpolation::Fallback, "NVIDIA Pascal: fallback");
    Check(!pascal.manual_interpolation && pascal.integer_fix, "NVIDIA Pascal: integer fix (unchanged)");

    const InterpolationRoute anv = ChooseInterpolation(IntelAnv());
    Check(anv.interpolation == Interpolation::NativeBarycentric, "Intel with barycentrics: native barycentric");
    Check(!anv.manual_interpolation, "non-NVIDIA barycentrics: the driver interpolates");
    Check(anv.integer_fix, "non-NVIDIA barycentrics: integer fix on");

    const InterpolationRoute intel = ChooseInterpolation(IntelNoBarycentric());
    Check(intel.interpolation == Interpolation::Fallback && intel.integer_fix && !intel.manual_interpolation,
          "Intel without barycentrics: fallback with integer fix");

    const InterpolationRoute nvk = ChooseInterpolation(Nvk());
    Check(nvk.interpolation == Interpolation::Fallback && nvk.integer_fix && !nvk.manual_interpolation,
          "NVK (not proprietary, no barycentrics): fallback with integer fix");
}

void TestSummary() {
    const std::string deck = Summary(SteamDeckRadv());
    Check(deck == "GPU: compatibility vendor=0x1002 interpolation=amd_explicit manual_interpolation=off "
                  "integer_fix=off mixed_samples=yes provoking_vertex=yes",
          "summary line of the Steam Deck: " + deck);
    Check(Contains(Summary(NvidiaTuring()), "interpolation=native_barycentric manual_interpolation=on integer_fix=off"),
          "summary of NVIDIA Turing");
    Check(Contains(Summary(Nvk()), "interpolation=fallback manual_interpolation=off integer_fix=on"), "summary of NVK");
}

struct Vertex {
    float value;
    float w;
};

// GCN V_INTERP_P1/P2 and the manual path: P0 + I * (P1 - P0) + J * (P2 - P0).
float GcnInterpolate(const Vertex (&v)[3], float i, float j) {
    const float p1 = std::fma(v[1].value - v[0].value, i, v[0].value);
    return std::fma(v[2].value - v[0].value, j, p1);
}

// A driver that interpolates value / w linearly in screen space and divides by the interpolated 1 / w.
float PerspectiveInterpolate(const Vertex (&v)[3], float l1, float l2) {
    const float l0 = 1.0f - l1 - l2;
    const float num = l0 * (v[0].value / v[0].w) + l1 * (v[1].value / v[1].w) + l2 * (v[2].value / v[2].w);
    const float den = l0 / v[0].w + l1 / v[1].w + l2 / v[2].w;
    return num / den;
}

void TestIntegerNudge() {
    int driver_errors = 0;
    int nudged_errors = 0;
    int gcn_errors = 0;
    for (const float integer : {1.0f, 3.0f, 7.0f, 41.0f, 255.0f, 1023.0f}) {
        for (const float w1 : {0.3f, 1.7f, 13.0f}) {
            const Vertex v[3] = {{integer, 1.0f}, {integer, w1}, {integer, 37.0f}};
            for (int a = 0; a <= 64; ++a) {
                for (int b = 0; a + b <= 64; ++b) {
                    const float l1 = a / 64.0f;
                    const float l2 = b / 64.0f;
                    const float driver = PerspectiveInterpolate(v, l1, l2);
                    driver_errors += std::trunc(driver) != integer;
                    nudged_errors += std::trunc(NudgeInterpolatedInteger(driver)) != integer;
                    gcn_errors += std::trunc(GcnInterpolate(v, l1, l2)) != integer;
                }
            }
        }
    }
    Check(driver_errors > 0, "a driver interpolation truncates some equal integers wrongly");
    Check(nudged_errors == 0, "the nudge restores every integer");
    Check(gcn_errors == 0, "GCN-style interpolation is exact");
    for (const float n : {-5.0f, -1.0f}) {
        Check(std::trunc(NudgeInterpolatedInteger(std::nextafter(n, 0.0f))) == n, "negative values nudge away from zero");
    }
    for (const float x : {0.25f, 2.5f, 100.75f, -3.5f}) {
        Check(std::fabs(NudgeInterpolatedInteger(x) - x) <= std::fabs(x) * 2e-5f, "other values move by ~2^-16");
        Check(std::trunc(NudgeInterpolatedInteger(x)) == std::trunc(x), "other values keep their integer part");
    }
}

void TestSparseRequirement() {
    Check(SparseBuffersSupported(true, true, true), "sparse: everything present");
    Check(!SparseBuffersSupported(false, true, true), "sparse: no sparseBinding");
    Check(!SparseBuffersSupported(true, false, true), "sparse: no sparseResidencyBuffer");
    Check(!SparseBuffersSupported(true, true, false), "sparse: queue without SPARSE_BINDING");
    Check(MissingSparseRequirements(true, true, true).empty(), "sparse: nothing missing");
    Check(MissingSparseRequirements(false, true, true) == "feature sparseBinding", "sparse: names sparseBinding");
    Check(Contains(MissingSparseRequirements(true, false, false), "feature sparseResidencyBuffer") &&
              Contains(MissingSparseRequirements(true, false, false), "VK_QUEUE_SPARSE_BINDING_BIT"),
          "sparse: names every missing piece");
}

void TestHeapDiagnostics() {
    Check(std::string(HeapKindName(true, false)) == "device-local", "heap kind device-local");
    Check(std::string(HeapKindName(true, true)) == "device-local, host-visible", "heap kind UMA / ReBAR");
    Check(std::string(HeapKindName(false, true)) == "system", "heap kind system");

    HeapBudgetTracker tracker;
    const std::uint64_t budget = 1000;
    Check(!tracker.Note(0, 900, budget), "under budget: no warning");
    Check(tracker.Note(0, 1001, budget), "first crossing: one warning");
    Check(!tracker.Note(0, 1200, budget), "still over: no second warning");
    Check(!tracker.Note(0, 950, budget) && tracker.IsOver(0), "95% of budget: still counted as over");
    Check(!tracker.Note(0, 850, budget) && !tracker.IsOver(0), "below 90%: back within budget");
    Check(!tracker.Note(0, 1100, budget) && tracker.IsOver(0), "second crossing: no second warning");
    Check(tracker.Note(1, 2000, budget), "another heap warns on its own");
    Check(!tracker.Note(2, 2000, 0), "a heap without a budget never warns");
    Check(!tracker.Note(HeapBudgetTracker::MaxHeaps, 2000, budget), "an out-of-range heap never warns");
}

void TestSourceContracts() {
    const std::string pipeline = Source("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    Check(Contains(pipeline, "GpuCompat::NeedsMixedSamplesInfo("), "graphics pipeline asks NeedsMixedSamplesInfo");
    Check(Contains(pipeline, ".pNext = attach_mixed_samples ? &mixed_samples : nullptr,"),
          "the mixed samples struct is chained only when NeedsMixedSamplesInfo says so");
    Check(!Contains(pipeline, "IsMixedDepthSamplesSupported() ? &mixed_samples"),
          "the struct is not chained on support alone");
    Check(Contains(pipeline, "static_cast<u32>(sdata.multisampling.rasterizationSamples)"),
          "the rasterization sample count is the reference");

    const std::string instance = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    const size_t features = instance.find("physical_device.getFeatures2<");
    const size_t features_end = instance.find(">();", features);
    Check(features != std::string::npos && features_end != std::string::npos &&
              instance.substr(features, features_end - features).find("vk::PhysicalDeviceProvokingVertexFeaturesEXT") !=
                  std::string::npos,
          "the device feature query includes VkPhysicalDeviceProvokingVertexFeaturesEXT");
    Check(Contains(instance, "feature_chain.get<vk::PhysicalDeviceProvokingVertexFeaturesEXT>().provokingVertexLast &&\n"
                             "        add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);"),
          "VK_EXT_provoking_vertex is enabled only with provokingVertexLast");
    Check(Contains(instance, "if (!provoking_vertex) {\n        device_chain.unlink<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();"),
          "the provoking vertex feature is unlinked when not enabled");
    Check(Contains(instance, "queue_sparse_binding = static_cast<bool>(family_properties[queue_family_index].queueFlags &\n"
                             "                                             vk::QueueFlagBits::eSparseBinding);"),
          "the graphics queue's sparse binding support is recorded");
    Check(Contains(instance, "heap_budget_tracker.Note(heap, memory_budget_props.heapUsage[heap],") &&
              Contains(instance, "went over its budget"),
          "the budget query warns once per heap over budget");
    Check(Contains(instance, "MemoryHeapKind(props.memoryProperties, heap)"), "heap diagnostics name the heap kind");
    Check(Contains(instance, "driver.driverInfo.data()"), "the startup log names driverInfo (Mesa version)");
    Check(Contains(instance, "fmt::print(\"GPU: heap {} ({}) size {} MiB, flags {}, budget {} MiB"),
          "the startup log lists every heap with size, flags and budget");

    const std::string instance_h = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.h");
    Check(Contains(instance_h, "GpuCompat::Capabilities GetGpuCapabilities() const"), "Instance::GetGpuCapabilities");
    Check(Contains(instance_h, ".amd_explicit_vertex_parameter = amd_shader_explicit_vertex_parameter,") &&
              Contains(instance_h, ".fragment_shader_barycentric = fragment_shader_barycentric,") &&
              Contains(instance_h, ".nvidia_proprietary = driver_id == vk::DriverId::eNvidiaProprietary,"),
          "the capabilities come from the enabled extensions and the driver");
    Check(Contains(instance_h, "features.sparseResidencyBuffer,\n                                                 queue_sparse_binding);"),
          "the sparse check covers the queue");

    const std::string cache = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    Check(Contains(cache, "const auto interpolation = GpuCompat::ChooseInterpolation(gpu_caps);"),
          "the pipeline cache uses ChooseInterpolation");
    Check(Contains(cache, ".needs_manual_interpolation = interpolation.manual_interpolation,") &&
              Contains(cache, ".needs_integer_interpolation_fix = interpolation.integer_fix,"),
          "the shader profile follows the interpolation route");
    Check(!Contains(cache, "!instance.IsFragmentShaderBarycentricSupported() &&"), "the NVIDIA-only fix rule is gone");
    Check(Contains(cache, "summary_printed.test_and_set()") && Contains(cache, "GpuCompat::Summary(gpu_caps)"),
          "the capability summary is printed once");

    const std::string pass = Source("gpu/shadps4/shader_recompiler/ir/passes/interpolated_integer_pass.cpp");
    Check(Contains(pass, "return primary != Qualifier::Flat && primary != Qualifier::PerVertex;"),
          "flat and per-vertex inputs are not nudged");
    Check(Contains(pass, "info.fs_interpolation[index].primary"), "the pass reads the input's qualifier");
    Check(Contains(pass, "ir.Imm32(InterpolatedIntegerScale)"), "the pass uses the tested scale");
    Check(Contains(pass, "static std::atomic<u32> shaders{0};"), "the log counter is atomic");

    const std::string serialization = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const size_t version = serialization.find("static constexpr u32 ShaderBinaryVersion = ");
    Check(version != std::string::npos &&
              std::stoul(serialization.substr(version + std::string("static constexpr u32 ShaderBinaryVersion = ").size())) >= 9,
          "ShaderBinaryVersion bumped to 9 (shader output changed)");

    const std::string platform = Source("gpu/shadps4/video_core/renderer_vulkan/vk_platform.cpp");
    for (const char* name : {"Win32", "Xlib", "Wayland", "Metal"}) {
        const std::string suffix = std::string(name) == "Metal" ? "EXT" : "KHR";
        const std::string call = std::string("instance.create") + name + "Surface" + suffix + "(";
        const std::string check = std::string("if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCreate") + name + "Surface" + suffix + ") {";
        const size_t call_at = platform.find(call);
        const size_t check_at = call_at == std::string::npos ? std::string::npos : platform.rfind(check, call_at);
        const size_t branch_at = call_at == std::string::npos ? std::string::npos : platform.rfind("window_info.type ==", call_at);
        const size_t stop_at = check_at == std::string::npos ? std::string::npos : platform.find("UNREACHABLE();", check_at);
        Check(call_at != std::string::npos && check_at != std::string::npos && branch_at != std::string::npos &&
                  check_at > branch_at && stop_at < call_at,
              std::string("create") + name + "Surface is preceded by its null check, which stops");
    }
    Check(Contains(platform, "VK_KHR_XLIB_SURFACE_EXTENSION_NAME,\n                InstanceExtensionOffered(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)"),
          "a missing Xlib surface says when Wayland is available");
    Check(Contains(platform, "SDL_GetCurrentVideoDriver()"), "the surface error names the SDL video driver");
    Check(Contains(platform, "if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDebugUtilsMessengerEXT) {"),
          "the debug messenger is not created without VK_EXT_debug_utils");

    const std::string platform_h = Source("gpu/shadps4/video_core/renderer_vulkan/vk_platform.h");
    Check(Before(platform_h, "if (VULKAN_HPP_DEFAULT_DISPATCHER.vkSetDebugUtilsObjectNameEXT == nullptr) {\n        return;",
                 "device.setDebugUtilsObjectNameEXT(name_info)"),
          "SetObjectName returns before a null vkSetDebugUtilsObjectNameEXT");
    Check(Contains(platform_h, "inline bool DebugLabelsAvailable()"), "DebugLabelsAvailable helper");

    const std::string rasterizer = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    Check(Count(rasterizer, "|| !DebugLabelsAvailable()) {") == 4 &&
              Count(rasterizer, "DebugUtilsLabelEXT(") == 4,
          "every rasterizer debug label is behind DebugLabelsAvailable");
    for (const char* path : {"gpu/shadps4/video_core/renderer_vulkan/host_passes/fsr_pass.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/host_passes/smaa_pass.cpp"}) {
        const std::string text = Source(path);
        Check(Count(text, "IsVkHostMarkersEnabled() && Vulkan::DebugLabelsAvailable()) {") == 2 &&
                  Count(text, "DebugUtilsLabelEXT(") == 2,
              std::string("debug labels guarded in ") + path);
    }

    const std::string buffer_cache = Source("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    // Missing sparse support selects fully resident arenas (tests/buffer_arena_tests.cpp) instead of stopping.
    Check(Before(buffer_cache, "ArenaLayout::SelectArenaModeFromEnvironment(instance.IsSparseBufferBindingSupported())",
                 "getBufferMemoryRequirements(req_info)"),
          "the buffer cache picks its arena mode from the device's sparse support before probing an arena");
    Check(!Contains(buffer_cache, "cannot run the game: sparse buffers"), "missing sparse support no longer stops the game");
    Check(Contains(buffer_cache, "instance.MissingSparseBufferRequirements()"), "the fully resident startup line names what is missing");
    Check(Contains(buffer_cache, "BDA page \"\n                \"table %llu MiB"), "arena block and BDA page table logged");
    Check(Contains(buffer_cache, "MemoryTypeLabel(memory_types, allocation.candidate)"),
          "an arena fallback names the memory type it used");

    const std::string fallback = Source("gpu/shadps4/video_core/allocation_fallback.h");
    Check(Contains(fallback, "return Vulkan::GpuCompat::HeapKindName(device_local, host_visible);"),
          "MemoryHeapKind uses the tested labels");
}

} // namespace

int main() {
    TestMixedSamples();
    TestTkxb2vRegression();
    TestInterpolationRoutes();
    TestIntelArlProfile();
    TestSummary();
    TestIntegerNudge();
    TestSparseRequirement();
    TestHeapDiagnostics();
    TestSourceContracts();
    if (g_failures) {
        std::printf("gpu compat tests: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("gpu compat tests: %d checks passed\n", g_checks);
    return 0;
}
