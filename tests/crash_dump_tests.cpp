#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "game_profile.h"
#include "log_scrubber.h"

extern "C" {
#include "runtime_log.h"
int sos_crash_dump_path(char* out, size_t size, const char* log_path, int attempt);
}

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace sos;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", label.c_str());
    }
}

void WriteFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), std::streamsize(content.size()));
}

void WriteMinidump(const fs::path& path, size_t size, const char magic[4] = "MDMP") {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::string buf(std::min<size_t>(size, 65536), '\0');
    if (size >= 4) std::memcpy(buf.data(), magic, 4);
    size_t remaining = size;
    bool first = true;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, buf.size());
        if (!first && chunk >= 4) std::memset(buf.data(), 0, 4);
        out.write(buf.data(), std::streamsize(chunk));
        remaining -= chunk;
        first = false;
    }
}

void SetWriteTimeOffset(const fs::path& path, int seconds_from_now) {
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(path, now + std::chrono::seconds(seconds_from_now), ec);
}

ReportFacts SampleFacts() {
    return {"0.1.0", "CUSA57767 1.01", "1.01", "CUSA57767", "Windows 11 (build 26200)"};
}

void TestDumpNamingAndPath(const fs::path& dir) {
    Check(DumpNameOf("sons_of_sparta_005.log") == "sons_of_sparta_005.dmp", "valid log maps to matching .dmp");
    Check(DumpNameOf("sons_of_sparta_12345.log") == "sons_of_sparta_12345.dmp", "5-digit log maps to .dmp");
    Check(DumpNameOf("sons_of_sparta_05.log").empty(), "2-digit log name rejected");
    Check(DumpNameOf("other_005.log").empty(), "unrelated prefix rejected");
    Check(DumpNameOf("../sons_of_sparta_005.log").empty(), "path traversal rejected");

    char out[260] = {};
    Check(sos_crash_dump_path(out, sizeof(out), "C:\\logs\\sons_of_sparta_007.log", 1) == 1 &&
              std::string(out) == "C:\\logs\\sons_of_sparta_007.dmp",
          "crash_guard attempt 1 dump path matches log stem");
    Check(sos_crash_dump_path(out, sizeof(out), "C:\\logs\\sons_of_sparta_007.log", 2) == 1 &&
              std::string(out) == "C:\\logs\\sons_of_sparta_007_2.dmp",
          "crash_guard attempt 2 dump path appends _2");

    // Verify runtime_log_create reserves numbers held by existing .dmp and _2.dmp files independently.
    const fs::path reserve_dir = dir / "reserve_dmp";
    fs::create_directories(reserve_dir);
    WriteMinidump(reserve_dir / "sons_of_sparta_041.dmp", 64);
    char created_path[4200] = {};
    void* handle = nullptr;
    int ok = runtime_log_create(reserve_dir.string().c_str(), nullptr, created_path, sizeof(created_path), &handle);
    Check(ok == 1, "runtime_log_create succeeds with existing .dmp");
#ifdef _WIN32
    if (ok && handle) CloseHandle(static_cast<HANDLE>(handle));
#endif
    Check(std::string(created_path).find("sons_of_sparta_042.log") != std::string::npos,
          "new log skips number reserved by sons_of_sparta_041.dmp -> 042.log");

    const fs::path reserve_dir2 = dir / "reserve_dmp2";
    fs::create_directories(reserve_dir2);
    WriteMinidump(reserve_dir2 / "sons_of_sparta_050_2.dmp", 64);
    handle = nullptr;
    ok = runtime_log_create(reserve_dir2.string().c_str(), nullptr, created_path, sizeof(created_path), &handle);
    Check(ok == 1, "runtime_log_create succeeds with existing _2.dmp");
#ifdef _WIN32
    if (ok && handle) CloseHandle(static_cast<HANDLE>(handle));
#endif
    Check(std::string(created_path).find("sons_of_sparta_051.log") != std::string::npos,
          "new log skips number reserved by sons_of_sparta_050_2.dmp -> 051.log");
}

