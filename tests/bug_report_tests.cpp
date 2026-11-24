// Headless checks of the Bug Report model and My reports history: the package, the request body, receipts, status
// vocabulary, feedback answers and the saved history. No window, network, game or launcher.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "bug_report_link.h"
#include "net/json.h"
#include "net/report_link.h"
#include "report_history.h"

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

ReportFacts Facts() { return {"0.1.0", "CUSA57767 1.01", "1.01", "CUSA57767", "Windows 11 (build 26200)"}; }

GameLog SampleLog(const fs::path& folder) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const fs::path path = folder / "sons_of_sparta_012.log";
    WriteFile(path, "[run] God of War: Sons of Sparta PC\nRuntime: Unity bootstrap entered\n"
                    "GPU: selected NVIDIA GeForce RTX 4070\nCRASH: access violation (0xc0000005)\n");
    return GameLog{path, "sons_of_sparta_012.log", now, fs::file_size(path), LogActivity::kGameplay, LogRole::kLastGameplay,
                   std::nullopt, DumpProblem::kMissing, {}};
}

void TestPackageAndRequest(const fs::path& temp) {
    const fs::path logs = temp / "package";
    fs::create_directories(logs);
    const GameLog log = SampleLog(logs);
    const auto package = BuildBugReportPackage("The game crashed after the first fight.", log, Facts(), PrivateFacts{});
    Check(package.has_value(), "a readable log builds a package");
    if (!package) return;
    Check(package->Value("description") == "The game crashed after the first fight.", "the description is the trimmed text");
    Check(package->Value("log_file") == "sons_of_sparta_012.log", "the selected log name is sent");
    Check(package->Value("cusa") == "CUSA57767", "the CUSA is sent");
    Check(package->Value("crash_detected") == "yes", "a crash line is detected");
    Check(package->Value("crash_kind") == "generic_crash", "the crash kind is classified");
    Check(!package->crash_dump.has_value() && !package->crash_dump_requested, "a dump is not attached without a request");

    const net::Json body = BugReportRequestBody(*package);
    Check(body["description"].as_string() == package->Value("description"), "the request body holds the previewed description");
    Check(body["log_text"].as_string() == package->Value("log_text"), "the request body holds the previewed log");
    Check(body["op"].is_null(), "the request body leaves the envelope to the link");
    Check(!body.has("dump_file"), "a package without a dump announces none");
    Check(BugReportRequestJson(*package) == body.dump(), "the serialized request is the same JSON");
    const std::string preview = BugReportPreviewText(*package);
    Check(preview.find(package->Value("log_text")) != std::string::npos,
          "the preview shows the same sanitized log the request carries");
    Check(preview.find(package->Value("description")) != std::string::npos,
          "the preview shows the same description the request carries");

    net::Json stored = net::Json::Object();
    for (const BugReportField& field : package->fields) stored.set(field.key, field.value);
    const BugReportPackage read = SentReportOf(stored);
    Check(read.Value("description") == package->Value("description"), "a stored report reads back as the previewed fields");
    Check(read.Value("log_text") == package->Value("log_text"), "a stored report keeps its log");
}

void TestReceiptsAndStatuses() {
    BugReportPackage package;
    package.fields.push_back({"description", "Description", "Crash on the first fight."});
    package.fields.push_back({"log_file", "Selected log", "sons_of_sparta_009.log"});
    package.fields.push_back({"launcher_version", "Launcher version", "0.1.0"});
    package.fields.push_back({"game_build", "Game build", "CUSA57767 1.01"});
    package.fields.push_back({"cusa", "CUSA", "CUSA57767"});
    package.fields.push_back({"operating_system", "Operating system", "Windows 11"});
    BugReportReceipt receipt;
    receipt.ticket = "SOS-ABC234";
    receipt.credential = std::string(64, 'a');
    receipt.submitted_at = 1700000000000;
    const TrackedReport tracked = TrackedReportOf(package, receipt);
    Check(tracked.ticket == receipt.ticket && tracked.receipt == receipt.credential, "a tracked report keeps the ticket and receipt");
    Check(tracked.description == "Crash on the first fight." && tracked.log_file == "sons_of_sparta_009.log",
          "a tracked report keeps the fields My reports shows");
    Check(tracked.status == "new" && tracked.status_updated_at == receipt.submitted_at, "a new report starts as New");

    std::vector<TrackedReport> reports(55, tracked);
    for (size_t i = 0; i < reports.size(); ++i) reports[i].ticket = "SOS-" + std::to_string(100000 + i);
    const net::Json request = IncidentStatusRequest(reports);
    Check(request["reports"].items().size() == kMostTrackedReports, "a status lookup sends at most 50 reports");
    Check(request["reports"].items().front()["ticket"].as_string() == reports.front().ticket, "the newest reports go first");
    Check(request["reports"].items().front()["receipt"].as_string() == receipt.credential, "each claim carries its receipt");

    const auto answer = net::Json::parse(
        "{\"reports\":[{\"ticket\":\"SOS-ABC234\",\"status\":\"fixed\",\"status_updated_at\":1700000001000,"
        "\"fixed_version\":\"0.1.1\",\"player_message\":\"Please try again.\",\"test_feedback\":\"fixed\"}]}");
    Check(answer.has_value(), "a status answer parses");
    if (!answer) return;
    const std::vector<ReportStatus> statuses = ReadReportStatuses(*answer);
    Check(statuses.size() == 1 && statuses[0].ticket == "SOS-ABC234", "the status answer names its report");
    Check(statuses[0].status == "fixed" && statuses[0].fixed_version == "0.1.1", "the fixed status keeps its version");
    Check(statuses[0].player_message == "Please try again.", "the developer message is read");
    Check(statuses[0].test_feedback == "fixed", "the player test answer is read");

    const auto unknown = net::Json::parse("{\"reports\":[{\"ticket\":\"SOS-ABC234\",\"status\":\"new\",\"test_feedback\":\"maybe\"}]}");
    Check(unknown && ReadReportStatuses(*unknown)[0].test_feedback.empty(), "an unknown test answer is ignored");

    Check(ReportStatusText("new") != nullptr && ReportStatusText("investigating") != nullptr &&
              ReportStatusText("awaiting_player_test") != nullptr && ReportStatusText("fixed") != nullptr &&
              ReportStatusText("cannot_reproduce") != nullptr && ReportStatusText("duplicate") != nullptr &&
              ReportStatusText("not_a_bug") != nullptr && ReportStatusText("closed") != nullptr,
          "the eight statuses have their names");
    Check(ReportStatusText("solved") == nullptr, "an unknown status has no name");
}

