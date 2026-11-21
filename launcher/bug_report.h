#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "log_scrubber.h"

namespace sos {

struct GameBuild;

inline constexpr size_t kMostGameLogs = 5;
inline constexpr size_t kFewestDescriptionCharacters = 10;
inline constexpr size_t kMostDescriptionCharacters = 500;
inline constexpr size_t kMostLogBytes = 64 * 1024;
inline constexpr size_t kMostDumpBytes = 8 * 1024 * 1024;
inline constexpr size_t kFewestDumpBytes = 32;
inline constexpr size_t kDumpChunkBytes = 64 * 1024;
inline constexpr int kDumpPairingSeconds = 120;
inline constexpr int kDumpFutureGraceSeconds = 300;
inline constexpr int kDumpUploadAttempts = 3;

enum class LogActivity { kLauncher, kGameStarted, kGameplay };
enum class LogRole { kNone, kCurrentLauncher, kCurrentGameplay, kLastGameplay, kOtherGameplay, kOtherLauncher };
enum class DumpProblem { kNone, kMissing, kNotThisLog, kNotAFile, kEmpty, kTooLarge, kNotADump };
enum class CrashKind { kNone, kGenericCrash, kDeviceLost, kDeviceLostCrash, kManualWithPriorDeviceLost };

const char* CrashKindKey(CrashKind kind);
const char* CrashKindLabel(CrashKind kind);

struct CrashDump {
    std::filesystem::path path;
    std::string name;
    uintmax_t bytes = 0;
};

struct LogDiagnostics {
    bool crash_detected = false;
    CrashKind crash_kind = CrashKind::kNone;
    std::string last_startup_stage;
    std::string gpu_name;
    std::string logged_minidump_name;
    bool device_lost = false;
    std::string device_lost_reason;
    std::string device_lost_timelines;
    bool device_fault_available = false;
    std::string device_fault_status;
    std::string device_fault_summary;
};

struct GameLog {
    std::filesystem::path path;
    std::string name;
    std::time_t written = 0;
    uintmax_t bytes = 0;
    LogActivity activity = LogActivity::kLauncher;
    LogRole role = LogRole::kNone;
    std::optional<CrashDump> crash;
    DumpProblem crash_problem = DumpProblem::kMissing;
    LogDiagnostics diagnostics;

    bool Crashed() const { return crash.has_value() || diagnostics.crash_detected; }
    bool CanAttachCrash() const { return crash.has_value() && crash_problem == DumpProblem::kNone; }
};

std::vector<GameLog> FindGameLogs(const std::filesystem::path& folder, const std::filesystem::path& running_log,
                                  size_t most = kMostGameLogs, std::optional<std::time_t> now = std::nullopt);

// "sons_of_sparta_005.dmp" for "sons_of_sparta_005.log"; empty for any other name.
std::string DumpNameOf(std::string_view log_name);

// Central validation of a candidate minidump against its owning log.
DumpProblem CheckCrashDump(const GameLog& log, const std::filesystem::path& dump,
                           std::optional<std::time_t> now = std::nullopt);

// Extracts bounded crash, Vulkan device-lost (authoritative first root cause), fault data, and last startup stage.
LogDiagnostics AnalyzeLogDiagnostics(std::string_view log_text);

bool DefaultAttachCrashReport(const GameLog& log);
size_t DefaultGameLog(const std::vector<GameLog>& logs);

const char* LogRoleText(LogRole role);
const char* PreviewButtonText(bool preview_open);
std::string LogDateText(std::time_t written);
std::string LogSizeText(uintmax_t bytes);

size_t BytesOfCharacters(std::string_view text, size_t most);
size_t CountCharacters(std::string_view text);

enum class DraftProblem { kNone, kDescriptionTooShort, kNoLog };
DraftProblem CheckDraft(std::string_view description, bool has_log);

struct ReportFacts {
    std::string launcher_version;
    std::string game_build;
    std::string game_version;
    std::string cusa;
    std::string operating_system;
};

const GameBuild* SessionBuild(std::string_view log_head);
ReportFacts SessionReportFacts(const ReportFacts& facts, const GameLog& log);
std::string OperatingSystemText();

struct BugReportField {
    std::string_view key;
    std::string_view label;
    std::string value;
};

struct BugReportPackage {
    std::vector<BugReportField> fields;
    std::optional<CrashDump> crash_dump;
    bool crash_dump_requested = false;
    const std::string& Value(std::string_view key) const;
};

std::string LastBytes(std::string_view text, size_t most);

std::optional<BugReportPackage> BuildBugReportPackage(std::string_view description, const GameLog& log,
                                                      const ReportFacts& facts, const PrivateFacts& private_facts,
                                                      bool attach_crash_dump = false,
                                                      std::optional<std::time_t> now = std::nullopt);

std::string BugReportPreviewText(const BugReportPackage& package);
std::string BugReportRequestJson(const BugReportPackage& package);

} // namespace sos