void TestDumpValidationRules(const fs::path& dir) {
    const fs::path logs = dir / "validation";
    fs::create_directories(logs);
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    const fs::path log_path = logs / "sons_of_sparta_005.log";
    WriteFile(log_path, "[run] God of War: Sons of Sparta PC\nRuntime: Unity bootstrap entered\nCRASH: access violation\n");
    SetWriteTimeOffset(log_path, -10);

    GameLog log;
    log.path = log_path;
    log.name = "sons_of_sparta_005.log";
    log.written = now - 10;
    log.bytes = 120;

    // Missing dump
    const fs::path dump_path = logs / "sons_of_sparta_005.dmp";
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kMissing, "missing dump reported as kMissing");

    // Valid same-run MDMP dump
    WriteMinidump(dump_path, 4096);
    SetWriteTimeOffset(dump_path, -12);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kNone, "valid same-run MDMP dump accepted");

    // Different-run dump name rejected
    const fs::path other_dump = logs / "sons_of_sparta_004.dmp";
    WriteMinidump(other_dump, 4096);
    SetWriteTimeOffset(other_dump, -12);
    Check(CheckCrashDump(log, other_dump, now) == DumpProblem::kNotThisLog, "different-run dump name rejected");

    // Dump in another directory rejected
    const fs::path sub = logs / "sub";
    fs::create_directories(sub);
    const fs::path sub_dump = sub / "sons_of_sparta_005.dmp";
    WriteMinidump(sub_dump, 4096);
    SetWriteTimeOffset(sub_dump, -12);
    Check(CheckCrashDump(log, sub_dump, now) == DumpProblem::kNotThisLog, "dump outside log directory rejected");

    // Old dump (> 120 s away from log) rejected
    SetWriteTimeOffset(dump_path, -500);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kNotThisLog, "old dump (>120s) rejected");

    // Future / unreasonable timestamp dump rejected
    SetWriteTimeOffset(dump_path, 600);
    GameLog future_log = log;
    future_log.written = now + 590;
    Check(CheckCrashDump(future_log, dump_path, now) == DumpProblem::kNotThisLog, "future timestamp dump rejected");

    // Zero-size dump rejected
    WriteFile(dump_path, "");
    SetWriteTimeOffset(dump_path, -10);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kEmpty, "zero-size dump rejected");

    // Too small (< 32 bytes) rejected
    WriteMinidump(dump_path, 16);
    SetWriteTimeOffset(dump_path, -10);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kNotADump, "dump below 32 bytes rejected");

    // Renamed .dmp without MDMP magic rejected
    WriteMinidump(dump_path, 256, "TEXT");
    SetWriteTimeOffset(dump_path, -10);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kNotADump, "renamed .dmp without MDMP magic rejected");

    // Oversize dump (> 8 MiB) rejected
    WriteMinidump(dump_path, kMostDumpBytes + 1);
    SetWriteTimeOffset(dump_path, -10);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kTooLarge, "oversize dump (>8 MiB) rejected");

    // Directory masquerading as .dmp rejected
    fs::remove(dump_path);
    fs::create_directory(dump_path);
    Check(CheckCrashDump(log, dump_path, now) == DumpProblem::kNotAFile, "directory named .dmp rejected");
    fs::remove(dump_path);
}

