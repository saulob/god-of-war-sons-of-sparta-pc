#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bug_report.h"
#include "bug_report_http.h"
#include "trg/launcher.h"

namespace sos {

class BugReportPage {
public:
    struct Places {
        std::filesystem::path logs_dir;
        std::filesystem::path running_log;
        std::filesystem::path user_dir;
        std::vector<std::filesystem::path> install_folders;
    };

    explicit BugReportPage(Places places);

    void Draw(trg::Ui& ui, const ReportFacts& facts);

private:
    struct Delivery {
        std::atomic<bool> done{false};
        BugReportReceipt receipt;
        std::shared_ptr<const BugReportPackage> package;
    };
    struct DumpRetry {
        std::atomic<bool> done{false};
        CrashUpload outcome = CrashUpload::kFailed;
    };

    void FindLogs();
    void CollectDelivery();
    void CollectDumpRetry();
    void RetryDump();
    void SelectLog(size_t index);
    void ForgetPreview();
    bool AcceptDraft();
    std::shared_ptr<const BugReportPackage> BuildPackage(const ReportFacts& facts);
    void TogglePreview(const ReportFacts& facts);
    void Send(const ReportFacts& facts);
    void DrawDescription(trg::Ui& ui, bool sending);
    void DrawLogs(trg::Ui& ui, bool sending);
    void DrawCrashReport(trg::Ui& ui, bool sending);
    void DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawResult(trg::Ui& ui);

    Places places_;
    char description_[kMostDescriptionCharacters * 4 + 1] = {};
    std::vector<GameLog> logs_;
    bool logs_found_ = false;
    size_t selected_ = 0;
    DraftProblem problem_ = DraftProblem::kNone;
    double problem_time_ = 0.0;
    bool bring_problem_into_view_ = false;
    std::shared_ptr<const BugReportPackage> previewed_;
    std::string preview_text_;
    bool log_failed_ = false;
    std::shared_ptr<Delivery> delivery_;
    std::shared_ptr<DumpRetry> dump_retry_;
    std::optional<CrashDump> sent_dump_;
    bool attach_crash_report_ = false;
    std::optional<BugReportReceipt> result_;
    bool bring_result_into_view_ = false;
};

} // namespace sos
