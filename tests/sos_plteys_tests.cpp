// SOS-PLTEYS (SteamOS, Steam Deck RADV VANGOGH, v0.1.0, build 1.00): the reporter's log read with the production
// session rules and compared with the issue #5 (1.01, Steam Deck) and issue #6 (1.01, Intel ARL) logs.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "session_outcome.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  [%s] %s\n", ok ? "ok" : "fail", name.c_str());
}

std::string Read(const char* path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using Lines = std::vector<std::string>;

Lines Split(const std::string& text) {
    Lines lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

std::string Join(const Lines& lines) {
    std::string text;
    for (const std::string& line : lines) text += line + "\n";
    return text;
}

bool StartsWith(const std::string& line, std::string_view prefix) { return line.compare(0, prefix.size(), prefix) == 0; }

bool AnyLine(const Lines& lines, std::string_view prefix) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return StartsWith(line, prefix); });
}

Lines From(const Lines& lines, std::string_view first) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (StartsWith(lines[i], first)) return {lines.begin() + std::ptrdiff_t(i), lines.end()};
    }
    return {};
}

constexpr std::string_view kLastLine = "Runtime: semaphore id=2, attr=0, initial=0, max=256";
constexpr std::string_view kTailStart = "Runtime: UserService initialized";
constexpr std::string_view kGuestEntry = "Entering original x86-64 code at guest offset 0x13550";

// The silent v0.1.0 end of issue #5: the same 24 lines after UserService, ending at semaphore id=2, with no report.
bool IsIssue5SilentEnd(const Lines& log, const Lines& issue5) {
    const Lines tail = From(log, kTailStart);
    return !log.empty() && log.back() == kLastLine && tail.size() == 24 && tail == From(issue5, kTailStart) &&
           !AnyLine(log, "CRASH:") && !AnyLine(log, "STOP:");
}

