// SOS-BXG9UT: the real shader recompiler (GCN -> IR -> SPIR-V) on the interpolation route of each GPU family, with every
// module checked by spirv-val. The Intel Arc 140T (ARL, Mesa ANV on Xe KMD) of the report has neither
// VK_AMD_shader_explicit_vertex_parameter nor VK_KHR_fragment_shader_barycentric (and no shaderFloat64): its fragment
// shaders take the driver-interpolation fallback, which no development or Steam Deck run ever compiled (RTX 4070: native
// barycentrics with manual interpolation; Steam Deck: AMD explicit). Checked here, headless and without a GPU:
//  - a smooth + flat + integer-truncation pixel shader is valid SPIR-V on every route;
//  - the fallback module uses no barycentric or AMD explicit-vertex capability, extension, built-in or decoration that the
//    device does not expose, decorates the flat input Flat, and carries the interpolated-integer fix;
//  - a P10 V_INTERP_MOV_F32 of a smooth attribute is valid on the AMD and barycentric routes, and on the fallback it stops
//    with status 23 naming the shader, after draining the session log (exit flush), never silently.
// Linux only (spirv-val and spirv-dis from SPIRV-Tools); tools/test_interpolation_spirv.ps1 builds and runs it under WSL.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "common/device_lost.h"
#include "dcp_first_ops.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

// The runtime's optimization toggles (src/runtime_memory.c in the game): none disabled.
extern "C" std::uint64_t runtime_disabled_optimizations;
std::uint64_t runtime_disabled_optimizations = 0;

namespace DcpFirstOps {
Tracker& Global() noexcept {
    static Tracker tracker{};
    return tracker;
}
} // namespace DcpFirstOps

namespace {

int g_checks = 0, g_failures = 0;
std::string g_work = "/tmp";

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[fail] %s\n", what.c_str());
    } else {
        std::printf("[ok]   %s\n", what.c_str());
    }
    std::fflush(stdout);
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// --- Sea Islands encodings (GCN 1.1, the PS4's ISA) ---
constexpr u32 Vintrp(u32 op, u32 vdst, u32 attr, u32 chan, u32 vsrc) {
    return (0x32u << 26) | (vdst << 18) | (op << 16) | (attr << 10) | (chan << 8) | vsrc;
}
constexpr u32 InterpP1(u32 vdst, u32 attr, u32 chan, u32 i_vgpr) { return Vintrp(0, vdst, attr, chan, i_vgpr); }
constexpr u32 InterpP2(u32 vdst, u32 attr, u32 chan, u32 j_vgpr) { return Vintrp(1, vdst, attr, chan, j_vgpr); }
// selector: 0 = P10, 1 = P20, 2 = P0
constexpr u32 InterpMov(u32 vdst, u32 attr, u32 chan, u32 selector) { return Vintrp(2, vdst, attr, chan, selector); }
constexpr u32 Vop1(u32 op, u32 vdst, u32 src_vgpr) { return (0x3Fu << 25) | (vdst << 17) | (op << 9) | (256 + src_vgpr); }
constexpr u32 CvtF32U32(u32 vdst, u32 src) { return Vop1(6, vdst, src); }
constexpr u32 CvtU32F32(u32 vdst, u32 src) { return Vop1(7, vdst, src); }
constexpr u32 ExpMrt0Word0 = (0x3Eu << 26) | (1u << 12) | (1u << 11) | (0u << 4) | 0xFu; // mrt0, xyzw, done, vm
constexpr u32 ExpVgprs(u32 a, u32 b, u32 c, u32 d) { return a | (b << 8) | (c << 16) | (d << 24); }
constexpr u32 SMovVccHi = 0xBEEB03FFu; // s_mov_b32 vcc_hi, literal (every PS4 shader starts with it)
constexpr u32 SEndPgm = 0xBF810000u;

// v0/v1: I/J of the perspective centre. attr0 smooth (x, y), attr1 flat; v5 is an integer carried as a float and
// truncated (the instanced-object pattern of the interpolated-integer fix).
std::vector<u32> SmoothFlatIntegerShader() {
    return {SMovVccHi, 0,
            InterpP1(2, 0, 0, 0), InterpP2(2, 0, 0, 1),
            InterpP1(3, 0, 1, 0), InterpP2(3, 0, 1, 1),
            InterpMov(4, 1, 0, 2),
            InterpP1(5, 2, 0, 0), InterpP2(5, 2, 0, 1),
            CvtU32F32(5, 5), CvtF32U32(5, 5),
            ExpMrt0Word0, ExpVgprs(2, 3, 4, 5),
            SEndPgm};
}

