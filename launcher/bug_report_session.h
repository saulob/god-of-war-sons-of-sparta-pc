#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "bug_report.h"
#include "bug_report_link.h"

namespace sos {

enum class ReportPhase { kIdle, kSendingText, kUploadingDump, kSent, kSentDumpFailed, kRetryingDump, kFailed };

// Everything the page may show. The service's receipt is not part of it.
struct ReportView {
    ReportPhase phase = ReportPhase::kIdle;
    BugReportOutcome failure = BugReportOutcome::kServiceError;
    std::string ticket;
    uint64_t dump_sent = 0;
    uint64_t dump_total = 0;
    bool busy = false;
    bool can_retry_dump = false;
};

struct SessionHooks {
    std::function<BugReportReceipt(const BugReportServiceConfig&, const BugReportPackage&, const BugReportProgressSink&)> file;
    std::function<CrashUpload(const BugReportServiceConfig&, const std::string&, const std::string&, const CrashDump&,
                              const BugReportProgressSink&)>
        upload;
    std::function<void(std::function<void()>)> run;
};

inline SessionHooks DefaultSessionHooks() {
    SessionHooks hooks;
    hooks.file = [](const BugReportServiceConfig& config, const BugReportPackage& package, const BugReportProgressSink& progress) {
        return FileBugReport(config, package, {}, progress);
    };
    hooks.upload = [](const BugReportServiceConfig& config, const std::string& ticket, const std::string& receipt,
                      const CrashDump& dump, const BugReportProgressSink& progress) {
        return UploadCrashDump(config, ticket, receipt, dump, {}, progress);
    };
    hooks.run = [](std::function<void()> job) { std::thread(std::move(job)).detach(); };
    return hooks;
}

enum class Collected { kNothing, kDelivery, kDumpRetry };

// One report at a time, delivered off the UI thread. A worker owns only its shared record, so a page that goes away
// while the service is answering leaves nothing dangling, and the answer is applied the next time the page collects.
class BugReportSession {
public:
    explicit BugReportSession(SessionHooks hooks = DefaultSessionHooks()) : hooks_(std::move(hooks)) {}

    bool busy() const { return delivery_ != nullptr || retry_ != nullptr; }

    bool Submit(std::shared_ptr<const BugReportPackage> package, BugReportServiceConfig config) {
        if (busy() || !package) {
            return false;
        }
        result_.reset();
        auto work = std::make_shared<Work>();
        work->package = package;
        delivery_ = work;
        hooks_.run([work, package = std::move(package), config = std::move(config), hooks = hooks_] {
            BugReportReceipt receipt = hooks.file(config, *package, [work](const BugReportProgress& event) { work->Note(event); });
            std::lock_guard lock(work->mutex);
            work->receipt = std::move(receipt);
            work->done = true;
        });
        return true;
    }

    bool CanRetryDump() const {
        return !busy() && result_ && result_->outcome == BugReportOutcome::kSent && result_->crash == CrashUpload::kFailed &&
               sent_dump_ && !result_->ticket.empty() && !result_->credential.empty();
    }

    bool RetryDump(BugReportServiceConfig config) {
        if (!CanRetryDump()) {
            return false;
        }
        auto work = std::make_shared<Work>();
        work->ticket = result_->ticket;
        work->text_accepted = true;
        retry_ = work;
        hooks_.run([work, config = std::move(config), ticket = result_->ticket, receipt = result_->credential,
                    dump = *sent_dump_, hooks = hooks_] {
            const CrashUpload outcome = hooks.upload(config, ticket, receipt, dump, [work](const BugReportProgress& event) { work->Note(event); });
            std::lock_guard lock(work->mutex);
            work->outcome = outcome;
            work->done = true;
        });
        return true;
    }

    Collected Collect() {
        if (delivery_ && delivery_->Finished()) {
            result_ = delivery_->receipt;
            sent_dump_ = delivery_->package ? delivery_->package->crash_dump : std::nullopt;
            delivery_.reset();
            return Collected::kDelivery;
        }
        if (retry_ && retry_->Finished()) {
            if (result_) {
                result_->crash = retry_->outcome;
            }
            retry_.reset();
            return Collected::kDumpRetry;
        }
        return Collected::kNothing;
    }

    ReportView View() const {
        ReportView view;
        view.busy = busy();
        view.can_retry_dump = CanRetryDump();
        if (delivery_ || retry_) {
            const std::shared_ptr<Work>& work = delivery_ ? delivery_ : retry_;
            std::lock_guard lock(work->mutex);
            view.ticket = work->text_accepted ? work->ticket : std::string();
            view.dump_sent = work->sent;
            view.dump_total = work->total;
            view.phase = retry_ ? ReportPhase::kRetryingDump : work->text_accepted ? ReportPhase::kUploadingDump : ReportPhase::kSendingText;
            return view;
        }
        if (!result_) {
            return view;
        }
        if (result_->outcome != BugReportOutcome::kSent) {
            view.phase = ReportPhase::kFailed;
            view.failure = result_->outcome;
            return view;
        }
        view.ticket = result_->ticket;
        view.phase = result_->crash == CrashUpload::kFailed ? ReportPhase::kSentDumpFailed : ReportPhase::kSent;
        return view;
    }

