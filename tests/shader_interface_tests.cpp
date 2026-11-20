// Headless proof of the interface between shader stages that the NVIDIA clip-distance emulation depends on, on the
// production rule functions and the production RuntimeInfo: the fragment shader reads the emulated clip-distance
// attribute exactly when the hardware vertex stage writes it, a pipeline without a pixel shader decides from its own
// registers and not from the previous draw, and the pipeline cache and the SPIR-V emitter use the rule. The same checks
// run against mutated copies of the rule from tools/test_nvidia_stability.ps1. No window, GPU, launcher or game.
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "shader_recompiler/runtime_info.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace fs = std::filesystem;
using namespace Shader;

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

struct Draw {
    bool pixel_shader;
    bool tessellation;
    bool geometry;
    bool clip_distances;
    bool user_clip_planes;
};

// Whether the stage that feeds the rasterizer writes the emulated attribute for a draw whose vertex stage stores clip
// distances: the vertex program runs on the hardware VS only without tessellation and geometry.
bool VertexStageWritesAttribute(const Draw& draw, bool needs) {
    return EmulatesClipDistances(HwStage::Vertex, HardwareVsProgram(draw.tessellation, draw.geometry), needs);
}

bool FragmentReadsAttribute(const Draw& draw, bool needs) {
    return FragmentClipDistanceEmulation(draw.tessellation, draw.geometry, draw.clip_distances, draw.user_clip_planes,
                                         needs);
}

void TestStageRule() {
    for (const bool needs : {false, true}) {
        for (u32 hw = 0; hw < u32(HwStage::Compute) + 1; ++hw) {
            for (u32 sw = 0; sw < u32(SwStage::NumLogicalStages); ++sw) {
                const bool expected = needs && HwStage(hw) == HwStage::Vertex && SwStage(sw) == SwStage::Vertex;
                Check(EmulatesClipDistances(HwStage(hw), SwStage(sw), needs) == expected,
                      "only the guest vertex shader on the hardware VS writes the emulated attribute, and only on NVIDIA");
            }
        }
    }
    Check(HardwareVsProgram(false, false) == SwStage::Vertex && HardwareVsProgram(true, false) == SwStage::TessellationEval &&
              HardwareVsProgram(false, true) == SwStage::Geometry && HardwareVsProgram(true, true) == SwStage::Geometry,
          "the hardware VS runs the vertex, domain or geometry program");
}

void TestFragmentAgreesWithTheVertexStage() {
    for (u32 bits = 0; bits < 32; ++bits) {
        const Draw draw{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0};
        for (const bool needs : {false, true}) {
            const bool vertex_stores = draw.clip_distances || draw.user_clip_planes;
            const bool written = vertex_stores && VertexStageWritesAttribute(draw, needs);
            Check(FragmentReadsAttribute(draw, needs) == written,
                  "the fragment shader reads the emulated attribute exactly when the vertex stage writes it");
        }
    }
}

void TestPipelineKinds() {
    const bool needs = true;
    Check(FragmentReadsAttribute({true, false, false, true, false}, needs), "a vertex-only pipeline with clip distances reads it");
    Check(FragmentReadsAttribute({true, false, false, false, true}, needs), "a vertex-only pipeline with user clip planes reads it");
    Check(!FragmentReadsAttribute({true, false, false, false, false}, needs), "no clip distances and no planes: not read");
    Check(!FragmentReadsAttribute({true, false, true, true, true}, needs), "a geometry pipeline never reads it: input locations are not shifted");
    Check(!FragmentReadsAttribute({true, true, false, true, true}, needs), "a tessellation pipeline never reads it");
    Check(!FragmentReadsAttribute({true, true, true, true, true}, needs), "geometry with tessellation never reads it");
    Check(!FragmentReadsAttribute({false, false, true, true, false}, needs), "a geometry pipeline without a pixel shader does not ask for a discard shader");
    Check(FragmentReadsAttribute({false, false, false, true, false}, needs), "a vertex-only pipeline without a pixel shader asks for the discard shader");
    Check(!FragmentReadsAttribute({true, false, false, true, true}, false), "a driver that interpolates clip distances right needs no emulation");
}