void TestFeedback() {
    Check(FeedbackOutcomeOf("ok") == FeedbackOutcome::kSaved, "an accepted answer is saved");
    Check(FeedbackOutcomeOf("stale_request") == FeedbackOutcome::kStale, "a stale request is distinguished");
    Check(FeedbackOutcomeOf("already_answered") == FeedbackOutcome::kStale, "a second answer is distinguished");
    Check(FeedbackOutcomeOf(net::kUnreachable) == FeedbackOutcome::kUnreachable, "an unreachable service is distinguished");
    Check(FeedbackOutcomeOf("internal") == FeedbackOutcome::kFailed, "a service error is a plain failure");

    const auto reply = net::Json::parse("{\"re\":1,\"code\":\"ok\",\"test_feedback\":\"fixed\"}");
    const FeedbackAnswer answer = FeedbackAnswerOf("ok", *reply, "fixed");
    Check(answer.outcome == FeedbackOutcome::kSaved, "a matching answer is accepted");
    const FeedbackAnswer mismatched = FeedbackAnswerOf("ok", *reply, "not_fixed");
    Check(mismatched.outcome == FeedbackOutcome::kFailed && mismatched.code == "malformed_reply",
          "an answer that does not confirm the request is refused");

    TrackedReport report;
    report.ticket = "SOS-ABC234";
    report.receipt = std::string(64, 'b');
    report.status = kAwaitingPlayerTest;
    report.status_updated_at = 1700000001000;
    const net::Json request = IncidentFeedbackRequest(report, kTestNotFixed, Facts());
    Check(request["ticket"].as_string() == report.ticket && request["receipt"].as_string() == report.receipt,
          "feedback carries the report's own receipt");
    Check(request["revision"].as_int() == report.status_updated_at, "feedback carries the open revision");
    Check(request["result"].as_string() == kTestNotFixed, "feedback carries the result");
}

void TestPlayerTest() {
    TrackedReport report;
    report.status = kAwaitingPlayerTest;
    report.status_updated_at = 100;
    Check(PlayerTestOf(report) == PlayerTest::kAwaitingAnswer, "an unanswered request asks the player");
    report.test_feedback = kTestFixed;
    report.test_feedback_revision = 100;
    Check(PlayerTestOf(report) == PlayerTest::kConfirmedFixed, "a fixed answer is confirmed");
    report.test_feedback = kTestNotFixed;
    Check(PlayerTestOf(report) == PlayerTest::kStillHappening, "a still-happening answer is shown");
    report.status_updated_at = 200;
    Check(PlayerTestOf(report) == PlayerTest::kAwaitingAnswer, "a newer request is open again");
    report.status = "fixed";
    Check(PlayerTestOf(report) == PlayerTest::kNone, "another status has no player test");
}