    bool DumpUploaded() const { return result_ && result_->crash == CrashUpload::kUploaded; }
    const std::optional<BugReportReceipt>& Result() const { return result_; }

private:
    struct Work {
        std::mutex mutex;
        bool done = false;
        bool text_accepted = false;
        std::string ticket;
        uint64_t sent = 0;
        uint64_t total = 0;
        BugReportReceipt receipt;
        CrashUpload outcome = CrashUpload::kFailed;
        std::shared_ptr<const BugReportPackage> package;

        void Note(const BugReportProgress& event) {
            std::lock_guard lock(mutex);
            if (event.kind == BugReportProgress::Kind::kTextAccepted) {
                text_accepted = true;
                ticket = event.ticket;
            } else {
                sent = event.sent;
                total = event.total;
            }
        }

        bool Finished() {
            std::lock_guard lock(mutex);
            return done;
        }
    };

    SessionHooks hooks_;
    std::shared_ptr<Work> delivery_;
    std::shared_ptr<Work> retry_;
    std::optional<BugReportReceipt> result_;
    std::optional<CrashDump> sent_dump_;
};

enum class CardTone { kNone, kBusy, kReady, kAttention };
enum class DumpLine { kNone, kUploading, kUploaded, kFailed };

// What the page draws for a view: one status card, then at most one line about the crash dump.
struct ReportCard {
    CardTone tone = CardTone::kNone;
    const char* title = "";
    std::string detail;
    bool copy_ticket = false;
    DumpLine dump = DumpLine::kNone;
    const char* dump_text = "";
    float dump_progress = -1.0f;
    bool retry_dump = false;
};

inline ReportCard CardFor(const ReportView& view, bool dump_uploaded) {
    ReportCard card;
    const std::string id = std::string("Report ID:") + " " + view.ticket;
    switch (view.phase) {
    case ReportPhase::kIdle:
        break;
    case ReportPhase::kSendingText:
        card.tone = CardTone::kBusy;
        card.title = "Sending your report...";
        card.detail = "This takes a few seconds.";
        break;
    case ReportPhase::kUploadingDump:
    case ReportPhase::kRetryingDump:
    case ReportPhase::kSent:
    case ReportPhase::kSentDumpFailed:
        card.tone = CardTone::kReady;
        card.title = "Report sent successfully";
        card.detail = id;
        card.copy_ticket = true;
        if (view.phase == ReportPhase::kUploadingDump || view.phase == ReportPhase::kRetryingDump) {
            card.dump = DumpLine::kUploading;
            card.dump_text = "Uploading the crash report...";
            card.dump_progress = view.dump_total > 0 ? static_cast<float>(view.dump_sent) / static_cast<float>(view.dump_total) : -1.0f;
        } else if (view.phase == ReportPhase::kSentDumpFailed) {
            card.dump = DumpLine::kFailed;
            card.dump_text = "The report was sent, but its crash report could not be uploaded.";
            card.retry_dump = view.can_retry_dump;
        } else if (dump_uploaded) {
            card.dump = DumpLine::kUploaded;
            card.dump_text = "The crash report was uploaded with it.";
        }
        break;
    case ReportPhase::kFailed:
        card.tone = CardTone::kAttention;
        switch (view.failure) {
        case BugReportOutcome::kRecentLimit:
        case BugReportOutcome::kDailyLimit:
            card.title = BugReportLimitTitle(view.failure);
            card.detail = "Please try again later";
            break;
        case BugReportOutcome::kUnreachable:
            card.title = "Cannot reach the report service";
            card.detail = "Check your connection and press Send report again; your text is kept.";
            break;
        case BugReportOutcome::kNotConfigured:
            card.title = "The report service is not configured";
            card.detail = "This build has no report service address. See docs/BUG_REPORT.md.";
            break;
        case BugReportOutcome::kInvalidEndpoint:
            card.title = "The report service address is invalid";
            card.detail = "Check user/bug_report_url.txt or SOS_BUG_REPORT_URL; see docs/BUG_REPORT.md.";
            break;
        case BugReportOutcome::kRefused:
            card.title = "The report was not accepted";
            card.detail = "Change the description or choose another log, then send it again.";
            break;
        default:
            card.title = "The report service could not take the report";
            card.detail = "This is not your fault. Try again in a few minutes; your text is kept.";
            break;
        }
        break;
    }
    return card;
}

} // namespace sos
