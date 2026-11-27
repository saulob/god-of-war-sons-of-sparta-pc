// Issue #6 (Linux, Intel Core Ultra 7 255H / Arc 140T "Intel(R) Graphics (ARL)", Mesa ANV 26.2.7 on Xe KMD, v0.1.0):
// the reporter's log, read with the production rules. Its device extensions give the driver-interpolation route with
// the integer fix and no AMD or barycentric path; the log ends silently after "Runtime: semaphore id=2", on the same
// line as the Steam Deck log of issue #5, so the end does not depend on the GPU; the session is never read as normal,
// and the report the current runtime writes for a fault there is read as a startup crash.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "session_outcome.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

using namespace Vulkan::GpuCompat;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    } else {
        std::printf("  [ok]   %s\n", name.c_str());
    }
}

std::string Read(const char* path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

bool StartsWith(const std::string& line, std::string_view prefix) {
    return line.compare(0, prefix.size(), prefix) == 0;
}

bool AnyLine(const std::vector<std::string>& lines, std::string_view prefix) {
    for (const std::string& line : lines) {
        if (StartsWith(line, prefix)) return true;
    }
    return false;
}

std::vector<std::string> From(const std::vector<std::string>& lines, std::string_view first) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (StartsWith(lines[i], first)) return {lines.begin() + std::ptrdiff_t(i), lines.end()};
    }
    return {};
}

struct DeviceReport {
    std::uint32_t vendor_id = 0;
    std::set<std::string> enabled, unavailable;
};

void SplitInto(std::set<std::string>& out, std::string list) {
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find(", ", at);
        if (end == std::string::npos) end = list.size();
        out.insert(list.substr(at, end - at));
        at = end + 2;
    }
}

DeviceReport ReadDevice(const std::vector<std::string>& lines) {
    DeviceReport device;
    constexpr std::string_view enabled = "GPU: Vulkan device extensions: ";
    constexpr std::string_view missing = "operator(): Extension ";
    constexpr std::string_view vendor = "(vendor 0x";
    for (const std::string& line : lines) {
        if (StartsWith(line, enabled)) SplitInto(device.enabled, line.substr(enabled.size()));
        const size_t miss = line.find(missing);
        if (miss != std::string::npos && line.ends_with(" unavailable.")) {
            const size_t name = miss + missing.size();
            device.unavailable.insert(line.substr(name, line.size() - name - 13));
        }
        const size_t id = line.find(vendor);
        if (StartsWith(line, "GPU: ") && id != std::string::npos) {
            device.vendor_id = std::uint32_t(std::stoul(line.substr(id + vendor.size(), 4), nullptr, 16));
        }
    }
    return device;
}

// Instance::CreateDevice and GetGpuCapabilities (vk_instance.cpp/.h): barycentrics are requested only without AMD explicit
// vertex parameters; mixed samples from either the NV or the AMD extension. Mesa ANV is not NVIDIA proprietary.
Capabilities FromDevice(const DeviceReport& device) {
    const auto has = [&](const char* name) { return device.enabled.count(name) != 0; };
    const bool amd = has("VK_AMD_shader_explicit_vertex_parameter");
    return {.vendor_id = device.vendor_id,
            .nvidia_proprietary = false,
            .amd_explicit_vertex_parameter = amd,
            .fragment_shader_barycentric = !amd && has("VK_KHR_fragment_shader_barycentric"),
            .mixed_depth_samples = has("VK_NV_framebuffer_mixed_samples") || has("VK_AMD_mixed_attachment_samples"),
            .provoking_vertex = has("VK_EXT_provoking_vertex")};
}

void TestTheReportedMachine(const std::vector<std::string>& log) {
    Check(AnyLine(log, "Settings: God of War: Sons of Sparta PC v0.1.0, Linux x86-64, game build CUSA57767 1.01 USA"),
          "the log is a v0.1.0 Linux run of build 1.01");
    Check(AnyLine(log, "CPU: Intel(R) Core(TM) Ultra 7 255H") && AnyLine(log, "GPU: selected Intel(R) Graphics (ARL)") &&
              AnyLine(log, "GPU: Vulkan device Intel(R) Graphics (ARL) (Intel open-source Mesa driver 26.2.7, Vulkan 1.4.354)"),
          "Core Ultra 7 255H with Intel Graphics (ARL) on Mesa 26.2.7");
    Check(AnyLine(log, "Session: Gamescope no, XDG_CURRENT_DESKTOP=KDE, XDG_SESSION_TYPE=wayland, MANGOHUD=1"),
          "KDE Wayland session with MangoHud");
    Check(AnyLine(log, "Swapchain: B8G8R8A8Unorm SrgbNonlinear, 4 images, 2560x1440") && AnyLine(log, "Startup: first present"),
          "Vulkan device and swapchain were created and the renderer presented its own first frame");
}

