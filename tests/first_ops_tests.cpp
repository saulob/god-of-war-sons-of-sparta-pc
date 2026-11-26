// SOS-BXG9UT: the first guest GPU operations behind the game's first frame (gpu/shim/dcp_first_ops.h). Each one is logged
// started and finished at most once per process, concurrent first callers elect exactly one owner, a failure keeps its
// VkResult, the crash-report summary names the operation still in flight with its shaders, and the production call sites
// (shader translation, SPIR-V, graphics and compute pipelines, draw, dispatch, acquire, the fault reports) are wired.
// No window, GPU, launcher or game.
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dcp_first_ops.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace {

int g_checks = 0, g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[fail] %s\n", what.c_str());
    } else {
        std::printf("[ok]   %s\n", what.c_str());
    }
}

std::mutex g_lines_mutex;
std::vector<std::string> g_lines;

void Capture(const char* line) {
    std::scoped_lock lock{g_lines_mutex};
    g_lines.emplace_back(line);
}

size_t CountStarting(const std::string& prefix) {
    std::scoped_lock lock{g_lines_mutex};
    size_t count = 0;
    for (const auto& line : g_lines) {
        count += line.starts_with(prefix);
    }
    return count;
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string Read(const std::string& relative) {
    std::ifstream file(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string out = text.str();
    std::erase(out, '\r');
    return out;
}

using DcpFirstOps::Op;
using DcpFirstOps::State;
using DcpFirstOps::Tracker;

void TestStartedAndFinishedOnce() {
    g_lines.clear();
    static Tracker tracker{Capture};
    Check(tracker.Begin(Op::GraphicsPipeline, "vs 0x1 ps 0x2 (game)"), "the first caller owns the first pipeline");
    Check(!tracker.Begin(Op::GraphicsPipeline, "vs 0x3 ps 0x4 (game)"), "a later pipeline is not a first one");
    tracker.Finish(Op::GraphicsPipeline, 0, "Success");
    tracker.Finish(Op::GraphicsPipeline, -3, "ErrorInitializationFailed");
    Check(tracker.Get(Op::GraphicsPipeline) == State::Finished, "a finished first operation stays finished");
    Check(CountStarting("Runtime: first graphics pipeline creation started: vs 0x1 ps 0x2 (game)") == 1 &&
              CountStarting("Runtime: first graphics pipeline creation started:") == 1,
          "one started line, naming the first pipeline's shaders");
    Check(CountStarting("Runtime: first graphics pipeline creation finished: Success (result 0)") == 1 &&
              CountStarting("Runtime: first graphics pipeline creation finished:") == 1,
          "one finished line with the VkResult");
}

void TestFailureKeepsResult() {
    g_lines.clear();
    static Tracker tracker{Capture};
    tracker.Begin(Op::ComputePipeline, "cs 0x9 (game)");
    tracker.Finish(Op::ComputePipeline, -13, "ErrorUnknown");
    Check(tracker.Get(Op::ComputePipeline) == State::Failed, "a failing first operation is FAILED");
    Check(CountStarting("Runtime: first compute pipeline creation finished: ErrorUnknown (result -13)") == 1,
          "the failure line names the VkResult and its value");
    char summary[512];
    tracker.Describe(summary, sizeof(summary));
    Check(Has(summary, "compute_pipeline=FAILED"), "the summary shows the failure");
}

void TestSummaryNamesTheOperationInFlight() {
    g_lines.clear();
    static Tracker tracker{Capture};
    tracker.Begin(Op::ShaderTranslate, "fragment shader 0x00000000deadbeef, 120 dwords");
    tracker.Finish(Op::ShaderTranslate, 0, "translated");
    tracker.Begin(Op::SpirvEmit, "Fragment shader 0x00000000deadbeef, interpolation fallback");
    tracker.Finish(Op::SpirvEmit, 0, "emitted");
    tracker.Begin(Op::GraphicsPipeline, "vs 0xaaaa ps 0xbbbb (game)");
    char summary[512];
    const size_t used = tracker.Describe(summary, sizeof(summary));
    Check(used == std::string(summary).size(), "Describe returns the summary's length");
    Check(Has(summary, "translate=done spirv=done graphics_pipeline=STARTED [vs 0xaaaa ps 0xbbbb (game)]"),
          "the summary names the pipeline in flight with its shaders: " + std::string(summary));
    Check(Has(summary, "draw=none") && Has(summary, "acquire=none"), "untouched operations are none");

    char tiny[24];
    const size_t tiny_used = tracker.Describe(tiny, sizeof(tiny));
    Check(tiny_used < sizeof(tiny) && tiny[tiny_used] == '\0', "a small buffer gets a bounded, terminated summary");
}

void TestOnce() {
    g_lines.clear();
    static Tracker tracker{Capture};
    tracker.Once(Op::Draw, "recorded, vs 0x1 ps 0x2, 3 vertices, 1 instances");
    tracker.Once(Op::Draw, "recorded, vs 0x5 ps 0x6, 6 vertices, 1 instances");
    Check(CountStarting("Runtime: first guest draw: recorded, vs 0x1 ps 0x2") == 1 &&
              CountStarting("Runtime: first guest draw:") == 1,
          "the first draw is logged once");
    Check(tracker.Get(Op::Draw) == State::Finished, "a recorded draw is done");
    Check(!tracker.Begin(Op::Draw, "late"), "a draw already recorded cannot be started again");
}

void TestDetailIsOneBoundedLine() {
    g_lines.clear();
    static Tracker tracker{Capture};
    std::string detail = "line one\nline two\r";
    detail += std::string(400, 'x');
    tracker.Begin(Op::Acquire, detail.c_str());
    std::string line;
    {
        std::scoped_lock lock{g_lines_mutex};
        line = g_lines.empty() ? "" : g_lines.front();
    }
    Check(line.find('\n') == std::string::npos && line.find('\r') == std::string::npos,
          "a detail with line breaks stays one log line");
    Check(line.size() < 64 + DcpFirstOps::DetailChars, "the detail is bounded");
}

void TestConcurrentFirstCallersElectOneOwner() {
    g_lines.clear();
    static Tracker tracker{Capture};
    std::atomic<int> owners{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) {
            }
            const std::string detail = "thread " + std::to_string(i);
            if (tracker.Begin(Op::SpirvEmit, detail.c_str())) {
                owners.fetch_add(1);
                tracker.Finish(Op::SpirvEmit, 0, "emitted");
            }
            tracker.Finish(Op::SpirvEmit, -1, "not the owner");
        });
    }
    go.store(true);
    for (auto& thread : threads) {
        thread.join();
    }
    Check(owners.load() == 1, "16 concurrent first callers elect exactly one owner");
    Check(CountStarting("Runtime: first SPIR-V emission started:") == 1 &&
              CountStarting("Runtime: first SPIR-V emission finished:") == 1,
          "concurrent callers print one started and one finished line");
    Check(tracker.Get(Op::SpirvEmit) == State::Finished, "only the owner's result counts");
}