void TestHistory(const fs::path& temp) {
    const fs::path file = temp / "history" / kReportHistoryFile;
    ReportHistory history(file);
    history.Load();
    Check(history.reports().empty(), "a missing history starts empty");

    TrackedReport first;
    first.ticket = "SOS-ABC234";
    first.receipt = std::string(64, 'a');
    first.submitted_at = 100;
    first.description = "First report about a startup crash.";
    first.log_file = "sons_of_sparta_001.log";
    first.status_updated_at = 100;
    TrackedReport second = first;
    second.ticket = "SOS-DEF567";
    second.receipt = std::string(64, 'b');
    second.submitted_at = 200;
    second.description = "Second report about a device lost.";
    second.status_updated_at = 200;
    Check(history.Add(first), "a report is saved");
    Check(history.Add(second), "a second report is saved");
    Check(history.reports().size() == 2 && history.reports()[0].ticket == second.ticket, "the newest report is first");

    std::vector<ReportStatus> statuses = {{first.ticket, "awaiting_player_test", 300, "", "Please test 0.1.1.", "pending"}};
    Check(history.Apply(statuses, {first.ticket}), "a newer status is applied");
    Check(history.HasUnseen(), "a status change is unseen");
    Check(history.NextUnseen() == first.ticket, "the changed report is offered first");
    Check(history.MarkSeen(first.ticket), "opening the report marks it seen");
    Check(!history.HasUnseen(), "nothing is unseen after opening it");

    statuses = {{first.ticket, "awaiting_player_test", 300, "", "The developer replied.", "pending"}};
    Check(history.Apply(statuses), "a new developer message is applied");
    Check(history.HasUnseenMessage(), "a new developer message has its own mark");
    history.MarkSeen(first.ticket);

    Check(history.RecordTestFeedback(first.ticket, 300, kTestFixed), "an answer is recorded for the open request");
    Check(!history.RecordTestFeedback(first.ticket, 299, kTestNotFixed), "an answer for another revision is refused");
    Check(history.Save(), "the history saves");

    ReportHistory reloaded(file);
    reloaded.Load();
    Check(reloaded.reports().size() == 2, "the saved history loads");
    const TrackedReport& loaded = reloaded.reports()[1];
    Check(loaded.ticket == first.ticket && loaded.test_feedback == kTestFixed, "the saved answer survives a reload");
    Check(loaded.player_message == "The developer replied.", "the saved developer message survives");

    statuses = {{first.ticket, "fixed", 400, "0.1.1", "", ""}};
    Check(reloaded.Apply(statuses), "a fixed status is applied");
    Check(reloaded.reports()[1].fixed_version == "0.1.1", "the fixed version is kept");

    Check(reloaded.Remove(second.ticket), "a report is removed from this PC only");
    Check(reloaded.reports().size() == 1, "the removed report is gone from the list");

    WriteFile(file, "{not json");
    ReportHistory damaged(file);
    damaged.Load();
    Check(damaged.reports().empty(), "a damaged history starts empty");
    Check(fs::exists(fs::path(file.string() + ".damaged")), "a damaged history is set aside, not overwritten");
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value && *value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

void TestConfig(const fs::path& temp) {
    constexpr char kProduction[] = "wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws";
    const fs::path user = temp / "user";
    fs::create_directories(user);
    const fs::path override_file = user / "bug_report_url.txt";

    SetEnv("SOS_BUG_REPORT_URL", "");
    BugReportServiceConfig clean = ResolveBugReportConfig(user);
    Check(clean.endpoint_url == kProduction, "a clean installation selects the compiled production endpoint");
    Check(!clean.endpoint_url.empty(), "the compiled production endpoint is never empty");
    Check(clean.endpoint_url.find("DCP") == std::string::npos && clean.endpoint_url.find("dcp-") == std::string::npos,
          "the default endpoint is not another game's service");
    Check(clean.client_id.size() >= 16 && clean.client_id.size() <= 64, "a client id is made");
    Check(fs::exists(user / "bug_report_client.txt"), "the client id is kept in the user folder");

    SetEnv("SOS_BUG_REPORT_URL", "https://reports.example.test/ws/");
    BugReportServiceConfig env = ResolveBugReportConfig(user);
    Check(env.endpoint_url == "wss://reports.example.test/ws",
          "an https environment override becomes wss and loses its trailing slash");

    WriteFile(override_file, "ws://127.0.0.1:8795/\n");
    SetEnv("SOS_BUG_REPORT_URL", "");
    BugReportServiceConfig file = ResolveBugReportConfig(user);
    Check(file.endpoint_url == "ws://127.0.0.1:8795", "the user folder can hold a local override");

    SetEnv("SOS_BUG_REPORT_URL", "https://reports.example.test/ws/");
    Check(ResolveBugReportConfig(user).endpoint_url == "wss://reports.example.test/ws",
          "the environment override takes precedence over the file");

    SetEnv("SOS_BUG_REPORT_URL", "");
    BugReportServiceConfig stored = ResolveBugReportConfig(user);
    Check(stored.endpoint_url == "ws://127.0.0.1:8795", "the file override is used when the environment is unset");

    WriteFile(override_file, "   \r\n");
    BugReportServiceConfig blank_file = ResolveBugReportConfig(user);
    Check(blank_file.endpoint_url == kProduction, "an empty override file falls back to the compiled production default");

    fs::remove(override_file);
    SetEnv("SOS_BUG_REPORT_URL", "");
    BugReportServiceConfig none = ResolveBugReportConfig(user);
    Check(none.endpoint_url == kProduction, "removing both overrides returns to the compiled production default");
}

} // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / ("sos_bug_report_tests_" + std::to_string(std::time(nullptr)));
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);
    TestPackageAndRequest(temp);
    TestReceiptsAndStatuses();
    TestFeedback();
    TestPlayerTest();
    TestHistory(temp);
    TestConfig(temp);
    fs::remove_all(temp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
