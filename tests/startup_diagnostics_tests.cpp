#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "common/device_lost.h"
#include "game_profile.h"
#include "startup_phase.h"

extern "C" FILE* sos_stderr_stream(void) { return stdout; }

#define STR2(x) #x
#define STR(x) STR2(x)

namespace {

namespace fs = std::filesystem;
const fs::path kRoot(STR(SOS_SOURCE_DIR));

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", label.c_str());
    }
}

std::string ReadSource(const char* relative) {
    std::ifstream in(kRoot / relative, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool InOrder(const std::string& text, const std::vector<std::string>& needles) {
    size_t pos = 0;
    for (const std::string& needle : needles) {
        const size_t found = text.find(needle, pos);
        if (found == std::string::npos) return false;
        pos = found + needle.size();
    }
    return true;
}

void TestStartupPhaseProgressionAndCrashContext() {
    const char* stages[] = {
        "configuration load",
        "launcher handoff",
        "boot image load",
        "runtime initialization",
        "renderer initialization",
        "renderer selection",
        "window creation",
        "Vulkan instance creation",
        "GPU selection",
        "Vulkan device creation",
        "swapchain creation",
        "module registration",
        "imports resolved",
        "guest image mapped",
        "linked modules initialization",
        "guest entry",
        "first present",
    };

    char context[320] = {};
    for (const char* stage : stages) {
        sos_startup_phase(stage);
        Check(std::string(sos_startup_phase_current()) == stage, std::string("phase updated to ") + stage);
        sos_crash_context_describe(context, sizeof(context));
        const std::string expected = std::string("phase=") + stage + ",";
        Check(std::string(context).find(expected) != std::string::npos,
              std::string("crash context carries ") + expected);
    }

    // Representative startup crashes report the exact last reached stage to Bug Report diagnostics.
    const char* crash_stages[] = {
        "boot image load",
        "Vulkan device creation",
        "swapchain creation",
        "linked modules initialization",
    };
    for (const char* stage : crash_stages) {
        sos_startup_phase(stage);
        sos_crash_context_describe(context, sizeof(context));
        const std::string log = std::string("[run] God of War: Sons of Sparta PC\nCRASH: access violation (0xc0000005)\n  ") +
                                context + "\n";
        const sos::LogDiagnostics diag = sos::AnalyzeLogDiagnostics(log);
        Check(diag.crash_detected && diag.last_startup_stage == stage,
              std::string("startup crash at '") + stage + "' diagnosed accurately");
    }

    // Milestones and checkpoints are bounded (no unbounded high-frequency spam).
    for (int i = 0; i < 40; ++i) sos_startup_milestone("synthetic milestone %d", i);
    for (int i = 0; i < 120; ++i) sos_checkpoint("synthetic checkpoint %d", i);
    Check(std::string(sos_checkpoint_last()).find("synthetic checkpoint 119") != std::string::npos,
          "last checkpoint stays updated even after log line cap");
}

void TestStartupStagesWiredInSharedHostPaths() {
    const std::string loader = ReadSource("src/loader.c");
    Check(InOrder(loader, {
                              "sos_startup_phase(\"configuration load\")",
                              "sos_startup_phase(\"launcher handoff\")",
                              "sos_startup_phase(\"launcher returned\")",
                              "sos_startup_phase(\"boot image load\")",
                              "sos_startup_phase(\"runtime initialization\")",
                              "sos_startup_phase(\"renderer initialization\")",
                              "sos_startup_phase(\"module registration\")",
                              "sos_startup_phase(\"imports resolved\")",
                              "sos_startup_phase(\"guest image mapped\")",
                              "sos_startup_phase(\"linked modules initialization\")",
                              "sos_startup_phase(\"guest entry\")",
                          }),
          "loader.c wires shared host startup stages in expected order");

    const std::string dcpgpu = ReadSource("gpu/shim/dcpgpu.cpp");
    Check(Contains(dcpgpu, "sos_startup_phase(\"renderer selection\")"), "dcpgpu.cpp records renderer selection");
    Check(Contains(dcpgpu, "sos_startup_phase(\"window creation\")"), "dcpgpu.cpp records window creation");
    Check(Contains(dcpgpu, "SetExitFlush") && Contains(dcpgpu, "runtime_log_flush()"),
          "dcpgpu.cpp wires DeviceLost exit log flush and startup phase output");

    const std::string vk_instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    Check(Contains(vk_instance, "sos_startup_phase(\"Vulkan instance creation\")"),
          "vk_instance.cpp records Vulkan instance creation");
    Check(Contains(vk_instance, "sos_startup_phase(\"GPU selection\")"), "vk_instance.cpp records GPU selection");
    Check(Contains(vk_instance, "sos_startup_phase(\"Vulkan device creation\")"),
          "vk_instance.cpp records Vulkan device creation");

    const std::string vk_swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    Check(Contains(vk_swapchain, "sos_startup_phase(\"swapchain creation\")"),
          "vk_swapchain.cpp records initial swapchain creation");

    const std::string watch = ReadSource("gpu/shim/dcp_startup_watch.cpp");
    Check(Contains(watch, "sos_startup_phase(\"first present\")"), "dcp_startup_watch.cpp records first present");
    Check(Contains(watch, "VideoOut open") && Contains(watch, "VideoOut buffers") &&
              Contains(watch, "guest submit") && Contains(watch, "GPU submission") &&
              Contains(watch, "flip prepared") && Contains(watch, "flip taken") && Contains(watch, "present"),
          "dcp_startup_watch.cpp retains all Dragon + Sons pipeline stages");

    const std::string audio = ReadSource("src/runtime_audio.c");
    Check(Contains(audio, "sos_startup_milestone(\"audio backend %s\""),
          "runtime_audio.c logs one-time audio backend startup milestone");

    // Shared host diagnostics apply identically to all supported builds (CUSA57767 1.00 and 1.01).
    const auto builds = sos::SupportedBuilds();
    Check(builds.size() == 2, "shared host startup diagnostics cover both supported builds");
    for (const sos::GameBuild& b : builds) {
        Check(std::string(b.title_id) == "CUSA57767", std::string("supported build covered: ") + b.label);
    }
}

void TestDeviceLostExitFlushHook() {
    static int flush_calls = 0;
    flush_calls = 0;
    Common::DeviceLost::Monitor monitor{Common::DeviceLost::Hooks{
        .print = [](const char*) {},
        .query_fault = nullptr,
        .shutdown = [](const char*, int) {},
        .park = []() {},
    }};
    monitor.SetExitFlush([] { ++flush_calls; });
    Check(monitor.ExitFlush() != nullptr, "SetExitFlush stores flush callback");
    if (auto* fn = monitor.ExitFlush()) fn();
    Check(flush_calls == 1, "ExitFlush callback executes cleanly");
}

} // namespace

int main() {
    TestStartupPhaseProgressionAndCrashContext();
    TestStartupStagesWiredInSharedHostPaths();
    TestDeviceLostExitFlushHook();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