void TestSameExecutionAndOptionalAttachment(const fs::path& dir) {
    const fs::path logs = dir / "same_exec";
    fs::create_directories(logs);
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    // Run A crashed and produced log 001 + dump 001
    const fs::path log_a = logs / "sons_of_sparta_001.log";
    const fs::path dump_a = logs / "sons_of_sparta_001.dmp";
    WriteFile(log_a, "[run] God of War: Sons of Sparta PC\nRuntime: Unity bootstrap entered\n"
                     "CRASH: access violation (0xc0000005)\n  phase=guest entry, render 1920x1080 (native) 16:9\n"
                     "  minidump: sons_of_sparta_001.dmp (next to this log)\n");
    WriteMinidump(dump_a, 512);
    SetWriteTimeOffset(log_a, -40);
    SetWriteTimeOffset(dump_a, -41);

    // Run B started later without crashing (log 002, no dump)
    const fs::path log_b = logs / "sons_of_sparta_002.log";
    WriteFile(log_b, "[run] God of War: Sons of Sparta PC\nRuntime: Unity bootstrap entered\n"
                     "Startup: first present +1200 ms\n");
    SetWriteTimeOffset(log_b, -5);

    const std::vector<GameLog> found = FindGameLogs(logs, {}, kMostGameLogs, now);
    Check(found.size() == 2, "both runs discovered");
    if (found.size() != 2) return;

    const GameLog& run_b = found[0];
    const GameLog& run_a = found[1];
    Check(run_b.name == "sons_of_sparta_002.log", "newest run B is first");
    Check(!run_b.Crashed(), "healthy run B does not show Crash detected");
    Check(!run_b.crash.has_value() && !run_b.CanAttachCrash(), "run A dump is never attached to run B");

    Check(run_a.name == "sons_of_sparta_001.log", "crashed run A is second");
    Check(run_a.Crashed(), "crashed run A shows Crash detected");
    Check(run_a.CanAttachCrash(), "crashed run A can attach its matching dump");
    Check(DefaultAttachCrashReport(run_a), "crash attachment defaults on for valid crash dump");
    Check(!DefaultAttachCrashReport(run_b), "crash attachment defaults off when no dump");

    const PrivateFacts priv{};
    const ReportFacts facts = SampleFacts();

    // Optional attachment disabled: report builds cleanly without dump
    const auto pkg_no_dump = BuildBugReportPackage("Crashed during combat encounter.", run_a, facts, priv, false, now);
    Check(pkg_no_dump.has_value(), "report without dump attachment builds");
    if (pkg_no_dump) {
        Check(!pkg_no_dump->crash_dump.has_value(), "disabled attachment omits crash_dump");
        Check(pkg_no_dump->Value("dump_available") == "yes", "metadata still notes dump_available=yes");
        Check(pkg_no_dump->Value("dump_included") == "no", "metadata records dump_included=no");
        const std::string json = BugReportRequestJson(*pkg_no_dump);
        Check(json.find("\"dump_file\"") == std::string::npos, "JSON omits dump_file when disabled");
    }

    // Optional attachment enabled: includes dump metadata
    const auto pkg_with_dump = BuildBugReportPackage("Crashed during combat encounter.", run_a, facts, priv, true, now);
    Check(pkg_with_dump.has_value(), "report with dump attachment builds");
    if (pkg_with_dump) {
        Check(pkg_with_dump->crash_dump.has_value(), "enabled attachment populates crash_dump");
        Check(pkg_with_dump->Value("dump_included") == "yes", "metadata records dump_included=yes");
        Check(pkg_with_dump->Value("crash_detected") == "yes", "metadata records crash_detected=yes");
        Check(pkg_with_dump->Value("crash_kind") == "generic_crash", "generic crash is not mislabeled as device lost");
        Check(pkg_with_dump->Value("device_lost") == "no", "generic crash has device_lost=no");
        Check(pkg_with_dump->Value("device_lost_reason").empty(), "generic crash has empty device_lost_reason");
        Check(pkg_with_dump->Value("last_startup_stage") == "guest entry", "last startup stage extracted from crash context");
        const std::string json = BugReportRequestJson(*pkg_with_dump);
        Check(json.find("\"dump_file\":\"sons_of_sparta_001.dmp\"") != std::string::npos, "JSON carries dump_file");
        Check(json.find("\"dump_bytes\":512") != std::string::npos, "JSON carries dump_bytes");
    }

    // Normal report without any dump works cleanly
    const auto pkg_normal = BuildBugReportPackage("Minor visual glitch in main menu.", run_b, facts, priv, true, now);
    Check(pkg_normal.has_value(), "normal non-crash report without dump succeeds");
    if (pkg_normal) {
        Check(!pkg_normal->crash_dump.has_value(), "normal report has no dump");
        Check(pkg_normal->Value("crash_detected") == "no", "normal report has crash_detected=no");
        Check(pkg_normal->Value("dump_available") == "no", "normal report has dump_available=no");
    }

    // Deleted / unreadable dump right before build does NOT break report creation
    fs::remove(dump_a);
    const auto pkg_after_remove = BuildBugReportPackage("Dump disappeared before sending.", run_a, facts, priv, true, now);
    Check(pkg_after_remove.has_value(), "missing dump at build time does not break report creation");
    if (pkg_after_remove) {
        Check(!pkg_after_remove->crash_dump.has_value(), "disappeared dump is safely ignored");
        Check(pkg_after_remove->Value("dump_included") == "no", "disappeared dump marks dump_included=no");
    }
}

