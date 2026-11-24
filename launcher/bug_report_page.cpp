#include "bug_report_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

#include "imgui.h"
#include "launcher_escape.h"
#include "localization/ui_text.h"
#include "trg/platform.h"
#include "utf8.h"

namespace sos {
namespace {

constexpr float kDescriptionHeight = 112.0f;
constexpr float kPreviewHeight = 280.0f;
constexpr float kAlertTint = 0.16f, kAlertFlashTint = 0.5f;
constexpr float kAlertFlashSeconds = 0.9f;
constexpr char kDot[] = " \xC2\xB7 ";

const char* kCrashDumpHelp =
    "Includes the crash dump to help diagnose the problem. A crash dump may contain process memory used for diagnostics.";
const char* kCrashDumpUnavailable = "The crash dump of this session cannot be sent, so only the log is.";
const char* kPrivacyHeadline = "No personal user data is intentionally included.";
const char* kPrivacyNote =
    "Before anything is sent, the launcher removes addresses, user and computer names, private folders and secrets from "
    "the selected log. Automatic filtering cannot promise to catch everything, so open the preview if you want to check "
    "what goes out.";

int KeepDescriptionShort(ImGuiInputTextCallbackData* data) {
    const size_t keep = BytesOfCharacters(std::string_view(data->Buf, size_t(data->BufTextLen)), kMostDescriptionCharacters);
    if (keep < size_t(data->BufTextLen)) data->DeleteChars(int(keep), data->BufTextLen - int(keep));
    return 0;
}

std::string LogDetails(const GameLog& log) { return LogDateText(log.written) + kDot + LogSizeText(log.bytes); }

void AccentText(trg::Ui& ui, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().accent);
    ImGui::TextUnformatted(Tr(text));
    ImGui::PopStyleColor();
}

void LogStatusLine(trg::Ui& ui, const GameLog& log) {
    const char* role = LogRoleText(log.role);
    if (!role && !log.Crashed()) return;
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    if (role) AccentText(ui, role);
    if (log.Crashed()) {
        if (role) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
        ImGui::TextUnformatted(Tr("Crash detected"));
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
}

ImVec4 AlertedColor(const ImVec4& color, const ImVec4& alert, double seconds) {
    const float flash = std::max(0.0f, 1.0f - float(seconds) / kAlertFlashSeconds);
    const float t = kAlertTint + (kAlertFlashTint - kAlertTint) * flash;
    return ImVec4(color.x + (alert.x - color.x) * t, color.y + (alert.y - color.y) * t,
                  color.z + (alert.z - color.z) * t, color.w);
}

void WarningText(trg::Ui& ui, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
    ImGui::TextWrapped("%s", Tr(text));
    ImGui::PopStyleColor();
}

} // namespace

BugReportPage::BugReportPage(Places places)
    : places_(std::move(places)), my_reports_(places_.user_dir / kReportHistoryFile, places_.user_dir) {}

void BugReportPage::Draw(trg::Ui& ui, const ReportFacts& facts) {
    if (!logs_found_) FindLogs();
    Collect();
    const bool sending = session_.busy();
    if (CheckDraft(description_, !logs_.empty()) != problem_) problem_ = DraftProblem::kNone;
    AccentText(ui, kPrivacyHeadline);
    ui.Paragraph(kPrivacyNote);
    if (my_reports_.HasUpdates()) {
        const bool reply = my_reports_.HasNewReply();
        if (ui.ButtonRow(reply ? "A report has a new reply" : "A report has an update",
                         reply ? "The developer replied to one of your reports."
                               : "Its status changed since you last looked.",
                         "Show")) {
            my_reports_.ShowUpdate();
        }
    }
    form_top_ = ImGui::GetCursorPosY();
    ui.Heading("1. Report the problem");
    DrawDescription(ui, sending);
    ui.Heading("2. Choose one game log");
    DrawLogs(ui, sending);
    ui.Heading("3. Preview what will be sent");
    DrawPreview(ui, facts, sending);
    ui.Heading("4. Send");
    DrawSend(ui, facts, sending);
    ui.EndRows();
    my_reports_.Draw(ui, facts);
    ui.EndRows();
    if (my_reports_.TakeNewReportRequest()) ImGui::SetScrollY(form_top_);
}

void BugReportPage::FindLogs() {
    logs_ = FindGameLogs(places_.logs_dir, places_.running_log);
    logs_found_ = true;
    SelectLog(DefaultGameLog(logs_));
}

void BugReportPage::Collect() {
    const Collected collected = session_.Collect();
    if (collected == Collected::kNothing) return;
    bring_result_into_view_ = true;
    if (collected == Collected::kDelivery) {
        const auto& sent = session_.Result();
        if (sent && sent->outcome == BugReportOutcome::kSent && sent_package_) {
            sent_report_remembered_ = my_reports_.Remember(*sent_package_, *sent);
        }
    }
    const ReportPhase phase = session_.View().phase;
    if (collected == Collected::kDelivery && (phase == ReportPhase::kSent || phase == ReportPhase::kSentDumpFailed)) {
        description_[0] = '\0';
        ForgetPreview();
    }
}

void BugReportPage::ForgetPreview() {
    previewed_.reset();
    preview_text_.clear();
    log_failed_ = false;
}

bool BugReportPage::AcceptDraft() {
    problem_ = CheckDraft(description_, !logs_.empty());
    problem_time_ = ImGui::GetTime();
    bring_problem_into_view_ = problem_ != DraftProblem::kNone;
    return problem_ == DraftProblem::kNone;
}

std::shared_ptr<const BugReportPackage> BugReportPage::BuildPackage(const ReportFacts& facts) {
    log_failed_ = false;
    const PrivateFacts private_facts = GatherPrivateFacts(places_.install_folders);
    std::optional<BugReportPackage> package = BuildBugReportPackage(
        description_, logs_[selected_], facts, private_facts, attach_crash_report_ && logs_[selected_].CanAttachCrash());
    if (!package) {
        log_failed_ = true;
        bring_problem_into_view_ = true;
        FindLogs();
        return nullptr;
    }
    return std::make_shared<const BugReportPackage>(std::move(*package));
}

void BugReportPage::TogglePreview(const ReportFacts& facts) {
    if (previewed_) {
        ForgetPreview();
        return;
    }
    if (!AcceptDraft()) return;
    previewed_ = BuildPackage(facts);
    if (previewed_) preview_text_ = BugReportPreviewText(*previewed_);
}

void BugReportPage::Send(const ReportFacts& facts) {
    if (session_.busy() || !AcceptDraft()) return;
    std::shared_ptr<const BugReportPackage> package = previewed_ ? previewed_ : BuildPackage(facts);
    if (!package) return;
    sent_report_remembered_ = true;
    sent_package_ = package;
    if (session_.Submit(std::move(package), ResolveBugReportConfig(places_.user_dir))) bring_result_into_view_ = true;
}

void BugReportPage::RetryDump() {
    if (session_.RetryDump(ResolveBugReportConfig(places_.user_dir))) bring_result_into_view_ = true;
}

void BugReportPage::SelectLog(size_t index) {
    selected_ = index;
    attach_crash_report_ = !logs_.empty() && DefaultAttachCrashReport(logs_[selected_]);
}

void BugReportPage::DrawDescription(trg::Ui& ui, bool sending) {
    const bool too_short = problem_ == DraftProblem::kDescriptionTooShort;
    if (too_short && std::exchange(bring_problem_into_view_, false)) ImGui::SetKeyboardFocusHere();
    const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::BeginDisabled(sending);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                          too_short ? AlertedColor(frame, ui.theme().warn_dot, ImGui::GetTime() - problem_time_) : frame);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackEdit;
    if (ImGui::InputTextMultiline("##description", description_, sizeof(description_),
                                  ImVec2(-FLT_MIN, kDescriptionHeight * ui.scale()), flags, KeepDescriptionShort)) {
        ForgetPreview();
    }
    if (KeepTextOnEscape(description_, sizeof(description_), description_backup_, LauncherEscape().Pressed(), description_active_)) {
        ForgetPreview();
    }
    description_active_ = ImGui::IsItemActive();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    const size_t used = CountCharacters(description_);
    const std::string counter = std::to_string(used) + " / " + std::to_string(kMostDescriptionCharacters);
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    ImGui::TextColored(used >= kMostDescriptionCharacters ? ui.theme().warn_dot : ui.theme().dim, "%s", counter.c_str());
    if (too_short) {
        ImGui::SameLine();
        ImGui::TextColored(ui.theme().warn_dot, "%s", Tr("Please describe the problem, minimum 10 characters"));
    }
    ImGui::PopFont();
    ui.Help("What were you doing, what went wrong, and does it happen every time?");
}

