#include "bug_report.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "game_profile.h"
#include "localization/ui_text.h"
#include "utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sos {
namespace fs = std::filesystem;
namespace {

constexpr std::string_view kLogPrefix = "sons_of_sparta_";
constexpr std::string_view kLogSuffix = ".log";
constexpr std::string_view kDumpSuffix = ".dmp";
constexpr char kMinidumpSignature[] = {'M', 'D', 'M', 'P'};
constexpr size_t kFewestLogDigits = 3, kMostLogDigits = 10;
constexpr size_t kLogsLookedAt = 200;
constexpr size_t kSessionHeadBytes = 128 * 1024;
constexpr size_t kLogTailBytes = 256 * 1024;
constexpr size_t kMaxReasonChars = 384;
constexpr size_t kMaxFaultSummaryChars = 512;
constexpr size_t kMaxStageChars = 96;

constexpr std::string_view kGameStartMarks[] = {"Launcher: the game process continues this log", "Launcher: closed",
                                                "Runtime: Unity bootstrap entered", "GPU: window and presenter ready",
                                                "Startup: game-only child process"};
constexpr std::string_view kLauncherClosedMark = "Launcher: quit";
constexpr std::string_view kGuestCodeMark = "Runtime: Unity bootstrap entered";
constexpr std::string_view kBuildSectionMark = "[build]";
constexpr std::string_view kRunSectionMark = "\n[run] ";

constexpr char kDescriptionKey[] = "description";
constexpr char kLogTextKey[] = "log_text";

struct FieldName {
    std::string_view key;
    std::string_view label;
};

constexpr FieldName kFieldNames[] = {
    {kDescriptionKey, "Description"},
    {"launcher_version", "Launcher version"},
    {"game_build", "Game build"},
    {"game_version", "Game version"},
    {"cusa", "CUSA"},
    {"operating_system", "Operating system"},
    {"log_file", "Selected log"},
    {"log_clipped", "Log shortened to its end"},
    {"crash_detected", "Crash detected"},
    {"crash_kind", "Crash classification"},
    {"last_startup_stage", "Last startup stage"},
    {"device_lost", "Vulkan device lost"},
    {"device_lost_reason", "Device-lost root reason"},
    {"device_fault_available", "VK_EXT_device_fault available"},
    {"device_fault_summary", "Device fault details"},
    {"dump_available", "Crash dump available"},
    {"dump_included", "Crash dump included"},
    {kLogTextKey, "Log after privacy filtering"},
};

constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool IsGameLogName(std::string_view name) {
    if (!name.starts_with(kLogPrefix) || !name.ends_with(kLogSuffix)) return false;
    name.remove_prefix(kLogPrefix.size());
    name.remove_suffix(kLogSuffix.size());
    return name.size() >= kFewestLogDigits && name.size() <= kMostLogDigits &&
           std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool StartsLikeMinidump(const fs::path& file) {
    char head[sizeof(kMinidumpSignature)] = {};
    std::ifstream in(file, std::ios::binary);
    in.read(head, sizeof(head));
    return size_t(in.gcount()) == sizeof(head) &&
           std::equal(std::begin(head), std::end(head), std::begin(kMinidumpSignature));
}

std::string ReadPart(const fs::path& file, uintmax_t offset, size_t size) {
    std::ifstream in(file, std::ios::binary);
    std::string text(size, '\0');
    in.seekg(std::streamoff(offset));
    in.read(text.data(), std::streamsize(size));
    text.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
    return text;
}

std::string ReadTail(const fs::path& file, size_t max_bytes) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size == 0) return {};
    const uintmax_t offset = size > max_bytes ? size - max_bytes : 0;
    return ReadPart(file, offset, size_t(size - offset));
}

std::string Trim(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(kSpace) - first + 1));
}

std::string BoundedSingleLine(std::string_view text, size_t max_chars) {
    std::string out;
    out.reserve(std::min(text.size(), max_chars));
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) out += ' ';
        else out += c;
    }
    out = Trim(out);
    if (out.size() > max_chars) {
        out.resize(BytesOfCharacters(out, max_chars));
        out = Trim(out);
    }
    return out;
}

