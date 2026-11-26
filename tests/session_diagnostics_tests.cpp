// Startup and session diagnostics added after the Steam Deck report SOS-VLHSWP (a launcher that ran and a session that
// ended with no word about PLAY): flushed stage lines with durations, the diagnostic watchdog, the system report, the
// launcher input telemetry and Gamescope gamepad policy, the deferred graphics card probe, the session outcome rules
// shared with the dashboard, and the Bug Report reading the new lines. Headless: no window, GPU, network or game.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "graphics_probe.h"
#include "launcher_input.h"
#include "session_outcome.h"
#include "sos_launcher.h"
#include "startup_phase.h"
#include "system_report.h"

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "  [fail] %s\n", what.c_str());
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

int CountOf(const std::string& text, const std::string& part) {
    int count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}

int flushes = 0;
void CountFlush() {
    ++flushes;
    std::fflush(stdout);
}

// --- startup_phase.c: stage lines, results, events, the watchdog --------------------------------------------------
void TestStageLines(const std::filesystem::path& folder) {
    const std::filesystem::path out = folder / "stages.txt";
    std::fflush(stdout);
    Check(std::freopen(out.string().c_str(), "w", stdout) != nullptr, "stdout can be captured");
    sos_startup_set_flush(CountFlush);
    sos_startup_phase("configuration load");
    sos_startup_phase("configuration load");  // the same phase again: no line
    sos_startup_phase("play: game files recheck");
    sos_startup_stage_result("play: game files recheck", 1, "%s", "valid");
    sos_startup_phase("play: graphics preflight");
    sos_startup_stage_result("play: graphics preflight", 0, "%s", "this system cannot run the game");
    sos_startup_event("PLAY requested input=%s", "gamepad");
    std::fflush(stdout);
    const std::string text = ReadFile(out);
    Check(CountOf(text, "Stage: configuration load started +") == 1, "a phase change logs one 'started' line, a repeat none");
    Check(Has(text, "Stage: play: game files recheck started +") && Has(text, "(after configuration load, "),
          "a stage line names the previous stage and its duration");
    Check(Has(text, "Stage: play: game files recheck completed in ") && Has(text, " ms: valid"),
          "a completed stage logs its duration and result");
    Check(Has(text, "Stage: play: graphics preflight failed in ") && Has(text, ": this system cannot run the game"),
          "a failed stage logs 'failed', its duration and the reason");
    Check(Has(text, "Stage event: PLAY requested input=gamepad +"), "an event line carries the input source");
    Check(flushes >= 6, "every stage line and event is flushed at once (" + std::to_string(flushes) + " flushes)");
    Check(std::string(sos_startup_phase_completed()) == "play: game files recheck",
          "a failed stage does not become the last completed one");
    sos_startup_phase("launcher running");
    Check(std::string(sos_startup_phase_completed()) == "play: graphics preflight",
          "leaving a phase records it as the last one passed");
    sos_startup_set_flush(nullptr);
}

// Repeated PLAY refusals toggle the launcher's phases; they must never use up the lines the game's start needs.
void TestStageLineShares(const std::filesystem::path& folder) {
    const std::filesystem::path out = folder / "shares.txt";
    std::fflush(stdout);
    Check(std::freopen(out.string().c_str(), "w", stdout) != nullptr, "stdout can be captured again");
    for (int i = 0; i < 150; ++i) {
        sos_startup_phase("launcher running");
        sos_startup_phase("play: game files recheck");
    }
    sos_startup_phase("Vulkan device creation");
    sos_startup_phase("swapchain creation");
    std::fflush(stdout);
    const std::string text = ReadFile(out);
    Check(Has(text, "Stage: Vulkan device creation started +") && Has(text, "Stage: swapchain creation started +"),
          "the game's stages are logged after 300 launcher phase changes");
    Check(CountOf(text, "Stage: launcher running started +") < 100, "the launcher's own lines are bounded");
}

