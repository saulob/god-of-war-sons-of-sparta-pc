#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "bug_report_http.h"
#include "log_scrubber.h"

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

std::vector<uint8_t> MakeDumpBytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    bytes[0] = 'M';
    bytes[1] = 'D';
    bytes[2] = 'M';
    bytes[3] = 'P';
    for (size_t i = 4; i < size; ++i) bytes[i] = uint8_t((i * 29 + 13) & 0xffu);
    return bytes;
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

ReportFacts SampleFacts() {
    return {"0.1.0", "CUSA57767 1.01", "1.01", "CUSA57767", "Windows 11 (build 26200)"};
}

void TestLiveResumableUploadAndDashboardDownload(const std::string& base_url, const std::string& token,
                                                 const fs::path& temp) {
    const fs::path logs = temp / "logs";
    const fs::path user = temp / "user";
    fs::create_directories(logs);
    fs::create_directories(user);

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
    Check(found.size() == 1 && found[0].Crashed() && found[0].CanAttachCrash(), "fixture log + dump discovered");
    if (found.empty()) return;

    const PrivateFacts priv{};
    const auto pkg = BuildBugReportPackage("Crash right after device lost during boss fight.", found[0], SampleFacts(),
                                           priv, true, now);
    Check(pkg.has_value() && pkg->crash_dump.has_value(), "package built with multi-chunk crash dump");
    if (!pkg) return;

    BugReportServiceConfig config;
    config.endpoint_url = base_url;
    config.client_id = "uploadtestclient0001";
    config.retry_pause_ms = 10;

    // The local server is configured with SOS_DROP_CHUNK_OFFSET=65536 so chunk 2 drops once and must resume cleanly.
    std::vector<BugReportProgress> events;
    const BugReportReceipt receipt = FileBugReport(config, *pkg, nullptr, [&](const BugReportProgress& event) { events.push_back(event); });
    Check(receipt.outcome == BugReportOutcome::kSent, "report filed over HTTP");
    bool ordered = !events.empty() && events.front().kind == BugReportProgress::Kind::kTextAccepted && events.front().ticket == receipt.ticket;
    bool monotonic = true;
    for (size_t i = 1; i < events.size(); ++i) {
        ordered = ordered && events[i].kind == BugReportProgress::Kind::kDumpBytes && events[i].total == dump_bytes.size();
        monotonic = monotonic && events[i].sent >= (i > 1 ? events[i - 1].sent : 0) && events[i].sent <= events[i].total;
    }
    Check(ordered && events.size() > 3, "progress: the accepted text comes first, then dump bytes of the whole dump");
    Check(monotonic && events.back().sent == dump_bytes.size(), "progress: confirmed dump bytes never go back and end at the full size");
    Check(receipt.ticket.rfind("SOS-", 0) == 0, "received SOS-XXXXXX ticket");
    Check(receipt.credential.size() == 64, "received 64-hex receipt");
    Check(receipt.crash == CrashUpload::kUploaded, "interrupted multi-chunk dump resumed and completed");

    // Verify dashboard download returns the exact uploaded bytes.
    const HttpResponse dl = SendHttpRequest({"GET", base_url + "/admin/api/reports/" + receipt.ticket + "/dump",
                                             {{"Authorization", "Bearer " + token}}, {}, 10000});
    Check(dl.transport_ok && dl.status == 200, "dashboard download returned 200");
    Check(dl.body.size() == dump_bytes.size() &&
              std::memcmp(dl.body.data(), dump_bytes.data(), dump_bytes.size()) == 0,
          "dashboard minidump bytes match uploaded file byte-for-byte");

    // Report without dump succeeds and dashboard /dump returns 404.
    const auto pkg_no_dump = BuildBugReportPackage("Report without attaching crash dump.", found[0], SampleFacts(),
                                                   priv, false, now);
    Check(pkg_no_dump.has_value() && !pkg_no_dump->crash_dump.has_value(), "package without dump built");
    if (pkg_no_dump) {
        BugReportServiceConfig cfg2 = config;
        cfg2.client_id = "uploadtestclient0002";
        const BugReportReceipt r2 = FileBugReport(cfg2, *pkg_no_dump);
        Check(r2.outcome == BugReportOutcome::kSent && r2.crash == CrashUpload::kNotRequested,
              "text-only report filed without uploading dump");
        const HttpResponse no_dl = SendHttpRequest({"GET", base_url + "/admin/api/reports/" + r2.ticket + "/dump",
                                                    {{"Authorization", "Bearer " + token}}, {}, 10000});
        Check(no_dl.transport_ok && no_dl.status == 404, "report without dump returns 404 on dashboard /dump");
    }
}

void TestTextPreservedWhenDumpUploadFails(const fs::path& temp) {
    const fs::path logs = temp / "fail_dump";
    fs::create_directories(logs);
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const fs::path log_path = logs / "sons_of_sparta_010.log";
    const fs::path dump_path = logs / "sons_of_sparta_010.dmp";
    WriteFile(log_path, "[run] God of War: Sons of Sparta PC\nCRASH: access violation\n");
    WriteBytes(dump_path, MakeDumpBytes(2048));
    const auto found = FindGameLogs(logs, {}, kMostGameLogs, now);
    Check(found.size() == 1 && found[0].CanAttachCrash(), "failure-path fixture ready");
    if (found.empty()) return;
    const auto pkg = BuildBugReportPackage("Report whose dump upload fails initially.", found[0], SampleFacts(),
                                           PrivateFacts{}, true, now);
    Check(pkg.has_value(), "package built");
    if (!pkg) return;

    int dump_requests = 0;
    const HttpTransport fake = [&](const HttpRequest& req) -> HttpResponse {
        if (req.method == "POST" && req.url.ends_with("/api/reports")) {
            return {true, 201,
                    "{\"ticket\":\"SOS-234567\",\"receipt\":\"" + std::string(64, 'a') +
                        "\",\"submitted_at\":1700000000000,\"dump\":\"expected\",\"dump_chunk_bytes\":65536}",
                    ""};
        }
        ++dump_requests;
        return {false, 0, "", "simulated network down"};
    };
    BugReportServiceConfig cfg{"http://127.0.0.1:1", "client0000000001", 0, 1000};
    std::vector<BugReportProgress> events;
    const BugReportReceipt r = FileBugReport(cfg, *pkg, fake, [&](const BugReportProgress& event) { events.push_back(event); });
    Check(events.size() == 1 && events[0].kind == BugReportProgress::Kind::kTextAccepted && events[0].ticket == "SOS-234567",
          "progress: a failed dump upload still reported the accepted text and no dump bytes");
    Check(r.outcome == BugReportOutcome::kSent, "textual report preserved even when dump upload fails");
    Check(r.ticket == "SOS-234567", "ticket preserved on dump upload failure");
    Check(r.crash == CrashUpload::kFailed, "dump upload failure surfaced clearly");
    Check(dump_requests == kDumpUploadAttempts, "client retried dump upload bounded number of times");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <base_url> <dashboard_token>\n", argv[0]);
        return 2;
    }
    const fs::path temp = fs::temp_directory_path() / ("sos_upload_tests_" + std::to_string(std::time(nullptr)));
    fs::remove_all(temp);
    fs::create_directories(temp);
    TestLiveResumableUploadAndDashboardDownload(argv[1], argv[2], temp);
    TestTextPreservedWhenDumpUploadFails(temp);
    std::error_code ec;
    fs::remove_all(temp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
