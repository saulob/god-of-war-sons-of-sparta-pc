#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "bug_report.h"

namespace sos {

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    int timeout_ms = 15000;
};

struct HttpResponse {
    bool transport_ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;

// What the service has confirmed so far: the report text (with its ticket), then the crash dump bytes it holds.
struct BugReportProgress {
    enum class Kind { kTextAccepted, kDumpBytes };
    Kind kind = Kind::kTextAccepted;
    std::string ticket;
    uint64_t sent = 0;
    uint64_t total = 0;
};
using BugReportProgressSink = std::function<void(const BugReportProgress&)>;

enum class BugReportOutcome { kSent, kRecentLimit, kDailyLimit, kUnreachable, kRefused, kServiceError };
enum class CrashUpload { kNotRequested, kUploaded, kFailed };

struct BugReportReceipt {
    BugReportOutcome outcome = BugReportOutcome::kServiceError;
    std::string ticket;
    std::string credential;
    int64_t submitted_at = 0;
    CrashUpload crash = CrashUpload::kNotRequested;
};

struct BugReportServiceConfig {
    std::string endpoint_url;
    std::string client_id;
    int retry_pause_ms = 250;
    int timeout_ms = 15000;
};

inline const char* BugReportLimitTitle(BugReportOutcome outcome) {
    if (outcome == BugReportOutcome::kRecentLimit) return "Too many reports were sent recently";
    if (outcome == BugReportOutcome::kDailyLimit) return "Daily report limit reached";
    return nullptr;
}
BugReportServiceConfig ResolveBugReportConfig(const std::filesystem::path& user_dir);
HttpResponse SendHttpRequest(const HttpRequest& request);

BugReportReceipt FileBugReport(const BugReportServiceConfig& config, const BugReportPackage& package,
                               const HttpTransport& transport = nullptr, const BugReportProgressSink& progress = nullptr);

CrashUpload UploadCrashDump(const BugReportServiceConfig& config, const std::string& ticket, const std::string& receipt,
                            const CrashDump& dump, const HttpTransport& transport = nullptr,
                            const BugReportProgressSink& progress = nullptr);

} // namespace sos