struct Session {
    LogActivity activity = LogActivity::kLauncher;
    bool closed_in_launcher = false;
    bool build_only = false;
};

uintmax_t SessionStart(const fs::path& file, const std::string& head) {
    if (!head.starts_with(kBuildSectionMark)) return 0;
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    std::string chunk = head;
    for (uintmax_t offset = 0;;) {
        const size_t found = chunk.find(kRunSectionMark);
        if (found != std::string::npos) return offset + found + 1;
        if (ec || offset + chunk.size() >= size || chunk.size() < kRunSectionMark.size()) return uintmax_t(-1);
        offset += chunk.size() - (kRunSectionMark.size() - 1);
        chunk = ReadPart(file, offset, kSessionHeadBytes);
    }
}

Session ReadSession(const fs::path& file) {
    std::string head = ReadPart(file, 0, kSessionHeadBytes);
    const uintmax_t start = SessionStart(file, head);
    if (start == uintmax_t(-1)) return {LogActivity::kLauncher, false, true};
    if (start) head = ReadPart(file, start, kSessionHeadBytes);
    const auto holds = [&](std::string_view mark) { return head.find(mark) != std::string::npos; };
    if (holds(kGuestCodeMark)) return {LogActivity::kGameplay, false};
    if (std::any_of(std::begin(kGameStartMarks), std::end(kGameStartMarks), holds)) return {LogActivity::kGameStarted, false};
    return {LogActivity::kLauncher, holds(kLauncherClosedMark)};
}

std::string SessionHead(const fs::path& file) {
    std::string head = ReadPart(file, 0, kSessionHeadBytes);
    const uintmax_t start = SessionStart(file, head);
    if (start == uintmax_t(-1)) return {};
    return start ? ReadPart(file, start, kSessionHeadBytes) : head;
}

std::time_t SystemTimeOf(fs::file_time_type written) {
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(written - fs::file_time_type::clock::now());
    return std::chrono::system_clock::to_time_t(system);
}

struct Candidate {
    fs::path path;
    std::string name;
    fs::file_time_type written;
    uintmax_t bytes = 0;
    bool running = false;
};

bool IsRunningLog(const fs::path& file, const fs::path& running_log) {
    std::error_code ec;
    return !running_log.empty() && (file == running_log || fs::equivalent(file, running_log, ec));
}

std::vector<Candidate> LogCandidates(const fs::path& folder, const fs::path& running_log) {
    std::vector<Candidate> found;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry;
        const std::string name = Utf8(it->path().filename());
        if (!IsGameLogName(name) || !it->is_regular_file(entry) || it->is_symlink(entry)) continue;
        const bool running = IsRunningLog(it->path(), running_log);
        const uintmax_t bytes = running ? fs::file_size(it->path(), entry) : it->file_size(entry);
        const fs::file_time_type written = running ? fs::last_write_time(it->path(), entry) : it->last_write_time(entry);
        if (entry || (bytes == 0 && !running)) continue;
        found.push_back({it->path(), name, written, bytes, running});
    }
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.running != b.running) return a.running;
        return a.written != b.written ? a.written > b.written : a.name > b.name;
    });
    return found;
}

void MarkLastGameplay(std::vector<GameLog>& logs) {
    const auto played = [](const GameLog& log) { return log.activity == LogActivity::kGameplay; };
    const auto last = std::find_if(logs.begin(), logs.end(), played);
    if (last != logs.end() && last->role == LogRole::kNone) last->role = LogRole::kLastGameplay;
    for (GameLog& log : logs) {
        if (log.role != LogRole::kNone) continue;
        if (log.activity == LogActivity::kGameplay) log.role = LogRole::kOtherGameplay;
        else if (log.activity == LogActivity::kLauncher) log.role = LogRole::kOtherLauncher;
    }
}