// Log 192: a PLAY that started the game was logged as "Launcher result: quit" (names taken by array position, while the
// enum has PLAY = 0, QUIT = 1).
void TestLauncherResultNames() {
    Check(std::string(sos_launcher_result_name(SOS_LAUNCHER_PLAY)) == "play", "SOS_LAUNCHER_PLAY is logged as play");
    Check(std::string(sos_launcher_result_name(SOS_LAUNCHER_QUIT)) == "quit", "SOS_LAUNCHER_QUIT is logged as quit");
    Check(std::string(sos_launcher_result_name(SOS_LAUNCHER_BLOCKED)) == "blocked", "SOS_LAUNCHER_BLOCKED is blocked");
    Check(std::string(sos_launcher_result_name(SOS_LAUNCHER_GAME_STARTED)) == "game process started",
          "SOS_LAUNCHER_GAME_STARTED is game process started");
    Check(std::string(sos_launcher_result_name(static_cast<SosLauncherResult>(9))) == "unknown", "an unknown value is unknown");
}

void TestWatchdog() {
    Check(sos_startup_phase_budget_seconds("game running") == 0, "play after the first game frame is never timed");
    Check(sos_startup_phase_budget_seconds("pipeline cache warm-up") == 0,
          "a first-run shader warm-up is never reported as a hang");
    Check(sos_startup_phase_budget_seconds("controller icon preparation") == 0,
          "first-run controller icon preparation is never reported as a hang");
    Check(sos_startup_phase_budget_seconds("launcher loop active") == 0, "the idle launcher has no budget");
    Check(sos_startup_phase_budget_seconds("launcher running") == 0, "the running launcher has no budget");
    Check(sos_startup_phase_budget_seconds("first present") == 0, "gameplay has no budget");
    Check(sos_startup_phase_budget_seconds("play: graphics preflight") == 15, "PLAY's steps have 15 s");
    Check(sos_startup_phase_budget_seconds("launcher destroying renderer") == 15, "launcher teardown has 15 s");
    Check(sos_startup_phase_budget_seconds("guest entry") == 120, "the guest's own start has 120 s");
    Check(sos_startup_phase_budget_seconds("Vulkan device creation") == 45, "other startup stages have 45 s");

    const char* watched = nullptr;
    unsigned reports = 0;
    const char* phase = "Vulkan device creation";
    int fired = 0;
    for (double ms = 0; ms <= 3600000; ms += 500) fired += sos_startup_watchdog_poll(phase, ms, &watched, &reports);
    Check(fired == 4, "a stage stuck for an hour is reported at 1x, 2x, 4x and 8x its budget and then no more (" +
                          std::to_string(fired) + ")");
    watched = nullptr;
    reports = 0;
    Check(!sos_startup_watchdog_poll(phase, 44000, &watched, &reports), "nothing is reported before the budget");
    Check(sos_startup_watchdog_poll(phase, 45000, &watched, &reports), "the budget itself is reported");
    const char* next = "swapchain creation";
    Check(!sos_startup_watchdog_poll(next, 1000, &watched, &reports) && reports == 0, "a new phase starts counting again");
    fired = 0;
    watched = nullptr;
    reports = 0;
    for (double ms = 0; ms <= 3600000; ms += 1000) fired += sos_startup_watchdog_poll("launcher running", ms, &watched, &reports);
    Check(fired == 0, "an hour in the launcher is never reported");
}

// --- system_report.c ----------------------------------------------------------------------------------------------
struct FakeEnvironment {
    std::vector<std::pair<std::string, std::string>> values;
};
FakeEnvironment* g_environment = nullptr;
const char* FakeGetenv(const char* name) {
    for (const auto& [key, value] : g_environment->values)
        if (key == name) return value.c_str();
    return nullptr;
}

std::string Report(FakeEnvironment environment, const char* os_release, const char* meminfo) {
    g_environment = &environment;
    char out[2048];
    sos_system_report_format(out, sizeof(out), "0.1.1", FakeGetenv, os_release, meminfo, "6.5.0-valve22-1-neptune-65", "x86_64");
    return out;
}