void TestNoLineIsTheGameFramePresent() {
    // launcher/session_outcome.h and server/bug-report/src/session_outcome.js take the exact line "Runtime: first present"
    // as the game's first frame: no first operation may print it.
    for (const char* name : DcpFirstOps::Names) {
        const std::string text = std::string("Runtime: first ") + name;
        Check(text != "Runtime: first present" && !std::string(name).starts_with("present"),
              std::string("operation '") + name + "' cannot be read as the game's first frame");
    }
}

void TestProductionWiring() {
    const std::string recompiler = Read("gpu/shadps4/shader_recompiler/recompiler.cpp");
    const size_t begin = recompiler.find("first = first_ops.Begin(DcpFirstOps::ShaderTranslate, detail);");
    const size_t translate = recompiler.find("EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);");
    const size_t finish = recompiler.find("first_ops.Finish(DcpFirstOps::ShaderTranslate, 0, \"translated\");");
    Check(begin != std::string::npos && translate != std::string::npos && finish != std::string::npos &&
              begin < translate && translate < finish,
          "shader translation starts before the translator runs and finishes after every pass");

    const std::string spirv = Read("gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv.cpp");
    Check(Has(spirv, "first = first_ops.Begin(DcpFirstOps::SpirvEmit, detail);") &&
              Has(spirv, "first_ops.Finish(DcpFirstOps::SpirvEmit, 0, \"emitted\");") && Has(spirv, ": \"fallback\");"),
          "SPIR-V emission is tracked with its interpolation route (fallback named)");

    const std::string graphics = Read("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    const size_t failed = graphics.find("        finish_first(clean_result);\n        throw PipelineCreationFailed{");
    Check(failed != std::string::npos, "a failing first graphics pipeline records its VkResult before it throws");
    Check(Has(graphics, "finish_first(vk::Result::eSuccess);") &&
              Has(graphics, "first = first_ops.Begin(DcpFirstOps::GraphicsPipeline, first_detail.c_str());") &&
              Has(graphics, "binary cache {}"),
          "the first graphics pipeline is started with its shaders and binary-cache route and finished on success");

    const std::string compute = Read("gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp");
    Check(Has(compute, "first_ops.Finish(DcpFirstOps::ComputePipeline, static_cast<int>(clean_result),") &&
              Has(compute, "first_ops.Finish(DcpFirstOps::ComputePipeline, 0, \"Success\");"),
          "the first compute pipeline records failure and success");

    const std::string rasterizer = Read("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    Check(Has(rasterizer, "first_ops.Once(DcpFirstOps::Draw, detail);") &&
              Has(rasterizer, "first_ops.Once(DcpFirstOps::Dispatch, detail);"),
          "the first guest draw and dispatch are recorded");

    const std::string swapchain = Read("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    Check(Has(swapchain, "first_ops.Finish(DcpFirstOps::Acquire, static_cast<int>(result), vk::to_string(result).c_str());"),
          "the first acquire records its VkResult");

    const std::string watch = Read("gpu/shim/dcp_startup_watch.cpp");
    Check(Has(watch, "extern \"C\" void dcpgpu_first_ops_summary(char* out, size_t size) {\n    DcpFirstOps::Global().Describe(out, size);") &&
              Has(watch, "extern \"C\" void dcpgpu_crash_trace(const char* reason) {\n    Common::GpuTrace::Dump(reason);"),
          "the crash report entry points are exported");

    const std::string loader = Read("src/loader.c");
    Check(Has(loader, "\"Crash: first GPU operations: %s\\n\"") && Has(loader, "\"  first GPU operations: %s\\n\""),
          "the Linux and Windows crash reports print the first GPU operations");
}

} // namespace

namespace DcpFirstOps {
Tracker& Global() noexcept {
    static Tracker tracker{Capture};
    return tracker;
}
} // namespace DcpFirstOps

int main() {
    TestStartedAndFinishedOnce();
    TestFailureKeepsResult();
    TestSummaryNamesTheOperationInFlight();
    TestOnce();
    TestDetailIsOneBoundedLine();
    TestConcurrentFirstCallersElectOneOwner();
    TestNoLineIsTheGameFramePresent();
    TestProductionWiring();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