// Guest startup from the entry point on, as a multiset: timings and the order of concurrently created threads vary.
Lines GuestStartup(const Lines& log) {
    Lines lines;
    for (const std::string& line : From(log, kGuestEntry)) {
        if (!StartsWith(line, "Startup: ") && !StartsWith(line, "Checkpoint: ")) lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::string Value(const Lines& log, std::string_view prefix) {
    for (const std::string& line : log) {
        if (StartsWith(line, prefix)) return line.substr(prefix.size());
    }
    return {};
}

void TestReportedMachine(const Lines& log) {
    Check(AnyLine(log, "Settings: God of War: Sons of Sparta PC v0.1.0, Linux x86-64, game build CUSA57767 1.00 USA") &&
              Value(log, "Eboot SHA-256: ") == "19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31",
          "v0.1.0 Linux run of build 1.00 (its eboot)");
    Check(AnyLine(log, "CPU: AMD Custom APU 0932") && AnyLine(log, "GPU: selected AMD Custom GPU 0932 (RADV VANGOGH)") &&
              AnyLine(log, "GPU: Vulkan device AMD Custom GPU 0932 (RADV VANGOGH) (radv 26.1.99, Vulkan 1.4.350)"),
          "Steam Deck APU with RADV VANGOGH, Mesa 26.1.99");
    Check(AnyLine(log, "Swapchain: R8G8B8A8Unorm SrgbNonlinear, 4 images, 1024x640") && AnyLine(log, "Runtime: first vblank") &&
              AnyLine(log, "Startup: first present"),
          "device, swapchain, first vblank and the renderer's own first present all succeeded");
    Check(AnyLine(log, "Startup: linked modules initialized") && AnyLine(log, std::string(kGuestEntry)) &&
              AnyLine(log, "Runtime: Unity bootstrap entered") && AnyLine(log, "Runtime: AppContent initialized"),
          "the modules initialized and the guest reached Unity bootstrap and AppContent");
    Check(!AnyLine(log, "Runtime: guest main stack "), "no guest main stack line: the build predates the issue #5 fix");
    Check(!AnyLine(log, "<home>/Downloads") && !AnyLine(log, "[SPSX]"), "the fixture holds no personal path");
}

void TestOutcome(const Lines& log) {
    const sos::SessionOutcome outcome = sos::AnalyzeSessionOutcome(Join(log));
    Check(outcome.key == "startup_hang", "an unfinished start of the game: " + outcome.key);
    Check(!outcome.key.starts_with("launcher_") && outcome.key != "runtime_handoff_failed" && outcome.key != "play_blocked" &&
              outcome.key != "unknown",
          "the game process ran: not a launcher-only session");
    Check(outcome.key != "normal_exit" && outcome.detail.find("without a closing record") != std::string::npos,
          "not a clean exit: " + outcome.detail);

    Lines crashed = log;
    for (const char* line : {"CRASH: signal 11 (SIGSEGV) code 1", "Crash: startup phase: guest entry",
                             "Crash: last completed stage: linked modules initialization", "Crash: thread 4242 (main)"}) {
        crashed.emplace_back(line);
    }
    Check(sos::AnalyzeSessionOutcome(Join(crashed)).key == "startup_crash",
          "the report the current runtime writes for a fault there is read as a crash while starting");
}

void TestSameEndAsIssue5(const Lines& log, const Lines& issue5, const Lines& issue6) {
    Check(IsIssue5SilentEnd(log, issue5), "the last 24 lines equal the issue #5 Steam Deck log and end at semaphore id=2");
    Check(IsIssue5SilentEnd(issue6, issue5), "reference: the issue #6 log has the same end");
    for (const char* line : {"Runtime: guest thread 'AssetGarbageCollectorHelper' created", "Runtime: UserService initialized",
                             "Runtime: AppContent initialized", "Runtime: dlsym handle 0 scriptingGetMem -> 0x801411d40",
                             "Runtime: mapped flexible memory 0x108aec0000", "Runtime: semaphore id=1"}) {
        Check(AnyLine(log, line), std::string("present on the way to the end: ") + line);
    }

    Lines cut = log;
    cut.pop_back();
    Check(!IsIssue5SilentEnd(cut, issue5), "contrast: a log stopping at semaphore id=1 is not this end");
    Lines longer = log;
    longer.emplace_back("Runtime: guest thread 'posix' created (stack=65536, prio=700, entry=0x800000000, arg=0x0)");
    Check(!IsIssue5SilentEnd(longer, issue5), "contrast: a log going past semaphore id=2 is not this end");
    Lines reported = log;
    reported.emplace_back("CRASH: signal 11 (SIGSEGV) code 1");
    Check(!IsIssue5SilentEnd(reported, issue5), "contrast: a log with a crash report is not the silent end");
}

void TestBuildIndependence(const Lines& log, const Lines& issue5, const Lines& issue6) {
    Check(AnyLine(issue5, "Settings: God of War: Sons of Sparta PC v0.1.0, Linux x86-64, game build CUSA57767 1.01 USA") &&
              AnyLine(issue6, "Settings: God of War: Sons of Sparta PC v0.1.0, Linux x86-64, game build CUSA57767 1.01 USA"),
          "the issue #5 and #6 logs are build 1.01");
    Check(Value(issue6, "Eboot SHA-256: ") != Value(log, "Eboot SHA-256: ") &&
              Value(log, "  module 3 Media/Modules/Il2cppUserAssemblies.prx ") !=
                  Value(issue6, "  module 3 Media/Modules/Il2cppUserAssemblies.prx "),
          "1.00 and 1.01 are different images (eboot and Il2cppUserAssemblies differ)");
    const Lines a = GuestStartup(log), b = GuestStartup(issue6);
    Check(!a.empty() && a == b, "from the guest entry point to the end, 1.00 and 1.01 log the same " + std::to_string(a.size()) +
                                    " lines: the end does not depend on the build");
}

} // namespace

int main() {
    const std::string text = Read("tests/fixtures/SOS_PLTEYS_v010_steamos_log.txt");
    const std::string issue5 = Read("tests/fixtures/issue_5_v010_steam_deck_tail_log.txt");
    const std::string issue6 = Read("tests/fixtures/issue_6_v010_linux_intel_arl_log.txt");
    if (text.empty() || issue5.empty() || issue6.empty()) {
        std::printf("  [fail] the SOS-PLTEYS, issue #5 and issue #6 fixtures could not be read\n");
        return 1;
    }
    const Lines log = Split(text), deck = Split(issue5), arl = Split(issue6);
    TestReportedMachine(log);
    TestOutcome(log);
    TestSameEndAsIssue5(log, deck, arl);
    TestBuildIndependence(log, deck, arl);
    std::printf("sos_plteys: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