void TestSystemReport() {
    const char* steamos = "NAME=\"SteamOS\"\nPRETTY_NAME=\"SteamOS\"\nVERSION_ID=3.6.19\nID=steamos\nVARIANT_ID=steamdeck\n";
    const char* meminfo = "MemTotal:       15950248 kB\nMemFree: 100 kB\nMemAvailable:   12000000 kB\nSwapTotal: 8388604 kB\n";
    const std::string deck = Report({{{"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"}, {"XDG_CURRENT_DESKTOP", "gamescope"},
                                      {"SteamDeck", "1"}, {"SteamGameId", "123"}, {"DISPLAY", ":0"},
                                      {"LD_PRELOAD", "/home/deck/.local/share/Steam/ubuntu12_32/gameoverlayrenderer.so"},
                                      {"XDG_SESSION_TYPE", "x11"}, {"SDL_VIDEODRIVER", "/home/deck/evil"}}},
                                    steamos, meminfo);
#ifdef _WIN32
    const char* os = "Windows";
#else
    const char* os = "Linux";
#endif
    Check(Has(deck, std::string("System: port 0.1.1, ") + os + " x86_64, kernel 6.5.0-valve22-1-neptune-65\n"),
          "port, OS and kernel");
    Check(Has(deck, "System: distribution SteamOS (version 3.6.19, variant steamdeck), SteamOS yes\n"), "SteamOS release");
    Check(Has(deck, "mode=Gaming Mode gamescope=yes steam=yes steam_deck=yes session_type=x11 desktop=gamescope"),
          "a Steam Deck in Gaming Mode is named so");
    Check(Has(deck, "steam_overlay=yes"), "the Steam overlay is detected from LD_PRELOAD");
    Check(!Has(deck, "/home/deck") && Has(deck, "sdl_videodriver=(withheld)"),
          "values that could hold a path or user name are never logged");
    Check(Has(deck, "System: memory total 15.2 GiB, available 11.4 GiB, swap 8.0 GiB\n"), "memory from /proc/meminfo");

    const std::string desktop = Report({{{"XDG_CURRENT_DESKTOP", "KDE"}, {"WAYLAND_DISPLAY", "wayland-0"},
                                         {"SteamAppId", "1"}, {"XDG_SESSION_TYPE", "wayland"},
                                         {"VK_INSTANCE_LAYERS", "VK_LAYER_MANGOHUD_overlay_x86_64"}}},
                                       steamos, nullptr);
    Check(Has(desktop, "mode=Desktop Mode (Steam) gamescope=no"), "Steam Desktop Mode");
    Check(Has(desktop, "vk_instance_layers=VK_LAYER_MANGOHUD_overlay_x86_64"), "Vulkan layer names are logged");
    Check(!Has(desktop, "System: memory"), "no memory line without its source");

    const std::string nested = Report({{{"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"}, {"XDG_CURRENT_DESKTOP", "KDE"}}},
                                      "ID=arch\nPRETTY_NAME=\"Arch Linux\"\n", nullptr);
    Check(Has(nested, "mode=nested Gamescope gamescope=yes steam=no"), "Gamescope nested on a desktop");
    Check(Has(nested, "distribution Arch Linux (version unknown), SteamOS no"), "another distribution");
    const std::string bare = Report({}, nullptr, nullptr);
    Check(Has(bare, "mode=desktop") && Has(bare, "session_type=(unset)"), "an empty environment is described, not guessed");
}

