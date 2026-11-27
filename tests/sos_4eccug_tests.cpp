// SOS-4ECCUG (SteamOS, Steam Deck RADV VANGOGH, v0.1.0, build 1.01): the reporter's log read with the production
// session rules and compared with issue #5 (1.01 Deck), SOS-PLTEYS (1.00 Deck) and issue #6 (1.01 Intel ARL).
#include <string>

#include "session_outcome.h"
#include "v010_linux_silent_end.h"

namespace {

using namespace sos_silent_end;

constexpr std::string_view kEboot101 = "aafe8a2ab8e55e0044fe371248749026c494a07d6864fbb6a7fa89fe39999d58";
constexpr std::string_view kXkb = "xkbcommon: ERROR: [XKB-679] couldn't find a Compose file for locale \"\"";

void TestReportedMachine(const Lines& log, const Lines& issue6) {
    Check(AnyLine(log, "Settings: God of War: Sons of Sparta PC v0.1.0, Linux x86-64, game build CUSA57767 1.01 USA") &&
              AnyLine(log, "Build: God of War: Sons of Sparta (USA) 1.01") && Value(log, "Eboot SHA-256: ") == kEboot101,
          "v0.1.0 Linux run of build 1.01 (its eboot)");
    Check(Value(issue6, "Eboot SHA-256: ") == kEboot101 &&
              Value(log, "  module 3 Media/Modules/Il2cppUserAssemblies.prx ") ==
                  Value(issue6, "  module 3 Media/Modules/Il2cppUserAssemblies.prx "),
          "the same 1.01 image as issue #6 (eboot and Il2cppUserAssemblies layout)");
    Check(AnyLine(log, "CPU: AMD Custom APU 0932") && AnyLine(log, "GPU: selected AMD Custom GPU 0932 (RADV VANGOGH)") &&
              AnyLine(log, "GPU: Vulkan device AMD Custom GPU 0932 (RADV VANGOGH) (radv 26.1.99, Vulkan 1.4.350)"),
          "Steam Deck APU with RADV VANGOGH, Mesa 26.1.99");
    Check(AnyLine(log, "Swapchain: B8G8R8A8Unorm SrgbNonlinear, 4 images, 1280x800") && AnyLine(log, "Runtime: first vblank") &&
              AnyLine(log, "Startup: first present") && !AnyLine(log, "GPU: device lost"),
          "device, swapchain, first vblank and first present all succeeded; no device loss");
    Check(AnyLine(log, "Startup: linked modules initialized") && AnyLine(log, std::string(kGuestEntry)) &&
              AnyLine(log, "Runtime: Unity bootstrap entered") && AnyLine(log, "Runtime: AppContent initialized"),
          "the modules initialized and the guest reached Unity bootstrap and AppContent");
    Check(!AnyLine(log, "Runtime: guest main stack "), "no guest main stack line: the build predates the issue #5 fix");
    Check(!AnyLine(log, "<home>") && !AnyLine(log, "/home/") && !AnyLine(log, "/Games/"), "the fixture holds no personal path");
}

void TestOutcome(const Lines& log) {
    const sos::SessionOutcome outcome = sos::AnalyzeSessionOutcome(Join(log));
    Check(outcome.key == "startup_hang", "an unfinished start of the game: " + outcome.key);
    Check(!outcome.key.starts_with("launcher_") && outcome.key != "runtime_handoff_failed" && outcome.key != "play_blocked" &&
              outcome.key != "unknown",
          "the game process ran: not a launcher-only session");
    Check(outcome.key != "normal_exit" && outcome.detail.find("without a closing record") != std::string::npos,
          "not a clean exit: " + outcome.detail);
    Check(!AnyLine(log, "Job.Worker") && !AnyLine(log, "SIGSEGV"), "no game frame worker and no fault report: the end is silent");

    Lines crashed = log;
    for (const char* line : {"CRASH: signal 11 (SIGSEGV) code 1", "Crash: startup phase: guest entry",
                             "Crash: last completed stage: linked modules initialization", "Crash: thread 4242 (main)"}) {
        crashed.emplace_back(line);
    }
    Check(sos::AnalyzeSessionOutcome(Join(crashed)).key == "startup_crash",
          "the report the current runtime writes for a fault there is read as a crash while starting");
}

void TestSameEnd(const Lines& log, const Lines& issue5, const Lines& plteys, const Lines& issue6) {
    Check(IsIssue5SilentEnd(log, issue5), "the last 24 lines equal the issue #5 Steam Deck log and end at semaphore id=2");
    Check(IsIssue5SilentEnd(plteys, issue5) && IsIssue5SilentEnd(issue6, issue5), "reference: SOS-PLTEYS and issue #6 have the same end");
    const Lines guest = GuestStartup(log);
    Check(!guest.empty() && guest == GuestStartup(plteys) && guest == GuestStartup(issue6),
          "from the guest entry point to the end, the same " + std::to_string(guest.size()) +
              " lines as SOS-PLTEYS (1.00) and issue #6 (1.01)");

    Lines cut = log;
    cut.pop_back();
    Check(!IsIssue5SilentEnd(cut, issue5), "contrast: a log stopping at semaphore id=1 is not this end");
    Lines longer = log;
    longer.emplace_back("Runtime: guest thread 'posix' created (stack=65536, prio=700, entry=0x800000000, arg=0x0)");
    Check(!IsIssue5SilentEnd(longer, issue5), "contrast: a log going past semaphore id=2 is not this end");
}

void TestXkbIsNoise(const Lines& log, const Lines& plteys, const Lines& issue6) {
    const auto count = std::count_if(log.begin(), log.end(), [](const std::string& line) { return StartsWith(line, kXkb); });
    const Lines game = From(log, "Overlay compatibility: original game process");
    Check(count == 2 && AnyLine(game, kXkb) && AnyLine(From(From(game, kXkb), "Window: "), "Runtime: Unity bootstrap entered"),
          "the xkbcommon locale error is logged by the launcher and the game window, and the guest starts after both");
    Check(!AnyLine(plteys, kXkb) && !AnyLine(issue6, kXkb), "SOS-PLTEYS and issue #6 end the same way without the xkbcommon error");
}

} // namespace

int main() {
    const std::string text = Read("tests/fixtures/SOS_4ECCUG_v010_steamos_log.txt");
    const std::string issue5 = Read("tests/fixtures/issue_5_v010_steam_deck_tail_log.txt");
    const std::string plteys = Read("tests/fixtures/SOS_PLTEYS_v010_steamos_log.txt");
    const std::string issue6 = Read("tests/fixtures/issue_6_v010_linux_intel_arl_log.txt");
    if (text.empty() || issue5.empty() || plteys.empty() || issue6.empty()) {
        std::printf("  [fail] the SOS-4ECCUG, issue #5, SOS-PLTEYS and issue #6 fixtures could not be read\n");
        return 1;
    }
    const Lines log = Split(text), deck = Split(issue5), deck100 = Split(plteys), arl = Split(issue6);
    TestReportedMachine(log, arl);
    TestOutcome(log);
    TestSameEnd(log, deck, deck100, arl);
    TestXkbIsNoise(log, deck100, arl);
    std::printf("sos_4eccug: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
