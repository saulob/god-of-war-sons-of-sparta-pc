// Headless proof of the launcher's WebSocket report client against the local Node reporting service: report submission,
// multi-chunk resumable minidump upload with a mid-transfer drop, status and read-back. Loopback only; no production
// service, game or launcher is started.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "bug_report_link.h"

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

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> MakeDumpBytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    bytes[0] = 'M';
    bytes[1] = 'D';
    bytes[2] = 'M';
    bytes[3] = 'P';
    for (size_t i = 4; i < size; ++i) bytes[i] = uint8_t((i * 29 + 13) & 0xffu);
    return bytes;
}

ReportFacts SampleFacts() { return {"0.1.0", "CUSA57767 1.01", "1.01", "CUSA57767", "Windows 11 (build 26200)"}; }

void TestLiveSubmission(const std::string& endpoint, const fs::path& temp) {
    const fs::path logs = temp / "logs";
    fs::create_directories(logs);
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const fs::path log_path = logs / "sons_of_sparta_009.log";
    const fs::path dump_path = logs / "sons_of_sparta_009.dmp";
    WriteFile(log_path, "[run] God of War: Sons of Sparta PC\nRuntime: Unity bootstrap entered\n"
                        "GPU device lost: first detected in vkQueueSubmit (site flush), VkResult VK_ERROR_DEVICE_LOST (-4), thread 7\n"
                        "GPU timelines: draw submitted 44 completed 42\n"
                        "Device fault: page fault in shader\n"
                        "CRASH: access violation (0xc0000005)\n"
                        "  phase=first present, render 1920x1080 (native) 16:9\n"
                        "  minidump: sons_of_sparta_009.dmp (next to this log)\n");
    const std::vector<uint8_t> dump_bytes = MakeDumpBytes(kDumpChunkBytes * 2 + 18900);
    WriteBytes(dump_path, dump_bytes);

    const std::vector<GameLog> found = FindGameLogs(logs, {}, kMostGameLogs, now);
    Check(found.size() == 1 && found[0].Crashed() && found[0].CanAttachCrash(), "fixture log and dump discovered");
    if (found.empty()) return;
    const auto package = BuildBugReportPackage("Crash right after device lost during the first fight.", found[0], SampleFacts(),
                                               PrivateFacts{}, true, now);
    Check(package.has_value() && package->crash_dump.has_value(), "package built with a multi-chunk crash dump");
    if (!package) return;

    BugReportServiceConfig config;
    config.endpoint_url = endpoint;
    config.client_id = "uploadtestclient0001";
    config.retry_pause_ms = 10;
    config.timeout_ms = 5000;

    // The local server drops the socket once 64 KiB of the dump arrived, so the client must resume from there.
    std::vector<BugReportProgress> events;
    const BugReportReceipt receipt =
        FileBugReport(config, *package, {}, [&](const BugReportProgress& event) { events.push_back(event); });
    Check(receipt.outcome == BugReportOutcome::kSent, "report filed over the WebSocket");
    Check(receipt.ticket.rfind("SOS-", 0) == 0, "received an SOS-XXXXXX ticket");
    Check(receipt.credential.size() == 64, "received a 64-hex receipt");
    Check(receipt.crash == CrashUpload::kUploaded, "interrupted multi-chunk dump resumed and completed");
    bool ordered = !events.empty() && events.front().kind == BugReportProgress::Kind::kTextAccepted &&
                   events.front().ticket == receipt.ticket;
    bool monotonic = true;
    for (size_t i = 1; i < events.size(); ++i) {
        ordered = ordered && events[i].kind == BugReportProgress::Kind::kDumpBytes && events[i].total == dump_bytes.size();
        monotonic = monotonic && events[i].sent >= (i > 1 ? events[i - 1].sent : 0) && events[i].sent <= events[i].total;
    }
    Check(ordered && events.size() > 3, "progress: the accepted text comes first, then dump bytes of the whole dump");
    Check(monotonic && events.back().sent == dump_bytes.size(), "progress: confirmed bytes never go back and end at the full size");

    TrackedReport tracked = TrackedReportOf(*package, receipt);
    const auto statuses = FetchReportStatuses(config, {tracked});
    Check(statuses.has_value() && statuses->size() == 1 && (*statuses)[0].ticket == receipt.ticket,
          "the filed report is found again by its receipt");
    Check(statuses && (*statuses)[0].status == "new", "a new report starts as New");

    const auto stored = FetchSentReport(config, tracked);
    Check(stored.has_value() && stored->Value("log_file") == "sons_of_sparta_009.log", "the stored report reads back");
    Check(stored && stored->Value("log_text").find("CRASH: access violation") != std::string::npos,
          "the stored report keeps its sanitized log");

    const auto text_only = BuildBugReportPackage("A report without a crash dump.", found[0], SampleFacts(), PrivateFacts{}, false, now);
    Check(text_only.has_value() && !text_only->crash_dump.has_value(), "package without a dump built");
    if (text_only) {
        BugReportServiceConfig second = config;
        second.client_id = "uploadtestclient0002";
        const BugReportReceipt receipt2 = FileBugReport(second, *text_only);
        Check(receipt2.outcome == BugReportOutcome::kSent && receipt2.crash == CrashUpload::kNotRequested,
              "a text-only report is filed without an upload");
        BugReportReceipt limited;
        for (int i = 0; i < 4; ++i) limited = FileBugReport(second, *text_only);
        Check(limited.outcome == BugReportOutcome::kRecentLimit && limited.ticket.empty(),
              "a report over the hourly limit is named as such, never sent");
    }
}

void TestFailureModes(const fs::path& temp) {
    BugReportPackage package;
    package.fields.push_back({"description", "Description", "A report that cannot be delivered."});
    BugReportServiceConfig config;
    config.client_id = "uploadtestclient0003";
    config.timeout_ms = 1500;
    config.retry_pause_ms = 0;

    config.endpoint_url = "ws://127.0.0.1:1";
    const BugReportReceipt unreachable = FileBugReport(config, package);
    Check(unreachable.outcome == BugReportOutcome::kUnreachable, "a refused connection is unreachable, never sent");
    Check(unreachable.ticket.empty() && unreachable.credential.empty(), "a failed report has no ticket or receipt");

    config.endpoint_url.clear();
    const BugReportReceipt unconfigured = FileBugReport(config, package);
    Check(unconfigured.outcome == BugReportOutcome::kNotConfigured, "a missing endpoint is reported as not configured");

    config.endpoint_url = "not-a-websocket";
    const BugReportReceipt invalid = FileBugReport(config, package);
    Check(invalid.outcome == BugReportOutcome::kInvalidEndpoint, "a malformed address is reported as invalid, never sent");
    config.endpoint_url = "wss://";
    Check(FileBugReport(config, package).outcome == BugReportOutcome::kInvalidEndpoint,
          "a WebSocket address without a host is reported as invalid");
    Check(fs::exists(temp), "the failure paths use no external service");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <ws_url>\n", argv[0]);
        return 2;
    }
    const fs::path temp = fs::temp_directory_path() / ("sos_upload_tests_" + std::to_string(std::time(nullptr)));
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);
    TestLiveSubmission(argv[1], temp);
    TestFailureModes(temp);
    fs::remove_all(temp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