// --- launcher_input.h ---------------------------------------------------------------------------------------------
void TestLauncherInput() {
    sos::InputTelemetry input;
    Check(input.last() == sos::InputKind::kNone && std::string(sos::InputKindName(input.last())) == "unknown",
          "no input yet is 'unknown'");
    int lines = 0;
    for (int i = 0; i < 50; ++i) lines += input.Note("gamepad_button", "button=start", sos::InputKind::kGamepad) ? 1 : 0;
    Check(lines == sos::InputTelemetry::kLinesPerEvent, "each input event is logged a bounded number of times");
    Check(input.count("gamepad_button") == 50, "counting goes on after the lines stop");
    Check(input.last() == sos::InputKind::kGamepad, "the last input names the PLAY source");
    const auto touch = input.Note("touch_click", "button=1", sos::InputKind::kTouch);
    Check(touch && *touch == "Launcher input: event=touch_click button=1 count=1", "the line format");
    input.Note("focus_lost");
    Check(input.last() == sos::InputKind::kTouch, "focus changes do not change the input source");
    Check(sos::AllowBackgroundGamepadEvents("gamescope-0", nullptr), "Gamescope allows gamepad events without focus");
    Check(sos::AllowBackgroundGamepadEvents(nullptr, "gamescope"), "Gaming Mode by its desktop name");
    Check(!sos::AllowBackgroundGamepadEvents(nullptr, "KDE") && !sos::AllowBackgroundGamepadEvents("", nullptr) &&
              !sos::AllowBackgroundGamepadEvents(nullptr, nullptr),
          "desktops and Windows keep SDL's focus rule");
    Check(Has(sos::kLauncherHints, "Start") && Has(sos::kLauncherHints, "Enter"), "the footer names Start as PLAY");
}

// --- graphics_probe.h ---------------------------------------------------------------------------------------------
void TestGraphicsProbe(const std::filesystem::path& folder) {
    Check(!sos::GraphicsProbeDue(2, 5.0) && !sos::GraphicsProbeDue(10, 0.2) && sos::GraphicsProbeDue(3, 0.5),
          "the probe waits for three frames and half a second");
    const std::filesystem::path marker = folder / sos::kGpuProbeMarker;
    bool marker_during_probe = false;
    int probes = 0;
    const auto probe = [&] {
        ++probes;
        marker_during_probe = std::filesystem::exists(marker);
        return std::vector<std::string>{"AMD Custom GPU 0405 (RADV VANGOGH)", ""};
    };
    sos::GraphicsCards cards = sos::DetectGraphicsCards(marker, nullptr, probe);
    Check(cards.state == sos::GraphicsProbeState::kProbed && probes == 1 && marker_during_probe && !std::filesystem::exists(marker),
          "the marker exists only while the probe runs");
    Check(cards.names.size() == 2 && std::string(sos::GraphicsCardsText(cards)).empty(), "cards found: no problem text");
    { std::ofstream(marker) << "graphics card detection started\n"; }  // a probe that never returned
    cards = sos::DetectGraphicsCards(marker, nullptr, probe);
    Check(cards.state == sos::GraphicsProbeState::kInterrupted && probes == 1, "an unfinished probe is not repeated at once");
    Check(Has(sos::GraphicsCardsText(cards), "did not finish last time"), "the row says why there is no list");
    cards = sos::DetectGraphicsCards(marker, nullptr, probe);
    Check(cards.state == sos::GraphicsProbeState::kProbed && probes == 2, "the start after that asks again");
    Check(std::string(sos::GraphicsCardsText(sos::GraphicsCards{})) == "Detecting graphics cards...", "before the probe");
    sos::GraphicsCards none;
    none.state = sos::GraphicsProbeState::kProbed;
    Check(std::string(sos::GraphicsCardsText(none)) == "No graphics card found", "no card");
}

// --- session_outcome.h against the shared cases -------------------------------------------------------------------
struct Case {
    std::string name, expect, detail;
    std::string log;
};

std::vector<Case> FixtureCases(const std::filesystem::path& file) {
    std::vector<Case> cases;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    bool any_log = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("=== ")) {
            cases.push_back({line.substr(4), "", "", ""});
            any_log = false;
        } else if (cases.empty()) {
            continue;
        } else if (line.starts_with("expect: ") && cases.back().expect.empty()) {
            cases.back().expect = line.substr(8);
        } else if (line.starts_with("detail: ") && cases.back().detail.empty() && !any_log) {
            cases.back().detail = line.substr(8);
        } else {
            cases.back().log += line + "\n";
            any_log = true;
        }
    }
    return cases;
}