// The edge of a smooth attribute as a value (P1 - P0): needs per-vertex attributes.
std::vector<u32> PerVertexEdgeShader() {
    return {SMovVccHi, 0,
            InterpP1(2, 0, 0, 0), InterpP2(2, 0, 0, 1),
            InterpMov(3, 0, 0, 0),
            InterpMov(4, 0, 0, 2),
            InterpMov(5, 0, 0, 1),
            ExpMrt0Word0, ExpVgprs(2, 3, 4, 5),
            SEndPgm};
}

struct Route {
    const char* name;
    Vulkan::GpuCompat::Capabilities caps;
    bool float64;
    u32 subgroup;
};

Shader::Profile MakeProfile(const Route& route) {
    const auto interpolation = Vulkan::GpuCompat::ChooseInterpolation(route.caps);
    Shader::Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    profile.max_shared_memory_size = 65536;
    profile.max_geometry_output_vertices = 256;
    profile.max_geometry_total_output_components = 1024;
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = route.subgroup;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = true;
    profile.support_float16 = true;
    profile.support_float64 = route.float64;
    profile.supports_amd_shader_explicit_vertex_parameter = route.caps.amd_explicit_vertex_parameter;
    profile.supports_fragment_shader_barycentric = route.caps.fragment_shader_barycentric;
    profile.needs_manual_interpolation = interpolation.manual_interpolation;
    profile.needs_integer_interpolation_fix = interpolation.integer_fix;
    profile.supports_shader_stencil_export = true;
    return profile;
}

Shader::RuntimeInfo FragmentRuntime() {
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    runtime.hw.fs.en_flags.persp_center_ena = 1;
    runtime.hw.fs.addr_flags.persp_center_ena = 1;
    runtime.hw.fs.num_inputs = 3;
    runtime.hw.fs.inputs[0].param_index = 0;
    runtime.hw.fs.inputs[1].param_index = 1;
    runtime.hw.fs.inputs[1].is_flat = 1;
    runtime.hw.fs.inputs[2].param_index = 2;
    auto& mrt0 = runtime.hw.fs.color_buffers[0];
    mrt0.data_format = AmdGpu::DataFormat::Format32_32_32_32;
    mrt0.num_format = AmdGpu::NumberFormat::Float;
    mrt0.export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    mrt0.swizzle = {AmdGpu::CompSwizzle::Red, AmdGpu::CompSwizzle::Green, AmdGpu::CompSwizzle::Blue,
                    AmdGpu::CompSwizzle::Alpha};
    runtime.hw.fs.mrt0_export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    runtime.props.num_input_vgprs = 2;
    runtime.props.num_allocated_vgprs = 8;
    return runtime;
}

std::vector<u32> Compile(const std::vector<u32>& code, u64 hash, const Shader::Profile& profile) {
    Shader::Pools pools;
    Shader::Info info{};
    info.hw_stage = Shader::HwStage::Fragment;
    info.sw_stage = Shader::SwStage::Fragment;
    info.pgm_hash = hash;
    info.pgm_base = reinterpret_cast<VAddr>(code.data());
    Shader::RuntimeInfo runtime = FragmentRuntime();
    const auto program = Shader::TranslateProgram(std::span<const u32>{code}, pools, info, runtime, profile);
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

int Run(const std::string& command, std::string* output) {
    const std::string out_file = g_work + "/cmd.txt";
    const int status = std::system((command + " > '" + out_file + "' 2>&1").c_str());
    if (output) {
        std::ifstream in(out_file);
        std::stringstream text;
        text << in.rdbuf();
        *output = text.str();
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct Module {
    bool valid = false;
    std::string text;
    std::string errors;
};

Module Validate(const std::vector<u32>& words, const std::string& name) {
    Module module;
    const std::string path = g_work + "/" + name + ".spv";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * sizeof(u32)));
    }
    module.valid = !words.empty() && Run("spirv-val --target-env vulkan1.3 '" + path + "'", &module.errors) == 0;
    Run("spirv-dis --raw-id '" + path + "'", &module.text);
    return module;
}

const Route kRoutes[] = {
    {"Steam Deck (RADV, AMD explicit vertex parameter)",
     {.vendor_id = 0x1002, .amd_explicit_vertex_parameter = true, .mixed_depth_samples = true, .provoking_vertex = true},
     true, 64},
    {"NVIDIA RTX (native barycentrics, manual interpolation)",
     {.vendor_id = 0x10de, .nvidia_proprietary = true, .fragment_shader_barycentric = true, .mixed_depth_samples = true,
      .provoking_vertex = true},
     true, 32},
    {"Intel Xe2 ANV (native barycentrics, driver interpolation)",
     {.vendor_id = 0x8086, .fragment_shader_barycentric = true, .provoking_vertex = true}, false, 32},
    // SOS-BXG9UT: no AMD explicit, no KHR barycentrics, no mixed samples, provoking vertex; also no shaderFloat64.
    {"Intel Arc 140T ARL (fallback, SOS-BXG9UT)", {.vendor_id = 0x8086, .provoking_vertex = true}, false, 32},
};
constexpr size_t FallbackRoute = 3;