void BugReportPage::DrawLogs(trg::Ui& ui, bool sending) {
    if (logs_.empty()) {
        ui.Paragraph("No game log is available yet, so a report cannot be sent. Start the game once and try again. With "
                     "Detailed logging off on the About page, only sessions that crashed keep a log.");
        if (problem_ == DraftProblem::kNoLog) {
            if (std::exchange(bring_problem_into_view_, false)) ImGui::SetScrollHereY();
            WarningText(ui, "There is no game log to send.");
        }
        return;
    }
    ui.Help("The newest logs of the game, most recent first. Only the one you choose is sent.");
    for (size_t i = 0; i < logs_.size(); ++i) {
        const std::string details = LogDetails(logs_[i]);
        ui.RowLeft(logs_[i].name.c_str(), details.c_str());
        LogStatusLine(ui, logs_[i]);
        ui.RowControl();
        ImGui::PushID(int(i));
        ImGui::BeginDisabled(sending);
        ImGui::AlignTextToFramePadding();
        if (ImGui::RadioButton(Tr("Send this log"), selected_ == i) && selected_ != i) {
            SelectLog(i);
            ForgetPreview();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ui.EndRows();
    if (log_failed_) {
        if (std::exchange(bring_problem_into_view_, false)) ImGui::SetScrollHereY();
        WarningText(ui, "That log could not be read. Choose another one and try again.");
    }
    const GameLog& log = logs_[selected_];
    if (ui.ButtonRow("Selected log", (log.name + kDot + LogDateText(log.written) + kDot + LogSizeText(log.bytes)).c_str(),
                     "View selected log")) {
        trg::OpenUrl(Utf8(log.path));
    }
    if (!log.diagnostics.last_startup_stage.empty()) {
        ui.Info("Last startup stage", log.diagnostics.last_startup_stage.c_str());
    }
    if (log.diagnostics.device_lost) {
        ui.Info("Vulkan device lost", log.diagnostics.device_lost_reason.c_str());
    }
    ui.EndRows();
    DrawCrashReport(ui, sending);
}

void BugReportPage::DrawCrashReport(trg::Ui& ui, bool sending) {
    const GameLog& log = logs_[selected_];
    if (!log.crash.has_value()) return;
    const bool attachable = log.CanAttachCrash();
    ImGui::BeginDisabled(sending || !attachable);
    if (ImGui::Checkbox(Tr("Attach crash report"), &attach_crash_report_)) ForgetPreview();
    ImGui::EndDisabled();
    ui.Help(attachable ? kCrashDumpHelp : kCrashDumpUnavailable);
}

void BugReportPage::DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ImGui::BeginDisabled(sending);
    const bool clicked = ui.ButtonRow("Preview", "Shows exactly what will be sent, after the privacy filter.",
                                      PreviewButtonText(previewed_ != nullptr));
    ImGui::EndDisabled();
    if (clicked) TogglePreview(facts);
    ui.EndRows();
    if (previewed_) {
        ImGui::InputTextMultiline("##preview", preview_text_.data(), preview_text_.size() + 1,
                                  ImVec2(-FLT_MIN, kPreviewHeight * ui.scale()),
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    }
}

void BugReportPage::DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ImGui::BeginDisabled(sending);
    const bool clicked = ui.ButtonRow("Send report", "Sends the selected log after the privacy filter is applied.",
                                      sending ? "Sending..." : "Send report", true);
    ImGui::EndDisabled();
    if (clicked) Send(facts);
    ui.EndRows();
    DrawResult(ui);
    if (std::exchange(bring_result_into_view_, false)) ImGui::SetScrollHereY(1.0f);
}

void BugReportPage::DrawResult(trg::Ui& ui) {
    const ReportView view = session_.View();
    const ReportCard card = CardFor(view, session_.DumpUploaded());
    if (card.tone == CardTone::kNone) return;
    ui.Spacer(4);
    const trg::Status status = card.tone == CardTone::kBusy ? trg::Status::kBusy
                               : card.tone == CardTone::kReady ? trg::Status::kReady : trg::Status::kAttention;
    ui.StatusCard(status, card.title, card.detail.c_str());
    if (card.copy_ticket) {
        ui.Spacer(4);
        if (ImGui::Button(Tr("Copy report ID"))) {
            ImGui::SetClipboardText(view.ticket.c_str());
            ui.SetStatus("Report ID copied.");
        }
        if (!sent_report_remembered_)
            ui.Help("The report was sent, but it could not be added to My reports on this PC.");
    }
    switch (card.dump) {
    case DumpLine::kNone:
        break;
    case DumpLine::kUploading:
        ui.Spacer(4);
        ui.StatusCard(trg::Status::kBusy, card.dump_text, "This can take a little longer than the report itself.",
                      card.dump_progress);
        break;
    case DumpLine::kUploaded:
        ui.Help(card.dump_text);
        break;
    case DumpLine::kFailed:
        WarningText(ui, card.dump_text);
        if (card.retry_dump && ImGui::Button(Tr("Retry crash report upload"))) RetryDump();
        break;
    }
}

} // namespace sos