void NoteCrash(GameLog& log, std::optional<std::time_t> now) {
    const std::string tail = ReadTail(log.path, kLogTailBytes);
    log.diagnostics = AnalyzeLogDiagnostics(tail);
    const std::string name = DumpNameOf(log.name);
    if (name.empty()) return;
    const fs::path path = log.path.parent_path() / name;
    const DumpProblem problem = CheckCrashDump(log, path, now);
    if (problem != DumpProblem::kNone && problem != DumpProblem::kTooLarge && problem != DumpProblem::kNotADump) return;
    std::error_code ec;
    log.crash = CrashDump{path, name, fs::file_size(path, ec)};
    log.crash_problem = problem;
    if (problem == DumpProblem::kNone) {
        log.diagnostics.crash_detected = true;
        if (log.diagnostics.crash_kind == CrashKind::kNone) {
            log.diagnostics.crash_kind = log.diagnostics.device_lost ? CrashKind::kDeviceLostCrash : CrashKind::kGenericCrash;
        }
    }
}

#ifdef _WIN32
std::string WindowsVersionText() {
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get_version =
        ntdll ? reinterpret_cast<GetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!get_version || get_version(&version) != 0) return "Windows";
    constexpr DWORD kFirstWindows11Build = 22000;
    const char* name = version.dwMajorVersion == 10
                           ? (version.dwBuildNumber >= kFirstWindows11Build ? "Windows 11" : "Windows 10")
                           : "Windows";
    return std::string(name) + " (build " + std::to_string(version.dwBuildNumber) + ")";
}
#else
std::string LinuxDistributionText() {
    std::ifstream in("/etc/os-release");
    constexpr std::string_view kKey = "PRETTY_NAME=";
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with(kKey)) continue;
        std::string value = line.substr(kKey.size());
        std::erase(value, '"');
        return value;
    }
    return "Linux";
}
#endif

} // namespace

const char* CrashKindKey(CrashKind kind) {
    switch (kind) {
    case CrashKind::kNone: return "none";
    case CrashKind::kGenericCrash: return "generic_crash";
    case CrashKind::kDeviceLost: return "device_lost";
    case CrashKind::kDeviceLostCrash: return "device_lost_crash";
    case CrashKind::kManualWithPriorDeviceLost: return "manual_with_prior_device_lost";
    }
    return "none";
}

const char* CrashKindLabel(CrashKind kind) {
    switch (kind) {
    case CrashKind::kNone: return "No crash detected";
    case CrashKind::kGenericCrash: return "Runtime crash";
    case CrashKind::kDeviceLost: return "Vulkan device lost";
    case CrashKind::kDeviceLostCrash: return "Vulkan device lost + crash";
    case CrashKind::kManualWithPriorDeviceLost: return "Prior Vulkan device lost";
    }
    return "No crash detected";
}