void TestSmoothFlatIntegerOnEveryRoute() {
    const auto code = SmoothFlatIntegerShader();
    for (size_t r = 0; r < std::size(kRoutes); ++r) {
        const Route& route = kRoutes[r];
        const auto profile = MakeProfile(route);
        const auto words = Compile(code, 0x5050000000000001ull + r, profile);
        const Module module = Validate(words, "smooth_flat_" + std::to_string(r));
        Check(module.valid, std::string(route.name) + ": smooth/flat/integer pixel shader passes spirv-val" +
                                (module.valid ? "" : ("\n" + module.errors)));
        const bool fix = Vulkan::GpuCompat::ChooseInterpolation(route.caps).integer_fix;
        // 1 + 2^-16 (shader_recompiler/ir/passes/interpolated_integer.h)
        const bool nudged = Has(module.text, "1.00001526");
        Check(nudged == fix, std::string(route.name) + ": interpolated-integer fix " + (fix ? "applied" : "absent"));
        if (r == FallbackRoute) {
            for (const char* banned :
                 {"FragmentBarycentricKHR", "SPV_KHR_fragment_shader_barycentric", "BaryCoordKHR", "BaryCoordNoPerspKHR",
                  "PerVertexKHR", "SPV_AMD_shader_explicit_vertex_parameter", "ExplicitInterpAMD", "InterpolateAtVertexAMD",
                  "BaryCoordSmoothAMD", "BaryCoordPullModelAMD", "Float64"}) {
                Check(!Has(module.text, banned),
                      std::string("fallback module has no ") + banned + " (the ARL device lacks it)");
            }
            Check(Has(module.text, "Decorate") && Has(module.text, " Flat"),
                  "fallback: the flat input is decorated Flat (driver flat interpolation, provoking vertex)");
        }
    }
}

void TestPerVertexEdgeNeedsPerVertexRoutes() {
    const auto code = PerVertexEdgeShader();
    for (size_t r = 0; r < FallbackRoute; ++r) {
        const auto words = Compile(code, 0x5060000000000001ull + r, MakeProfile(kRoutes[r]));
        const Module module = Validate(words, "edge_" + std::to_string(r));
        Check(module.valid, std::string(kRoutes[r].name) + ": a P10/P20 V_INTERP_MOV_F32 compiles to valid SPIR-V" +
                                (module.valid ? "" : ("\n" + module.errors)));
    }
}

// The fallback cannot express P10/P20: it stops, with status 23, after naming the shader and draining the log.
int EdgeChild() {
    Common::DeviceLost::Global().SetExitFlush([] {
        std::fputs("Test: exit flush ran\n", stderr);
        std::fflush(stderr);
    });
    (void)Compile(PerVertexEdgeShader(), 0x00000000BADC0DE5ull, MakeProfile(kRoutes[FallbackRoute]));
    return 3;
}

void TestFallbackStopsWithAName(const std::string& self) {
    std::string output;
    const int status = Run("'" + self + "' --edge-child", &output);
    Check(status == 23, "fallback: a P10 V_INTERP_MOV_F32 of a smooth attribute ends with status 23 (" +
                            std::to_string(status) + ")");
    Check(Has(output, "V_INTERP_MOV_F32 P10 of smooth attribute 0 in shader 0x00000000badc0de5 needs per-vertex attributes"),
          "fallback: the stop names the instruction, the attribute and the shader");
    const size_t stop = output.find("STOP: GPU library assertion failed");
    const size_t flushed = output.find("Test: exit flush ran");
    Check(stop != std::string::npos && flushed != std::string::npos && stop < flushed,
          "fallback: the session log is drained after the stop line");
    Check(Has(output, "GPU trace: summary: GPU assertion failed; last event: compile fragment shader 0x00000000badc0de5"),
          "fallback: the GPU trace names the shader being compiled");
    Check(Has(output, "Runtime: first shader translation started: fragment shader 0x00000000badc0de5"),
          "fallback: the first-operation line names the shader in translation");
}

} // namespace

int main(int argc, char** argv) {
    const std::string self = argc > 0 ? argv[0] : "interpolation_fallback_spirv_tests";
    if (argc == 2 && std::string{argv[1]} == "--edge-child") {
        return EdgeChild();
    }
    char work[] = "/tmp/sos-interp-XXXXXX";
    if (mkdtemp(work)) {
        g_work = work;
    }
    std::string version;
    if (Run("spirv-val --version", &version) != 0) {
        std::printf("spirv-val is not installed: not applicable\n");
        return 77;
    }
    TestSmoothFlatIntegerOnEveryRoute();
    TestPerVertexEdgeNeedsPerVertexRoutes();
    TestFallbackStopsWithAName(self);
    Run("rm -rf '" + g_work + "'", nullptr);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
