#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bug_report.h"
#include "net/json.h"
#include "report_history.h"

namespace sos {

// What the service has confirmed so far: the report text (with its ticket), then the crash dump bytes it holds.
struct BugReportProgress {
    enum class Kind { kTextAccepted, kDumpBytes };
    Kind kind = Kind::kTextAccepted;
    std::string ticket;
    uint64_t sent = 0;
    uint64_t total = 0;
};
using BugReportProgressSink = std::function<void(const BugReportProgress&)>;

enum class BugReportOutcome {
    kSent,
    kRecentLimit,
    kDailyLimit,
    kUnreachable,
    kRefused,
    kServiceError,
    kNotConfigured,
    kInvalidEndpoint,
};
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
    int timeout_ms = 10000;
};

inline const char* BugReportLimitTitle(BugReportOutcome outcome) {
    if (outcome == BugReportOutcome::kRecentLimit) return "Too many reports were sent recently";
    if (outcome == BugReportOutcome::kDailyLimit) return "Daily report limit reached";
    return nullptr;
}

// The address comes from SOS_BUG_REPORT_URL, else bug_report_url.txt in the user folder, else the compiled
// production default. Only a build without all three reports it as not configured.
BugReportServiceConfig ResolveBugReportConfig(const std::filesystem::path& user_dir);

// Blocks: run it off the UI thread. A missing endpoint is kNotConfigured and a malformed one kInvalidEndpoint,
// never a false success.
BugReportReceipt FileBugReport(const BugReportServiceConfig& config, const BugReportPackage& package,
                               const std::string& endpoint_override = {},
                               const BugReportProgressSink& progress = nullptr);

CrashUpload UploadCrashDump(const BugReportServiceConfig& config, const std::string& ticket, const std::string& receipt,
                            const CrashDump& dump, const std::string& endpoint_override = {},
                            const BugReportProgressSink& progress = nullptr);

// All block. nullopt when the service could not answer; FetchSentReport also for a report the service no longer has.
std::optional<std::vector<ReportStatus>> FetchReportStatuses(const BugReportServiceConfig& config,
                                                             const std::vector<TrackedReport>& reports);
std::optional<BugReportPackage> FetchSentReport(const BugReportServiceConfig& config, const TrackedReport& report);

enum class FeedbackOutcome { kSaved, kStale, kUnreachable, kFailed };

struct FeedbackAnswer {
    FeedbackOutcome outcome = FeedbackOutcome::kFailed;
    std::string stage;
    std::string code;
    std::string detail;
};

FeedbackOutcome FeedbackOutcomeOf(const std::string& code);
FeedbackAnswer FeedbackAnswerOf(const std::string& code, const net::Json& reply, std::string_view requested);
net::Json IncidentFeedbackRequest(const TrackedReport& report, std::string_view result, const ReportFacts& facts);
FeedbackAnswer SendTestFeedback(const BugReportServiceConfig& config, const TrackedReport& report, std::string_view result,
                                const ReportFacts& facts);

TrackedReport TrackedReportOf(const BugReportPackage& package, const BugReportReceipt& receipt);
net::Json IncidentStatusRequest(const std::vector<TrackedReport>& reports);
std::vector<ReportStatus> ReadReportStatuses(const net::Json& answer);

} // namespace sos