LogDiagnostics AnalyzeLogDiagnostics(std::string_view log_text) {
    LogDiagnostics diag;
    bool saw_crash_line = false;
    bool saw_device_lost_exit = false;
    std::vector<std::string> fault_parts;
    size_t fault_lines = 0;

    for (size_t at = 0; at < log_text.size();) {
        const size_t end = std::min(log_text.find('\n', at), log_text.size());
        std::string_view line = log_text.substr(at, end - at);
        if (line.ends_with('\r')) line.remove_suffix(1);
        at = end + 1;

        if (line.starts_with("GPU: selected ")) {
            diag.gpu_name = BoundedSingleLine(line.substr(14), 128);
            continue;
        }
        if (line.starts_with("CRASH:")) {
            saw_crash_line = true;
            continue;
        }
        if (line.starts_with("  minidump: ")) {
            std::string_view rest = line.substr(12);
            const size_t space = rest.find(' ');
            std::string_view candidate = space == std::string_view::npos ? rest : rest.substr(0, space);
            if (candidate.ends_with(kDumpSuffix)) diag.logged_minidump_name = std::string(candidate);
            continue;
        }
        if (line.starts_with("Startup phase: ")) {
            diag.last_startup_stage = BoundedSingleLine(line.substr(15), kMaxStageChars);
            continue;
        }
        if (line.starts_with("  startup phase: ")) {
            diag.last_startup_stage = BoundedSingleLine(line.substr(17), kMaxStageChars);
            continue;
        }
        if (const size_t phase_at = line.find("phase="); phase_at != std::string_view::npos) {
            std::string_view rest = line.substr(phase_at + 6);
            const size_t comma = rest.find(',');
            diag.last_startup_stage = BoundedSingleLine(rest.substr(0, comma), kMaxStageChars);
        }
        if (line.starts_with("Startup: ")) {
            std::string_view rest = line.substr(9);
            const size_t plus = rest.find(" +");
            if (plus != std::string_view::npos && plus > 0) {
                diag.last_startup_stage = BoundedSingleLine(rest.substr(0, plus), kMaxStageChars);
            }
            continue;
        }
        if (line.starts_with("Runtime: first ")) {
            diag.last_startup_stage = BoundedSingleLine(line.substr(9), kMaxStageChars);
            continue;
        }
        // Authoritative first device-lost root cause: later faults or secondary consequences never replace it.
        if (line.starts_with("GPU device lost: first detected in ")) {
            if (!diag.device_lost) {
                diag.device_lost = true;
                diag.device_lost_reason = BoundedSingleLine(line, kMaxReasonChars);
            }
            continue;
        }
        if (diag.device_lost && diag.device_lost_timelines.empty() && line.starts_with("GPU timelines:")) {
            diag.device_lost_timelines = BoundedSingleLine(line, 256);
            continue;
        }
        if (diag.device_lost && line.starts_with("Runtime: GPU device lost:") && line.find("ends with status 25") != std::string_view::npos) {
            saw_device_lost_exit = true;
            continue;
        }
        if (diag.device_lost && line.starts_with("Device fault: ")) {
            const std::string_view detail = line.substr(14);
            if (detail.starts_with("unavailable")) {
                if (diag.device_fault_status.empty()) diag.device_fault_status = "unavailable";
            } else if (detail.starts_with("not queried")) {
                if (diag.device_fault_status.empty()) diag.device_fault_status = "not_queried";
            } else if (detail.starts_with("query failed")) {
                if (diag.device_fault_status.empty()) {
                    diag.device_fault_status = "failed";
                    fault_parts.push_back(BoundedSingleLine(detail, 160));
                }
            } else if (fault_lines < 4) {
                diag.device_fault_available = true;
                diag.device_fault_status = "captured";
                fault_parts.push_back(BoundedSingleLine(detail, 180));
                ++fault_lines;
            }
            continue;
        }
    }

    if (!fault_parts.empty()) {
        std::string combined;
        for (size_t i = 0; i < fault_parts.size(); ++i) {
            if (i) combined += "; ";
            combined += fault_parts[i];
        }
        diag.device_fault_summary = BoundedSingleLine(combined, kMaxFaultSummaryChars);
    }

    if (diag.device_lost && !diag.device_lost_timelines.empty()) {
        const std::string combined = diag.device_lost_reason + "; " + diag.device_lost_timelines;
        diag.device_lost_reason = BoundedSingleLine(combined, kMaxReasonChars);
    }

    if (diag.device_lost && saw_crash_line) {
        diag.crash_detected = true;
        diag.crash_kind = CrashKind::kDeviceLostCrash;
    } else if (diag.device_lost && saw_device_lost_exit) {
        diag.crash_detected = true;
        diag.crash_kind = CrashKind::kDeviceLost;
    } else if (diag.device_lost) {
        diag.crash_detected = false;
        diag.crash_kind = CrashKind::kManualWithPriorDeviceLost;
    } else if (saw_crash_line) {
        diag.crash_detected = true;
        diag.crash_kind = CrashKind::kGenericCrash;
    } else {
        diag.crash_detected = false;
        diag.crash_kind = CrashKind::kNone;
    }
    return diag;
}