void TestArlProfile(const std::vector<std::string>& log) {
    const DeviceReport device = ReadDevice(log);
    Check(device.vendor_id == 0x8086, "vendor 0x8086 read from the log");
    const std::set<std::string> missing = {
        "VK_EXT_depth_range_unrestricted",   "VK_AMD_shader_explicit_vertex_parameter", "VK_KHR_fragment_shader_barycentric",
        "VK_AMD_shader_image_load_store_lod", "VK_AMD_gcn_shader",                       "VK_AMD_shader_trinary_minmax",
        "VK_NV_framebuffer_mixed_samples",   "VK_AMD_mixed_attachment_samples"};
    Check(device.unavailable == missing, "exactly the eight extensions of the report are unavailable");
    Check(device.enabled.size() == 24 && device.enabled.count("VK_EXT_provoking_vertex") &&
              device.enabled.count("VK_KHR_pipeline_binary") && device.enabled.count("VK_KHR_maintenance5"),
          "the 24 enabled device extensions, provoking vertex and pipeline binaries among them");
    bool disjoint = true;
    for (const std::string& name : device.unavailable) disjoint &= device.enabled.count(name) == 0;
    Check(disjoint, "no unavailable extension is enabled");

    const Capabilities caps = FromDevice(device);
    Check(!caps.amd_explicit_vertex_parameter && !caps.fragment_shader_barycentric && !caps.mixed_depth_samples &&
              caps.provoking_vertex && !caps.nvidia_proprietary,
          "capabilities: no AMD explicit, no barycentrics, no mixed samples, provoking vertex");
    const InterpolationRoute route = ChooseInterpolation(caps);
    Check(route.interpolation == Interpolation::Fallback, "the ARL device takes the driver-interpolation fallback");
    Check(!route.manual_interpolation, "no manual interpolation (it needs barycentrics)");
    Check(route.integer_fix, "the interpolated-integer fix is on");
    const std::uint32_t mixed[2] = {4, 1};
    Check(!NeedsMixedSamplesInfo(caps.mixed_depth_samples, 4, mixed, 2, true, 1),
          "no VkAttachmentSampleCountInfoAMD on a mixed-sample pipeline");
    const std::string summary = Summary(caps);
    Check(summary == "GPU: compatibility vendor=0x8086 interpolation=fallback manual_interpolation=off integer_fix=on "
                     "mixed_samples=no provoking_vertex=yes",
          "the compatibility line the current build prints for this device: " + summary);

    // The same device with the extensions the fallback must never assume.
    Capabilities with_barycentrics = caps;
    with_barycentrics.fragment_shader_barycentric = true;
    Check(ChooseInterpolation(with_barycentrics).interpolation == Interpolation::NativeBarycentric,
          "contrast: the route follows the barycentric capability, not the vendor");
}

void TestSilentEnd(const std::vector<std::string>& log, const std::vector<std::string>& steam_deck) {
    Check(!log.empty() && log.back() == "Runtime: semaphore id=2, attr=0, initial=0, max=256",
          "the v0.1.0 log ends right after semaphore id=2");
    bool diagnostic = false;
    for (std::string_view prefix : {"CRASH:", "Crash:", "STOP:", "Session: normal exit", "Session: ended by signal",
                                    "Stage", "GPU trace:", "GPU device lost", "GPU: compatibility", "Runtime: exit",
                                    "Runtime: runtime_exit", "Runtime: _exit", "Runtime: abort"}) {
        diagnostic |= AnyLine(log, prefix);
    }
    for (std::string_view op : {"shader translation", "SPIR-V emission", "graphics pipeline creation",
                                "compute pipeline creation", "guest draw", "guest dispatch", "swapchain acquire"}) {
        diagnostic |= AnyLine(log, "Runtime: first " + std::string(op));
    }
    Check(!diagnostic, "v0.1.0 wrote no crash, stop, stage, closing record or GPU operation line");
    Check(!AnyLine(log, "Runtime: first present"), "the game's own first frame was never reached");

    const std::vector<std::string> tail = From(log, "Runtime: UserService initialized");
    const std::vector<std::string> deck = From(steam_deck, "Runtime: UserService initialized");
    Check(tail.size() == 24 && tail == deck,
          "the last 24 lines are identical to the issue #5 Steam Deck log (" + std::to_string(tail.size()) + ")");
    Check(AnyLine(steam_deck, "GPU: selected AMD Custom GPU 0932 (RADV VANGOGH)") &&
              AnyLine(steam_deck, "Settings: God of War: Sons of Sparta PC v0.1.0, Linux x86-64"),
          "issue #5 is a v0.1.0 Linux run on another GPU and driver (RADV VANGOGH): the end point is not the GPU's");
}

void TestOutcome(const std::string& text) {
    const sos::SessionOutcome outcome = sos::AnalyzeSessionOutcome(text);
    Check(outcome.key == "startup_hang", "the v0.1.0 log is read as an unfinished start: " + outcome.key);
    Check(outcome.key != "normal_exit" && outcome.detail.find("without a closing record") != std::string::npos,
          "never as a normal session: " + outcome.detail);

    // What the current runtime adds when a fatal signal ends the process at the same point (loader.c posix_fault_handler).
    const std::string current = text +
        "CRASH: signal 11 (SIGSEGV) code 1\n"
        "Crash: startup phase: guest entry\n"
        "Crash: last completed stage: linked modules initialization\n"
        "Crash: thread 4242 (main)\n"
        "Crash: guest fault at guest offset 0x26bc088, address 0x80ac90000\n"
        "Crash: first GPU operations: translate=none spirv=none graphics_pipeline=none compute_pipeline=none draw=none "
        "dispatch=none acquire=none\n";
    const sos::SessionOutcome crashed = sos::AnalyzeSessionOutcome(current);
    Check(crashed.key == "startup_crash", "with the current crash report it reads as a crash while starting: " + crashed.key);
}

} // namespace

int main() {
    const std::string text = Read("tests/fixtures/issue_6_v010_linux_intel_arl_log.txt");
    const std::string deck = Read("tests/fixtures/issue_5_v010_steam_deck_tail_log.txt");
    if (text.empty() || deck.empty()) {
        std::printf("  [fail] the issue #6 and issue #5 fixtures could not be read\n");
        return 1;
    }
    const std::vector<std::string> log = Lines(text);
    TestTheReportedMachine(log);
    TestArlProfile(log);
    TestSilentEnd(log, Lines(deck));
    TestOutcome(text);
    std::printf("intel_arc_linux: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