alignas(RuntimeInfo) unsigned char g_storage[sizeof(RuntimeInfo)];

RuntimeInfo& Poisoned() {
    std::memset(g_storage, 0xCD, sizeof(g_storage));
    return *reinterpret_cast<RuntimeInfo*>(g_storage);
}

// The fragment-stage statements of PipelineCache::RefreshGraphicsStages and BuildRuntimeInfo.
void SelectFragment(RuntimeInfo& fragment, const Draw& draw, bool needs, bool initialize_without_pixel_shader) {
    if (draw.pixel_shader) {
        fragment.Initialize(HwStage::Fragment, SwStage::Fragment);
        fragment.hw.fs.clip_distance_emulation = FragmentReadsAttribute(draw, needs);
    } else if (initialize_without_pixel_shader) {
        fragment.Initialize(HwStage::Fragment, SwStage::Fragment);
        fragment.hw.fs.clip_distance_emulation = FragmentReadsAttribute(draw, needs);
    }
}

void TestNoStaleFragmentState() {
    const Draw a{true, false, false, true, false};
    const Draw b{false, false, true, true, false};
    const Draw c{false, false, false, false, false};

    RuntimeInfo& slot = Poisoned();
    SelectFragment(slot, a, true, true);
    slot.hw.fs.num_inputs = 5;
    Check(slot.hw.fs.clip_distance_emulation, "pipeline A reads the attribute");
    SelectFragment(slot, b, true, true);
    Check(!slot.hw.fs.clip_distance_emulation && slot.hw.fs.num_inputs == 0,
          "A -> B: a geometry pipeline without a pixel shader does not inherit A's state");
    SelectFragment(slot, a, true, true);
    Check(slot.hw.fs.clip_distance_emulation, "B -> A: pipeline A reads it again");
    SelectFragment(slot, c, true, true);
    Check(!slot.hw.fs.clip_distance_emulation, "A -> a pipeline without clip distances and without a pixel shader: cleared");

    RuntimeInfo& stale = Poisoned();
    SelectFragment(stale, a, true, false);
    SelectFragment(stale, b, true, false);
    Check(stale.hw.fs.clip_distance_emulation, "(model of the old behaviour) a pipeline without a pixel shader keeps the previous draw's state");

    RuntimeInfo& garbage = Poisoned();
    SelectFragment(garbage, c, false, true);
    Check(!garbage.hw.fs.clip_distance_emulation && garbage.hw.fs.num_inputs == 0 && garbage.hw.fs.num_samples == 1 &&
              garbage.hw_stage == HwStage::Fragment && garbage.sw_stage == SwStage::Fragment,
          "poisoned storage is fully initialized for a pipeline without a pixel shader");
}

bool PipelineCacheUsesTheRule(const std::string& cache) {
    const auto no_pixel_shader = cache.find("if (!bind_stage(HwStage::Fragment, SwStage::Fragment)) {");
    if (no_pixel_shader == std::string::npos) {
        return false;
    }
    const std::string branch = cache.substr(no_pixel_shader, 480);
    const auto rule = cache.find("bool PipelineCache::FragmentClipDistanceEmulation(const AmdGpu::Regs& regs) const {");
    const std::string function = rule == std::string::npos ? std::string{} : cache.substr(rule, 520);
    return Contains(branch, "fragment.Initialize(HwStage::Fragment, SwStage::Fragment);") &&
           Contains(branch, "fragment.hw.fs.clip_distance_emulation = FragmentClipDistanceEmulation(regs);") &&
           Contains(cache, "info.hw.fs.clip_distance_emulation = FragmentClipDistanceEmulation(regs);") &&
           !Contains(cache, "lowers_user_clip_planes") &&
           Contains(function, "IsStageEnabled(static_cast<u32>(HwStage::Local))") &&
           Contains(function, "IsStageEnabled(static_cast<u32>(HwStage::Geometry))") &&
           Contains(function, "regs.vs_output_control.clip_distance_enable") &&
           Contains(function, "regs.clipper_control.user_clip_plane_enable") &&
           Contains(function, "profile.needs_clip_distance_emulation");
}