std::vector<GameLog> FindGameLogs(const fs::path& folder, const fs::path& running_log, size_t most,
                                  std::optional<std::time_t> now) {
    std::vector<GameLog> logs;
    std::vector<Candidate> candidates = LogCandidates(folder, running_log);
    if (candidates.size() > kLogsLookedAt) candidates.resize(kLogsLookedAt);
    for (const Candidate& candidate : candidates) {
        if (logs.size() == most) break;
        const Session session = ReadSession(candidate.path);
        if ((session.closed_in_launcher || session.build_only) && !candidate.running) continue;
        LogRole role = LogRole::kNone;
        if (candidate.running) {
            role = session.activity == LogActivity::kGameplay ? LogRole::kCurrentGameplay : LogRole::kCurrentLauncher;
        }
        GameLog log{candidate.path, candidate.name, SystemTimeOf(candidate.written), candidate.bytes, session.activity,
                    role, std::nullopt, DumpProblem::kMissing, {}};
        NoteCrash(log, now);
        logs.push_back(std::move(log));
    }
    MarkLastGameplay(logs);
    return logs;
}

std::string DumpNameOf(std::string_view log_name) {
    if (!IsGameLogName(log_name)) return {};
    return std::string(log_name.substr(0, log_name.size() - kLogSuffix.size())) + std::string(kDumpSuffix);
}

DumpProblem CheckCrashDump(const GameLog& log, const fs::path& dump, std::optional<std::time_t> now) {
    const std::string expected = DumpNameOf(log.name);
    if (expected.empty() || Utf8(dump.filename()) != expected) return DumpProblem::kNotThisLog;
    if (dump.lexically_normal().parent_path() != log.path.lexically_normal().parent_path()) return DumpProblem::kNotThisLog;
    if (!log.diagnostics.logged_minidump_name.empty() && log.diagnostics.logged_minidump_name != expected) {
        return DumpProblem::kNotThisLog;
    }
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(dump, ec);
    if (ec || !fs::exists(status)) return DumpProblem::kMissing;
    if (!fs::is_regular_file(status)) return DumpProblem::kNotAFile;
    const fs::file_time_type written = fs::last_write_time(dump, ec);
    if (ec) return DumpProblem::kMissing;
    const std::time_t dump_time = SystemTimeOf(written);
    if (std::abs(std::difftime(dump_time, log.written)) > kDumpPairingSeconds) return DumpProblem::kNotThisLog;
    const std::time_t reference_now = now.value_or(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    if (std::difftime(dump_time, reference_now) > kDumpFutureGraceSeconds) return DumpProblem::kNotThisLog;
    const uintmax_t size = fs::file_size(dump, ec);
    if (ec || size == 0) return DumpProblem::kEmpty;
    if (size > kMostDumpBytes) return DumpProblem::kTooLarge;
    if (size < kFewestDumpBytes || !StartsLikeMinidump(dump)) return DumpProblem::kNotADump;
    return DumpProblem::kNone;
}

bool DefaultAttachCrashReport(const GameLog& log) { return log.CanAttachCrash(); }

size_t DefaultGameLog(const std::vector<GameLog>& logs) {
    const auto started = std::find_if(logs.begin(), logs.end(), [](const GameLog& log) { return log.activity != LogActivity::kLauncher; });
    if (started != logs.end()) return size_t(started - logs.begin());
    const auto earlier = std::find_if(logs.begin(), logs.end(), [](const GameLog& log) {
        return log.role != LogRole::kCurrentLauncher && log.role != LogRole::kCurrentGameplay;
    });
    return earlier == logs.end() ? 0 : size_t(earlier - logs.begin());
}

const char* LogRoleText(LogRole role) {
    switch (role) {
    case LogRole::kCurrentLauncher: return "Current launcher session";
    case LogRole::kCurrentGameplay: return "Current session \xC2\xB7 Launcher + gameplay";
    case LogRole::kLastGameplay: return "Last gameplay";
    case LogRole::kOtherGameplay: return "Gameplay session";
    case LogRole::kOtherLauncher: return "Launcher only";
    case LogRole::kNone: break;
    }
    return nullptr;
}

const char* PreviewButtonText(bool preview_open) { return preview_open ? "Close preview" : "Open preview"; }

std::string LogDateText(std::time_t written) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &written);
#else
    localtime_r(&written, &local);
#endif
    char text[40];
    std::snprintf(text, sizeof(text), "%s %d, %04d %02d:%02d", kMonths[std::clamp(local.tm_mon, 0, 11)], local.tm_mday,
                  local.tm_year + 1900, local.tm_hour, local.tm_min);
    return text;
}

