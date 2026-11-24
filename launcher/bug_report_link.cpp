#include "bug_report_link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>

#include "net/report_link.h"
#include "net/websocket.h"
#include "utf8.h"

namespace sos {
namespace fs = std::filesystem;
namespace {

constexpr char kClientIdFile[] = "bug_report_client.txt";
constexpr char kEndpointFile[] = "bug_report_url.txt";
constexpr size_t kReceiptHexDigits = 64;
constexpr char kDefaultBugReportUrl[] = "wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws";
static_assert(sizeof(kDefaultBugReportUrl) > 1, "the production report endpoint must be configured");
static_assert(std::string_view(kDefaultBugReportUrl).ends_with("/ws"), "the report endpoint must be the Worker /ws route");
// "DI", version 1, chunk: server/bug-report/src/incidents.js DUMP_FRAME.
constexpr uint8_t kDumpFrameHeader[] = {0x44, 0x49, 1, 1};

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() &&
           (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
        ++start;
    return text.substr(start);
}

std::string NormalizeEndpoint(std::string url) {
    url = Trim(std::move(url));
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.rfind("https://", 0) == 0) return "wss://" + url.substr(8);
    if (url.rfind("http://", 0) == 0) return "ws://" + url.substr(7);
    return url;
}

std::string ReadFirstLine(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) return Trim(line);
    return {};
}