void TestDeviceLostDiagnostics() {
    // 1. Generic crash mentioning Vulkan must NOT be labeled device lost.
    const std::string generic_log =
        "[run] God of War: Sons of Sparta PC\n"
        "Renderer: selected Vulkan, active Vulkan\n"
        "GPU: selected NVIDIA GeForce RTX 4070\n"
        "CRASH: access violation (0xc0000005)\n"
        "  phase=guest entry, render 1920x1080 (native) 16:9\n";
    const LogDiagnostics g = AnalyzeLogDiagnostics(generic_log);
    Check(g.crash_detected, "generic crash sets crash_detected");
    Check(g.crash_kind == CrashKind::kGenericCrash, "generic Vulkan-mentioning crash stays generic_crash");
    Check(!g.device_lost, "generic crash does not set device_lost");
    Check(g.device_lost_reason.empty(), "generic crash has no fabricated device_lost_reason");
    Check(!g.device_fault_available, "generic crash has no fault data");

    // 2. Authoritative root device-lost wins over later device-lost / secondary lines; fault data captured & bounded.
    std::string huge_desc(600, 'A');
    const std::string dl_log =
        "[run] God of War: Sons of Sparta PC\n"
        "GPU: selected AMD Radeon RX 7800 XT\n"
        "GPU device lost: first detected in vkQueueSubmit (site flush), VkResult VK_ERROR_DEVICE_LOST (-4), thread 42, draw tick 120, last submitted draw tick 121\n"
        "GPU timelines: draw submitted 121 completed 118; present submitted 50 completed 50\n"
        "Device fault: " + huge_desc + "\n"
        "Device fault: invalid read at 0xdeadbeef (precision 0x1000)\n"
        "Device fault: 2 vendor records, first 0x1234 data 0x5678: shader page fault\n"
        "GPU device lost: vkAcquireNextImageKHR on thread 77 stopped, a consequence of the first report\n"
        "GPU device lost: first detected in vkQueuePresentKHR (site present), VkResult VK_ERROR_DEVICE_LOST (-4), thread 99\n"
        "Runtime: GPU device lost: first detected in vkQueueSubmit; the process ends with status 25\n"
        "Startup phase: first present\n";
    const LogDiagnostics dl = AnalyzeLogDiagnostics(dl_log);
    Check(dl.crash_detected, "device-lost exit sets crash_detected");
    Check(dl.crash_kind == CrashKind::kDeviceLost, "device-lost exit classified as device_lost");
    Check(dl.device_lost, "device_lost is true");
    Check(dl.device_lost_reason.find("vkQueueSubmit (site flush)") != std::string::npos,
          "authoritative first root cause preserved");
    Check(dl.device_lost_reason.find("vkQueuePresentKHR") == std::string::npos,
          "later device-lost line never replaces authoritative root cause");
    Check(dl.device_lost_reason.find("draw submitted 121 completed 118") != std::string::npos,
          "timeline ticks included in device-lost reason");
    Check(dl.device_fault_available, "VK_EXT_device_fault marked available");
    Check(dl.device_fault_summary.size() <= 512, "large fault data is bounded");
    Check(dl.device_fault_summary.find("invalid read at 0xdeadbeef") != std::string::npos, "fault address included");
    Check(dl.last_startup_stage == "first present", "last startup stage captured");

    // 3. Absence of VK_EXT_device_fault remains valid.
    const std::string no_fault_log =
        "GPU device lost: first detected in vkWaitForFences (site acquire), VkResult VK_ERROR_DEVICE_LOST (-4), thread 12\n"
        "GPU timelines: draw submitted 10 completed 9\n"
        "Device fault: unavailable (no VK_EXT_device_fault)\n"
        "Runtime: GPU device lost: first detected in vkWaitForFences; the process ends with status 25\n";
    const LogDiagnostics nf = AnalyzeLogDiagnostics(no_fault_log);
    Check(nf.device_lost && nf.crash_kind == CrashKind::kDeviceLost, "device lost valid without VK_EXT_device_fault");
    Check(!nf.device_fault_available, "unavailable VK_EXT_device_fault marks device_fault_available=false");
    Check(nf.device_fault_status == "unavailable", "unavailable fault status recorded");

    // 4. Device lost followed by crash exception.
    const std::string dl_crash =
        "GPU device lost: first detected in vkQueueSubmit (site submit), VkResult VK_ERROR_DEVICE_LOST (-4), thread 5\n"
        "CRASH: access violation (0xc0000005)\n";
    const LogDiagnostics dlc = AnalyzeLogDiagnostics(dl_crash);
    Check(dlc.crash_kind == CrashKind::kDeviceLostCrash, "device lost + CRASH classified as device_lost_crash");

    // 5. Manual report with prior device-lost line (no fatal exit / crash line).
    const std::string prior_dl =
        "GPU device lost: first detected in vkQueueSubmit (site submit), VkResult VK_ERROR_DEVICE_LOST (-4), thread 5\n";
    const LogDiagnostics pdl = AnalyzeLogDiagnostics(prior_dl);
    Check(pdl.crash_kind == CrashKind::kManualWithPriorDeviceLost, "prior device lost classified distinctly");
    Check(!pdl.crash_detected, "manual report without fatal exit does not fabricate crash_detected");
}