std::string LogSizeText(uintmax_t bytes) {
    constexpr uintmax_t kKilobyte = 1024, kMegabyte = kKilobyte * 1024;
    char text[32];
    if (bytes < kMegabyte) {
        std::snprintf(text, sizeof(text), "%llu KB",
                      static_cast<unsigned long long>(std::max<uintmax_t>(1, (bytes + kKilobyte / 2) / kKilobyte)));
    } else {
        std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / double(kMegabyte));
    }
    return text;
}

size_t BytesOfCharacters(std::string_view text, size_t most) {
    size_t characters = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (characters == most) return i;
        ++characters;
    }
    return text.size();
}

size_t CountCharacters(std::string_view text) {
    return size_t(std::count_if(text.begin(), text.end(),
                                [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

DraftProblem CheckDraft(std::string_view description, bool has_log) {
    if (CountCharacters(Trim(description)) < kFewestDescriptionCharacters) return DraftProblem::kDescriptionTooShort;
    return has_log ? DraftProblem::kNone : DraftProblem::kNoLog;
}

std::string OperatingSystemText() {
#ifdef _WIN32
    return WindowsVersionText();
#else
    return LinuxDistributionText();
#endif
}

const std::string& BugReportPackage::Value(std::string_view key) const {
    static const std::string missing;
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const BugReportField& f) { return f.key == key; });
    return found == fields.end() ? missing : found->value;
}

std::string LastBytes(std::string_view text, size_t most) {
    if (text.size() <= most) return std::string(text);
    std::string_view tail = text.substr(text.size() - most);
    const size_t line = tail.find('\n');
    if (line != std::string_view::npos && line + 1 < tail.size()) tail.remove_prefix(line + 1);
    return std::string(tail);
}

const GameBuild* SessionBuild(std::string_view log_head) {
    std::string_view game_label, launcher_label;
    const auto label_after = [](std::string_view line, std::string_view prefix, std::string_view end) {
        line.remove_prefix(prefix.size());
        return line.substr(0, line.find(end));
    };
    for (size_t at = 0; at < log_head.size();) {
        const size_t end = std::min(log_head.find('\n', at), log_head.size());
        std::string_view line = log_head.substr(at, end - at);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with("Launcher: active game build: ")) {
            launcher_label = label_after(line, "Launcher: active game build: ", " (");
        } else if (line.starts_with("Game: build ")) {
            game_label = label_after(line, "Game: build ", ",");
        }
        at = end + 1;
    }
    const std::string_view label = game_label.empty() ? launcher_label : game_label;
    for (const GameBuild& build : SupportedBuilds()) {
        if (label == build.label) return &build;
    }
    return nullptr;
}

ReportFacts SessionReportFacts(const ReportFacts& facts, const GameLog& log) {
    const GameBuild* build = SessionBuild(SessionHead(log.path));
    if (!build || (facts.cusa == build->title_id && facts.game_version == build->version)) return facts;
    ReportFacts session = facts;
    session.game_build = BuildDisplayName(*build);
    session.game_version = build->version;
    session.cusa = build->title_id;
    return session;
}

std::optional<BugReportPackage> BuildBugReportPackage(std::string_view description, const GameLog& log,
                                                      const ReportFacts& facts, const PrivateFacts& private_facts,
                                                      bool attach_crash_dump, std::optional<std::time_t> now) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(log.path, ec);
    if (ec || size == 0) return std::nullopt;
    const uintmax_t offset = size > kLogTailBytes ? size - kLogTailBytes : 0;
    std::string raw = ReadPart(log.path, offset, size_t(size - offset));
    if (raw.empty()) return std::nullopt;
    if (offset > 0) {
        const size_t line = raw.find('\n');
        if (line != std::string::npos) raw.erase(0, line + 1);
    }
    const LogDiagnostics diag = AnalyzeLogDiagnostics(raw);
    const std::string scrubbed = ScrubText(raw, private_facts);
    std::string text = LastBytes(scrubbed, kMostLogBytes);
    if (text.find_first_not_of(" \t\n") == std::string::npos) return std::nullopt;
    const bool clipped = offset > 0 || text.size() < scrubbed.size();

    std::string summary = ScrubText(Trim(description), private_facts);
    summary.resize(BytesOfCharacters(summary, kMostDescriptionCharacters));
    if (summary.empty()) return std::nullopt;

    const bool dump_valid = log.crash && CheckCrashDump(log, log.crash->path, now) == DumpProblem::kNone;
    std::optional<CrashDump> attached;
    if (attach_crash_dump && dump_valid) {
        std::error_code size_error;
        const uintmax_t dump_bytes = fs::file_size(log.crash->path, size_error);
        if (!size_error && dump_bytes >= kFewestDumpBytes && dump_bytes <= kMostDumpBytes) {
            attached = CrashDump{log.crash->path, log.crash->name, dump_bytes};
        }
    }

    const bool crash_detected = diag.crash_detected || dump_valid;
    CrashKind kind = diag.crash_kind;
    if (kind == CrashKind::kNone && dump_valid) {
        kind = diag.device_lost ? CrashKind::kDeviceLostCrash : CrashKind::kGenericCrash;
    }

    const std::string scrubbed_reason =
        diag.device_lost ? BoundedSingleLine(ScrubText(diag.device_lost_reason, private_facts), kMaxReasonChars) : std::string();
    const std::string scrubbed_fault =
        diag.device_lost && diag.device_fault_available
            ? BoundedSingleLine(ScrubText(diag.device_fault_summary, private_facts), kMaxFaultSummaryChars)
            : std::string();
    const std::string stage = BoundedSingleLine(ScrubText(diag.last_startup_stage, private_facts), kMaxStageChars);

    const ReportFacts session = SessionReportFacts(facts, log);
    std::string values[] = {
        std::move(summary),
        session.launcher_version,
        session.game_build,
        session.game_version,
        session.cusa,
        session.operating_system,
        log.name,
        clipped ? "yes" : "no",
        crash_detected ? "yes" : "no",
        CrashKindKey(kind),
        stage,
        diag.device_lost ? "yes" : "no",
        scrubbed_reason,
        diag.device_lost && diag.device_fault_available ? "yes" : "no",
        scrubbed_fault,
        dump_valid ? "yes" : "no",
        attached ? "yes" : "no",
        std::move(text),
    };

    BugReportPackage package;
    for (size_t i = 0; i < std::size(kFieldNames); ++i) {
        package.fields.push_back({kFieldNames[i].key, kFieldNames[i].label, std::move(values[i])});
    }
    package.crash_dump_requested = attach_crash_dump;
    package.crash_dump = std::move(attached);
    return package;
}