bool IsReceiptHex(std::string_view text) {
    return text.size() == kReceiptHexDigits &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string LoadOrCreateClientId(const fs::path& user_dir) {
    const auto valid = [](const std::string& id) {
        return id.size() >= 16 && id.size() <= 64 &&
               std::all_of(id.begin(), id.end(),
                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    const fs::path path = user_dir / kClientIdFile;
    {
        std::ifstream in(path);
        std::string stored;
        if (std::getline(in, stored) && valid(stored)) return stored;
    }
    std::random_device rd;
    char hex[33] = {};
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", rd() & 0xffu);
    std::string generated(hex);
    std::error_code ec;
    fs::create_directories(user_dir, ec);
    std::ofstream out(path, std::ios::trunc);
    if (out) out << generated << "\n";
    return generated;
}

std::string EffectiveEndpoint(const BugReportServiceConfig& config, const std::string& override) {
    return NormalizeEndpoint(override.empty() ? config.endpoint_url : override);
}

bool IsValidEndpoint(const std::string& url) {
    net::WsUrl parsed;
    return !url.empty() && net::parse_ws_url(url, parsed, nullptr);
}

std::string JsonStringField(const net::Json& obj, std::string_view key) {
    return obj[key].as_string();
}

int64_t JsonIntField(const net::Json& obj, std::string_view key, int64_t fallback = 0) {
    const net::Json& value = obj[key];
    return value.is_number() ? value.as_int(fallback) : fallback;
}

BugReportOutcome OutcomeOfCode(const std::string& code) {
    if (code == "ok") return BugReportOutcome::kSent;
    if (code == "incident_hourly_limit" || code == "limit") return BugReportOutcome::kRecentLimit;
    if (code == "incident_daily_limit") return BugReportOutcome::kDailyLimit;
    if (code == "bad_request" || code == "too_large") return BugReportOutcome::kRefused;
    if (code == net::kLinkLost || code == net::kUnreachable || code == "timeout") return BugReportOutcome::kUnreachable;
    return BugReportOutcome::kServiceError;
}

BugReportReceipt NotSent(BugReportOutcome outcome) {
    BugReportReceipt receipt;
    receipt.outcome = outcome;
    return receipt;
}

net::Json ReceiptClaim(const std::string& ticket, const std::string& receipt) {
    net::Json claim = net::Json::Object();
    claim.set("ticket", ticket);
    claim.set("receipt", receipt);
    return claim;
}

enum class DumpStep { kDone, kRetry, kStop };

// One pass on an open connection: ask where the service's copy ends, send the rest in chunks read from the file, close
// the upload.
DumpStep SendDumpOnce(net::ReportSession& session, const net::Json& claim, const CrashDump& dump,
                      const BugReportProgressSink& progress) {
    const auto report = [&](uint64_t sent) {
        if (progress) progress({BugReportProgress::Kind::kDumpBytes, claim["ticket"].as_string(), sent, dump.bytes});
    };
    std::error_code ec;
    if (fs::file_size(dump.path, ec) != dump.bytes || ec) return DumpStep::kStop;
    std::string code;
    const std::optional<net::Json> begun = session.call("incident.dump", claim, &code);
    if (!begun) return code == "not_found" || code == "bad_request" || code == "too_large" ? DumpStep::kStop : DumpStep::kRetry;
    if ((*begun)["bytes"].as_u64() != dump.bytes) return DumpStep::kStop;
    if ((*begun)["complete"].as_bool()) return DumpStep::kDone;
    uint64_t at = (*begun)["stored"].as_u64();
    if (at > dump.bytes) return DumpStep::kStop;
    std::ifstream in(dump.path, std::ios::binary);
    if (!in) return DumpStep::kStop;
    in.seekg(std::streamoff(at));
    constexpr size_t kHeader = sizeof(kDumpFrameHeader);
    std::vector<uint8_t> frame(kHeader + kDumpChunkBytes);
    std::copy(std::begin(kDumpFrameHeader), std::end(kDumpFrameHeader), frame.begin());
    while (at < dump.bytes) {
        const size_t want = size_t(std::min<uint64_t>(kDumpChunkBytes, dump.bytes - at));
        in.read(reinterpret_cast<char*>(frame.data() + kHeader), std::streamsize(want));
        if (size_t(in.gcount()) != want) return DumpStep::kStop;
        if (!session.send_binary(frame.data(), kHeader + want)) return DumpStep::kRetry;
        at += want;
        report(at);
    }
    if (session.call("incident.dump_end", claim, &code)) return DumpStep::kDone;
    return code == "bad_request" || code == "not_found" ? DumpStep::kStop : DumpStep::kRetry;
}

CrashUpload UploadCrashDumpOn(const BugReportServiceConfig& config, const std::string& endpoint, net::ReportSession* open,
                              const std::string& ticket, const std::string& receipt, const CrashDump& dump,
                              const BugReportProgressSink& progress) {
    const net::Json claim = ReceiptClaim(ticket, receipt);
    net::ReportSession own;
    net::ReportSession* session = open;
    uint64_t high_water = 0;
    const BugReportProgressSink note = [&](const BugReportProgress& event) {
        if (event.kind == BugReportProgress::Kind::kTextAccepted) {
            if (progress) progress(event);
            return;
        }
        high_water = std::max(high_water, event.sent);
        if (progress) progress({BugReportProgress::Kind::kDumpBytes, event.ticket, high_water, event.total});
    };
    for (int attempt = 0; attempt < kDumpUploadAttempts; ++attempt) {
        if (!session || !session->connected()) {
            if (attempt > 0 && config.retry_pause_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(config.retry_pause_ms * attempt));
            std::string error;
            if (!own.connect(endpoint, &error, config.timeout_ms, config.client_id)) continue;
            session = &own;
        }
        const DumpStep step = SendDumpOnce(*session, claim, dump, note);
        if (step == DumpStep::kDone) {
            std::printf("Bug report: crash report %s uploaded for %s\n", dump.name.c_str(), ticket.c_str());
            return CrashUpload::kUploaded;
        }
        session->close();
        if (step == DumpStep::kStop) break;
    }
    std::printf("Bug report: crash report %s was not uploaded for %s\n", dump.name.c_str(), ticket.c_str());
    return CrashUpload::kFailed;
}

} // namespace

BugReportServiceConfig ResolveBugReportConfig(const fs::path& user_dir) {
    BugReportServiceConfig config;
    const char* env = std::getenv("SOS_BUG_REPORT_URL");
    const std::string from_env = env && *env ? NormalizeEndpoint(env) : std::string();
    std::string from_file;
    if (!user_dir.empty()) from_file = NormalizeEndpoint(ReadFirstLine(user_dir / kEndpointFile));
    config.endpoint_url = !from_env.empty() ? from_env : (!from_file.empty() ? from_file : NormalizeEndpoint(kDefaultBugReportUrl));
    config.client_id = user_dir.empty() ? std::string() : LoadOrCreateClientId(user_dir);
    return config;
}

BugReportReceipt FileBugReport(const BugReportServiceConfig& config, const BugReportPackage& package,
                               const std::string& endpoint_override, const BugReportProgressSink& progress) {
    const std::string endpoint = EffectiveEndpoint(config, endpoint_override);
    if (endpoint.empty()) return NotSent(BugReportOutcome::kNotConfigured);
    if (!IsValidEndpoint(endpoint)) return NotSent(BugReportOutcome::kInvalidEndpoint);

    net::ReportSession session;
    std::string error, code;
    if (!session.connect(endpoint, &error, config.timeout_ms, config.client_id)) {
        std::printf("Bug report: no session: %s\n", error.c_str());
        return NotSent(BugReportOutcome::kUnreachable);
    }
    const std::optional<net::Json> answer = session.call("incident.file", BugReportRequestBody(package), &code);
    if (!answer) {
        session.close();
        std::printf("Bug report: not accepted (%s)\n", code.c_str());
        return NotSent(OutcomeOfCode(code));
    }
    const std::string ticket = JsonStringField(*answer, "ticket");
    if (ticket.empty()) {
        session.close();
        return NotSent(BugReportOutcome::kServiceError);
    }
    std::printf("Bug report: sent as %s\n", ticket.c_str());
    const std::string credential = JsonStringField(*answer, "receipt");
    BugReportReceipt receipt{BugReportOutcome::kSent, ticket, IsReceiptHex(credential) ? credential : std::string(),
                            JsonIntField(*answer, "submitted_at")};
    if (progress) progress({BugReportProgress::Kind::kTextAccepted, ticket, 0, 0});
    if (package.crash_dump) {
        receipt.crash = receipt.credential.empty() || JsonStringField(*answer, "dump") != "expected"
                            ? CrashUpload::kFailed
                            : UploadCrashDumpOn(config, endpoint, &session, ticket, receipt.credential, *package.crash_dump,
                                                progress);
    } else if (package.crash_dump_requested) {
        receipt.crash = CrashUpload::kFailed;
    }
    session.close();
    return receipt;
}

CrashUpload UploadCrashDump(const BugReportServiceConfig& config, const std::string& ticket, const std::string& receipt,
                            const CrashDump& dump, const std::string& endpoint_override,
                            const BugReportProgressSink& progress) {
    const std::string endpoint = EffectiveEndpoint(config, endpoint_override);
    if (endpoint.empty() || !IsValidEndpoint(endpoint) || ticket.empty() || !IsReceiptHex(receipt))
        return CrashUpload::kFailed;
    return UploadCrashDumpOn(config, endpoint, nullptr, ticket, receipt, dump, progress);
}

std::optional<std::vector<ReportStatus>> FetchReportStatuses(const BugReportServiceConfig& config,
                                                             const std::vector<TrackedReport>& reports) {
    const std::string endpoint = EffectiveEndpoint(config, {});
    if (endpoint.empty() || !IsValidEndpoint(endpoint)) return std::nullopt;
    net::ReportSession session;
    std::string error, code;
    if (!session.connect(endpoint, &error, config.timeout_ms, config.client_id)) return std::nullopt;
    const std::optional<net::Json> answer = session.call("incident.status", IncidentStatusRequest(reports), &code);
    session.close();
    if (!answer || !(*answer)["reports"].is_array()) return std::nullopt;
    return ReadReportStatuses(*answer);
}

std::optional<BugReportPackage> FetchSentReport(const BugReportServiceConfig& config, const TrackedReport& report) {
    const std::string endpoint = EffectiveEndpoint(config, {});
    if (endpoint.empty() || !IsValidEndpoint(endpoint)) return std::nullopt;
    net::ReportSession session;
    std::string error, code;
    if (!session.connect(endpoint, &error, config.timeout_ms, config.client_id)) return std::nullopt;
    const std::optional<net::Json> answer =
        session.call("incident.read", ReceiptClaim(report.ticket, report.receipt), &code);
    session.close();
    if (!answer || !(*answer)["report"].is_object()) return std::nullopt;
    return SentReportOf((*answer)["report"]);
}

FeedbackOutcome FeedbackOutcomeOf(const std::string& code) {
    if (code == "ok") return FeedbackOutcome::kSaved;
    if (code == "stale_request" || code == "already_answered" || code == "not_found") return FeedbackOutcome::kStale;
    if (code == net::kLinkLost || code == net::kUnreachable || code == "timeout") return FeedbackOutcome::kUnreachable;
    return FeedbackOutcome::kFailed;
}

FeedbackAnswer FeedbackAnswerOf(const std::string& code, const net::Json& reply, std::string_view requested) {
    constexpr size_t kMostDetailBytes = 80;
    FeedbackAnswer answer;
    answer.stage = "call";
    answer.code = code;
    answer.detail = reply["detail"].as_string().substr(0, kMostDetailBytes);
    answer.outcome = FeedbackOutcomeOf(code);
    if (answer.outcome == FeedbackOutcome::kSaved && reply["test_feedback"].as_string() != requested) {
        answer.outcome = FeedbackOutcome::kFailed;
        answer.code = "malformed_reply";
        answer.detail = "test_feedback";
    }
    return answer;
}

net::Json IncidentFeedbackRequest(const TrackedReport& report, std::string_view result, const ReportFacts& facts) {
    net::Json body = ReceiptClaim(report.ticket, report.receipt);
    body.set("revision", report.status_updated_at);
    body.set("result", std::string(result));
    body.set("launcher_version", facts.launcher_version);
    body.set("game_build", facts.game_build);
    return body;
}

FeedbackAnswer SendTestFeedback(const BugReportServiceConfig& config, const TrackedReport& report, std::string_view result,
                                const ReportFacts& facts) {
    const std::string endpoint = EffectiveEndpoint(config, {});
    if (endpoint.empty()) return {FeedbackOutcome::kUnreachable, "connect", "not_configured", ""};
    if (!IsValidEndpoint(endpoint)) return {FeedbackOutcome::kFailed, "connect", "invalid_endpoint", ""};
    net::ReportSession session;
    std::string error, code;
    if (!session.connect(endpoint, &error, config.timeout_ms, config.client_id))
        return {FeedbackOutcome::kUnreachable, "connect", net::kUnreachable, error};
    session.call("incident.feedback", IncidentFeedbackRequest(report, result, facts), &code);
    const FeedbackAnswer answer = FeedbackAnswerOf(code, session.last_reply(), result);
    session.close();
    return answer;
}

TrackedReport TrackedReportOf(const BugReportPackage& package, const BugReportReceipt& receipt) {
    TrackedReport report;
    report.ticket = receipt.ticket;
    report.receipt = receipt.credential;
    report.submitted_at = receipt.submitted_at;
    report.status_updated_at = receipt.submitted_at;
    report.description = package.Value("description");
    report.log_file = package.Value("log_file");
    report.launcher_version = package.Value("launcher_version");
    report.game_build = package.Value("game_build");
    report.cusa = package.Value("cusa");
    report.operating_system = package.Value("operating_system");
    return report;
}

net::Json IncidentStatusRequest(const std::vector<TrackedReport>& reports) {
    net::Json list = net::Json::Array();
    for (size_t i = 0; i < std::min(reports.size(), kMostTrackedReports); ++i) {
        net::Json claim = net::Json::Object();
        claim.set("ticket", reports[i].ticket);
        claim.set("receipt", reports[i].receipt);
        list.push(std::move(claim));
    }
    net::Json body = net::Json::Object();
    body.set("reports", std::move(list));
    return body;
}

std::vector<ReportStatus> ReadReportStatuses(const net::Json& answer) {
    std::vector<ReportStatus> statuses;
    if (!answer["reports"].is_array()) return statuses;
    for (const net::Json& entry : answer["reports"].items()) {
        const std::string& feedback = entry["test_feedback"].as_string();
        statuses.push_back({entry["ticket"].as_string(), entry["status"].as_string(), entry["status_updated_at"].as_int(),
                            entry["fixed_version"].as_string(), entry["player_message"].as_string(),
                            feedback == kTestFixed || feedback == kTestNotFixed ? feedback : std::string()});
    }
    return statuses;
}

} // namespace sos