bool EmitterUsesTheRule(const std::string& emit) {
    return Contains(emit, "EmulatesClipDistances(hw_stage, sw_stage, profile.needs_clip_distance_emulation)") &&
           Contains(emit, "EmitsEmulatedClipDistances(&info, profile.needs_clip_distance_emulation)") &&
           !Contains(emit, "hw_stage == HwStage::Vertex && profile.needs_clip_distance_emulation") &&
           Contains(emit, "const bool has_clip_distance_inputs = runtime_info.hw.fs.clip_distance_emulation;");
}

void TestProductionWiring() {
    const std::string cache = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    const std::string emit = Source("gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp");
    Check(PipelineCacheUsesTheRule(cache), "the pipeline cache takes the fragment state from the shared rule and its own registers");
    Check(EmitterUsesTheRule(emit), "the SPIR-V emitter writes and reads the attribute under the shared rule");

    std::string legacy = cache;
    const std::string rule = "fragment.Initialize(HwStage::Fragment, SwStage::Fragment);";
    if (const auto at = legacy.find(rule); at != std::string::npos) {
        legacy.erase(at, rule.size());
    }
    Check(!PipelineCacheUsesTheRule(legacy), "the gate fails when a pipeline without a pixel shader is not initialized");
    legacy = cache;
    const std::string call = "if (!bind_stage(HwStage::Fragment, SwStage::Fragment)) {";
    if (const auto at = legacy.find(call); at != std::string::npos) {
        legacy.replace(at, call.size(), "bind_stage(HwStage::Fragment, SwStage::Fragment); if (false) {");
    }
    Check(!PipelineCacheUsesTheRule(legacy), "the gate fails when the no-pixel-shader branch is detached from the bind result");
    legacy = cache;
    const std::string geometry = "regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry)),";
    if (const auto at = legacy.find(geometry); at != std::string::npos) {
        legacy.replace(at, geometry.size(), "false,");
    }
    Check(!PipelineCacheUsesTheRule(legacy), "the gate fails when the geometry stage is dropped from the rule's inputs");

    std::string old_emit = emit;
    const std::string shifted = "EmitsEmulatedClipDistances(&info, profile.needs_clip_distance_emulation)";
    if (const auto at = old_emit.find(shifted); at != std::string::npos) {
        old_emit.replace(at, shifted.size(),
                         "hw_stage == HwStage::Vertex && profile.needs_clip_distance_emulation");
    }
    Check(!EmitterUsesTheRule(old_emit), "the gate fails when the output location shift ignores the software stage");

    const std::string pipeline = Source("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    Check(Contains(pipeline, "runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation"),
          "a pipeline without a pixel shader builds its discard-only shader from the same flag");
}

// The thread-naming exception (MS_VC_EXCEPTION, 0x406D1388) is raised and handled by the caller's __try; the crash guard
// must leave it alone: only the listed fatal and fault codes are ever classified as a crash.
bool ClassifierIgnoresThreadNaming(const std::string& guard) {
    const auto function = guard.find("SosFaultAction sos_crash_classify(");
    if (function == std::string::npos) {
        return false;
    }
    const std::string body = guard.substr(function, guard.find("\n}\n", function) - function);
    return !Contains(body, "406D1388") && !Contains(body, "406d1388") &&
           Contains(body, "default:\n        return SOS_FAULT_IGNORE;");
}

void TestCrashClassifierLeavesThreadNamingAlone() {
    const std::string guard = Source("src/crash_guard.c");
    Check(ClassifierIgnoresThreadNaming(guard), "the crash guard ignores the thread-naming exception");
    std::string mutated = guard;
    const std::string fallthrough = "default:\n        return SOS_FAULT_IGNORE;";
    if (const auto at = mutated.find(fallthrough); at != std::string::npos) {
        mutated.replace(at, fallthrough.size(), "default:\n        return SOS_FAULT_CRASH;");
    }
    Check(!ClassifierIgnoresThreadNaming(mutated), "the gate fails when unknown exception codes become crashes");
}

} // namespace

int main() {
    TestStageRule();
    TestFragmentAgreesWithTheVertexStage();
    TestPipelineKinds();
    TestNoStaleFragmentState();
    TestProductionWiring();
    TestCrashClassifierLeavesThreadNamingAlone();
    std::printf("shader interface tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