void TestSessionOutcome(const std::filesystem::path& fixtures) {
    const std::vector<Case> cases = FixtureCases(fixtures);
    Check(cases.size() >= 18, "the shared cases were read (" + std::to_string(cases.size()) + ")");
    std::vector<std::string> seen;
    for (const Case& c : cases) {
        const sos::SessionOutcome outcome = sos::AnalyzeSessionOutcome(c.log);
        Check(outcome.key == c.expect, c.name + ": expected " + c.expect + ", got " + outcome.key);
        Check(outcome.label == sos::SessionOutcomeLabel(c.expect), c.name + ": label");
        if (!c.detail.empty()) Check(Has(outcome.detail, c.detail), c.name + ": detail '" + outcome.detail + "'");
        seen.push_back(outcome.key);
    }
    for (std::string_view key : sos::SessionOutcomeKeys()) {
        bool found = false;
        for (const std::string& s : seen) found |= s == key;
        Check(found || key == "unknown", std::string("a shared case covers ") + std::string(key));
    }
    for (const char* stops : {"Launcher: shown\n", "Startup: game-only child process\nStartup: first present +1 ms\n", ""}) {
        const std::string key = sos::AnalyzeSessionOutcome(stops).key;
        Check(key != "crash" && key != "startup_crash", "a log that just stops is never a crash");
    }
}

// --- bug_report.cpp reads the new lines ---------------------------------------------------------------------------
void TestBugReportDiagnostics() {
    const sos::LogDiagnostics linux_crash = sos::AnalyzeLogDiagnostics(
        "Stage: swapchain creation started +6300 ms (after Vulkan device creation, 90 ms)\n"
        "CRASH: signal 11 (SIGSEGV) code 1\nCrash: startup phase: swapchain creation\n"
        "Crash: last completed stage: Vulkan device creation\nCrash: context phase=swapchain creation, render 0x0 () , "
        "video=none, last checkpoint=none\n");
    Check(linux_crash.crash_detected && linux_crash.crash_kind == sos::CrashKind::kGenericCrash,
          "a Linux signal report is a detected crash");
    Check(linux_crash.last_startup_stage == "swapchain creation", "its startup stage comes from the report");

    const sos::LogDiagnostics hang = sos::AnalyzeLogDiagnostics(
        "Launcher: shown\nStage event: PLAY requested input=gamepad +9000 ms\n"
        "Stage: play: graphics preflight started +9040 ms (after play: game files recheck, 40 ms)\n");
    Check(!hang.crash_detected && hang.crash_kind == sos::CrashKind::kNone, "a log that stops is not a crash");
    Check(hang.last_startup_stage == "play: graphics preflight", "the stage in progress is the last startup stage");

    const sos::LogDiagnostics old = sos::AnalyzeLogDiagnostics(
        "Launcher: shown\nLauncher performance: renderer=opengl render_hz=59.3 cpu=7.0%\n");
    Check(!old.crash_detected && old.last_startup_stage.empty(), "an old launcher-only log stays crash-free and stage-less");

    sos::BugReportPackage package;
    package.fields.push_back({"description", "Description", "Dont start."});
    package.fields.push_back({"log_text", "Log after privacy filtering", "Launcher: shown\nUpdater: No update available\n"});
    const std::string preview = sos::BugReportPreviewText(package);
    Check(Has(preview, "Session outcome: Launcher open, no PLAY attempt (the log ends without a closing record"),
          "the preview shows the derived session outcome");
    Check(sos::BugReportRequestJson(package).find("session_outcome") == std::string::npos,
          "the outcome is not sent as a field (the deployed service refuses unknown fields)");
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path fixtures = argc > 1 ? argv[1] : SOS_SOURCE_DIR "/tests/fixtures/session_outcome_cases.txt";
    const std::filesystem::path folder = std::filesystem::temp_directory_path() /
                                         ("sos_session_diag_" + std::to_string(static_cast<unsigned long>(std::rand())) +
                                          std::to_string(static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&failures) & 0xffff)));
    std::filesystem::create_directories(folder);
    TestStageLines(folder);
    TestStageLineShares(folder);
    TestLauncherResultNames();
    TestWatchdog();
    TestSystemReport();
    TestLauncherInput();
    TestGraphicsProbe(folder);
    TestSessionOutcome(fixtures);
    TestBugReportDiagnostics();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::fprintf(stderr, "session diagnostics: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