std::string BugReportPreviewText(const BugReportPackage& package) {
    std::string out;
    for (const BugReportField& field : package.fields) {
        const std::string label = Tr(std::string(field.label));
        if (field.key == kDescriptionKey || field.key == kLogTextKey) {
            out += (out.empty() ? "" : "\n") + label + ":\n" + field.value + "\n";
            if (field.key == kDescriptionKey) out += "\n";
        } else {
            out += label + ": " + (field.value.empty() ? "-" : field.value) + "\n";
        }
    }
    if (package.crash_dump) {
        out += "\n" + std::string(Tr("Crash report")) + ": " + package.crash_dump->name + " (" +
               LogSizeText(package.crash_dump->bytes) + ", " + Tr("sent as a file, not shown here") + ")\n";
    }
    return out;
}

net::Json BugReportRequestBody(const BugReportPackage& package) {
    net::Json body = net::Json::Object();
    for (const BugReportField& field : package.fields) body.set(field.key, field.value);
    if (package.crash_dump) {
        body.set("dump_file", package.crash_dump->name);
        body.set("dump_bytes", int64_t(package.crash_dump->bytes));
    }
    return body;
}

std::string BugReportRequestJson(const BugReportPackage& package) { return BugReportRequestBody(package).dump(); }

BugReportPackage SentReportOf(const net::Json& report) {
    BugReportPackage package;
    for (const FieldName& name : kFieldNames) package.fields.push_back({name.key, name.label, report[name.key].as_string()});
    return package;
}

} // namespace sos