void TestScrubberPrivacy() {
    PrivateFacts facts;
    facts.user_names = {"SauloSecretUser"};
    facts.machine_names = {"SPARTA-DEV-PC"};
    facts.profile_folders = {"C:\\Users\\SauloSecretUser"};
    facts.install_folders = {"S:\\VisualStudio\\repos\\god-of-war-sons-of-sparta-pc"};
    const std::string raw =
        "Log: C:\\Users\\SauloSecretUser\\Documents\\save.bin\n"
        "Install: S:\\VisualStudio\\repos\\god-of-war-sons-of-sparta-pc\\boot.bin\n"
        "User SauloSecretUser on SPARTA-DEV-PC token=abc12345 ip=192.168.1.42 Vulkan 1.3.280\n";
    const std::string scrubbed = ScrubText(raw, facts);
    Check(scrubbed.find("SauloSecretUser") == std::string::npos, "username scrubbed");
    Check(scrubbed.find("SPARTA-DEV-PC") == std::string::npos, "machine name scrubbed");
    Check(scrubbed.find("abc12345") == std::string::npos, "secret token scrubbed");
    Check(scrubbed.find("192.168.1.42") == std::string::npos, "IPv4 scrubbed");
    Check(scrubbed.find("Vulkan 1.3.280") != std::string::npos, "Vulkan version preserved");
}

} // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / ("sos_crash_dump_tests_" + std::to_string(std::time(nullptr)));
    fs::remove_all(temp);
    fs::create_directories(temp);
    TestDumpNamingAndPath(temp);
    TestDumpValidationRules(temp);
    TestSameExecutionAndOptionalAttachment(temp);
    TestDeviceLostDiagnostics();
    TestScrubberPrivacy();
    std::error_code ec;
    fs::remove_all(temp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
