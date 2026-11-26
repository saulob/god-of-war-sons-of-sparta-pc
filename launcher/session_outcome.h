// What a session log says about how the session went, for the Bug Report and the admin dashboard: a launcher left
// without PLAY, a PLAY that was refused or failed, a hang or crash while starting, a black screen, a device loss, a
// failed allocation or a normal end. It is derived from the log text alone (never sent as a field, so reports keep
// working with the deployed service, and old reports are classified the same way); server/bug-report/src/
// session_outcome.js implements the same rules and tests/fixtures/session_outcome_cases.txt holds the cases both pass.
// A log that just stops is never called a crash: it is "ended without a closing record" unless a crash was written.
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace sos {

struct SessionOutcome {
    std::string key;     // a stable key, see SessionOutcomeKeys
    std::string label;   // English text for people
    std::string detail;  // the evidence: last stage, closing record
};

inline const std::vector<std::string_view>& SessionOutcomeKeys() {
    static const std::vector<std::string_view> keys = {
        "launcher_no_play",       "play_blocked",      "graphics_preflight_failed", "settings_save_failed",
        "launcher_close_hang",    "runtime_handoff_failed", "startup_hang",          "startup_crash",
        "black_screen",           "device_lost",       "memory_failure",            "crash",
        "normal_exit",            "ended_unexpectedly", "install_failed",           "unknown",
    };
    return keys;
}

inline const char* SessionOutcomeLabel(std::string_view key) {
    if (key == "install_failed") return "Game installation failed";
    if (key == "launcher_no_play") return "Launcher open, no PLAY attempt";
    if (key == "play_blocked") return "PLAY requested but blocked";
    if (key == "graphics_preflight_failed") return "Graphics check failed";
    if (key == "settings_save_failed") return "Settings could not be saved";
    if (key == "launcher_close_hang") return "Hang while the launcher closed";
    if (key == "runtime_handoff_failed") return "Game did not start after the launcher closed";
    if (key == "startup_hang") return "Hang while the game started";
    if (key == "startup_crash") return "Crash while the game started";
    if (key == "black_screen") return "Black screen (frames made, none shown)";
    if (key == "device_lost") return "Vulkan device lost";
    if (key == "memory_failure") return "Memory allocation failure";
    if (key == "crash") return "Crash during play";
    if (key == "normal_exit") return "Normal exit";
    if (key == "ended_unexpectedly") return "Ended without a closing record";
    return "Not enough information";
}

namespace session_outcome_detail {

inline bool Has(std::string_view line, std::string_view text) { return line.find(text) != std::string_view::npos; }

inline std::string StageOf(std::string_view line) {
    // "Stage: <name> started +T ms ..." / "Stage: <name> completed..." / "Stage: <name> failed..."
    std::string_view rest = line.substr(7);
    for (std::string_view end : {" started +", " completed", " failed"}) {
        const size_t at = rest.find(end);
        if (at != std::string_view::npos) return std::string(rest.substr(0, at));
    }
    return std::string(rest);
}

inline std::string ValueOf(std::string_view line, std::string_view key, std::string_view until = " ") {
    const size_t at = line.find(key);
    if (at == std::string_view::npos) return {};
    std::string_view rest = line.substr(at + key.size());
    return std::string(rest.substr(0, std::min(rest.find(until), rest.size())));
}

// The latest installation attempt; a later success or cancel clears its failure.
struct InstallTrace {
    int attempts = 0;
    bool failed = false;
    std::string package, error, stage, progress, operation, win32, path_length;

    void Read(std::string_view line) {
        if (line.starts_with("Launcher: installing the game from ") || line.starts_with("Launcher: installing an update from ")) {
            const int seen = attempts + 1;
            *this = InstallTrace{};
            attempts = seen;
        } else if (line.starts_with("Launcher: package recognized: ")) {
            const size_t is = line.find(" is ");
            if (is != std::string_view::npos) package = std::string(line.substr(is + 4));
        } else if (line.starts_with("Launcher: game install failed: ")) {
            failed = true;
            error = std::string(line.substr(31));
        } else if (line.starts_with("Launcher: install failure: ")) {
            stage = ValueOf(line, " stage=", " progress=");
            progress = ValueOf(line, " progress=");
        } else if (line.starts_with("Launcher: install filesystem error: ")) {
            const std::string_view rest = line.substr(36);
            operation = std::string(rest.substr(0, std::min(rest.find(" failed:"), rest.size())));
            win32 = ValueOf(line, " win32=", "\n");
            path_length = ValueOf(line, " path_length=");
        } else if (line.starts_with("Launcher: game installed: ") || line.starts_with("Launcher: game already up to date") ||
                   line == "Launcher: game install cancelled") {
            failed = false;
        }
    }

    std::string Detail() const {
        std::string out;
        const auto add = [&out](const std::string& part) {
            if (part.empty()) return;
            out += (out.empty() ? "" : "; ") + part;
        };
        add(package);
        add(error);
        if (!stage.empty() && stage != "-") add("stage: " + stage + (progress.empty() || progress == "unknown" ? "" : " at " + progress));
        if (!operation.empty()) {
            add(operation + " failed" + (win32.empty() ? "" : ", win32=" + win32) +
                (path_length.empty() ? "" : ", path_length=" + path_length));
        }
        if (attempts > 1) add(std::to_string(attempts) + " install attempts");
        return out;
    }
};

inline bool IsMemoryLine(std::string_view line) {
    return Has(line, "ErrorOutOfDeviceMemory") || Has(line, "ErrorOutOfHostMemory") || Has(line, "OUT_OF_DEVICE_MEMORY") ||
           Has(line, "OUT_OF_HOST_MEMORY") || Has(line, "Out of memory");
}

} // namespace session_outcome_detail

inline SessionOutcome AnalyzeSessionOutcome(std::string_view log) {
    using namespace session_outcome_detail;
    bool launcher_shown = false, play = false, play_starting = false, launcher_closed = false, handoff = false;
    bool game_child = false, first_present = false, first_flip = false, normal_exit = false, quit_requested = false;
    bool crash = false, fatal_stop = false, memory_near_end = false, black_screen = false, device_lost = false;
    bool watchdog = false, ended_by_signal = false;
    // A closing record counts for the game only when it comes after the handoff: "Launcher: closed, starting the game"
    // is in every in-process start. Logs with no Stage/Session line come from before 0.1.1, which wrote no closing
    // record at all, so their silence proves nothing.
    bool game_quit = false, modern = false;
    std::string blocked;  // the last PLAY refusal not followed by a start
    std::string last_stage, watchdog_stage, signal_line;
    std::string trace;  // the GPU trace summary of a device loss or GPU assertion, without its prefix
    std::vector<std::string> recent;  // the last lines before a crash, for the memory check
    InstallTrace install;

    for (size_t at = 0; at < log.size();) {
        const size_t end = std::min(log.find('\n', at), log.size());
        std::string_view line = log.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;

        install.Read(line);

        if (line.starts_with("Stage: ")) last_stage = StageOf(line);
        if (line.starts_with("Stage") || line.starts_with("Session: ")) modern = true;
        if (line == "Launcher: shown") launcher_shown = true;
        if (line.starts_with("Stage event: PLAY requested") || line == "Launcher: settings saved, starting the game") {
            play = true;
            blocked.clear();
        }
        if (line.starts_with("Stage: play: starting started") || line == "Launcher: settings saved, starting the game") {
            play_starting = true;
        }
        if (line.starts_with("Stage event: PLAY blocked: ")) {
            blocked = std::string(line.substr(27));
            const size_t plus = blocked.rfind(" +");
            if (plus != std::string::npos) blocked.resize(plus);
        }
        if (line.starts_with("Stage: play: settings save failed")) blocked = "settings save failed";
        if (line.starts_with("Launcher: closed")) launcher_closed = true;
        if (Has(line, "quit requested by the system") || line == "Launcher: window close requested") {
            quit_requested = true;
            if (handoff) game_quit = true;
        }
        if (line.starts_with("Stage event: Runtime handoff") || line.starts_with("Startup: game-only child process") ||
            line.starts_with("Launcher: the game process continues this log") ||
            line.starts_with("Overlay compatibility: ")) {
            handoff = true;
        }
        if (line.starts_with("Startup: game-only child process")) game_child = true;
        // Play starts with the first frame of the game itself ("Runtime: first present", also in 0.1.0 logs), not with
        // "Startup: first present", which is the renderer's loading screen before the guest even starts.
        if (line == "Runtime: first present" || line.starts_with("Stage: game running started")) first_present = true;
        if (line.starts_with("Runtime: first flip taken") || line.starts_with("Runtime: first flip prepared")) first_flip = true;
        if (line.starts_with("Session: normal exit")) normal_exit = true;
        if (line.starts_with("Session: ended by signal")) {
            ended_by_signal = true;
            signal_line = std::string(line);
        }
        if (line.starts_with("Presenter diagnostic: no successful present")) black_screen = true;
        if (line.starts_with("Presenter diagnostic: presenting resumed")) black_screen = false;
        if (line.starts_with("GPU device lost: first detected in ")) device_lost = true;
        if (line.starts_with("Stage watchdog: ")) {
            watchdog = true;
            const std::string_view rest = line.substr(16);
            watchdog_stage = std::string(rest.substr(0, rest.find(" still running")));
        }
        const bool fatal_line = line.starts_with("CRASH:") || line.starts_with("STOP: ");
        if (fatal_line) {
            for (const std::string& before : recent) memory_near_end |= IsMemoryLine(before);
            memory_near_end |= IsMemoryLine(line);
            if (line.starts_with("CRASH:")) crash = true;
            else fatal_stop = true;
        }
        if (trace.empty() && line.starts_with("GPU trace: summary: ")) trace = std::string(line.substr(20));
        // The GPU trace (up to 1000 lines) is printed between the failure and its stop line: it is not evidence of its
        // own and must not push the failure out of the window.
        if (line.starts_with("GPU trace: ")) continue;
        recent.emplace_back(line);
        if (recent.size() > 40) recent.erase(recent.begin());
    }
    if (!crash && !fatal_stop) {
        for (const std::string& line : recent) memory_near_end |= IsMemoryLine(line) && Has(line, "fatal");
    }

    const bool closed_record = normal_exit || launcher_closed || quit_requested || ended_by_signal;
    std::string where = last_stage.empty() ? std::string() : "last stage: " + last_stage;
    const auto with = [&](std::string_view key, std::string detail) {
        return SessionOutcome{std::string(key), SessionOutcomeLabel(key), std::move(detail)};
    };
    const std::string no_record = "the log ends without a closing record (the process was killed, hung or ended "
                                  "without a report)";
    const auto joined = [](std::string a, const std::string& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        return a + "; " + b;
    };

    const std::string traced = trace.empty() ? std::string() : "GPU trace: " + trace;
    if (device_lost) return with("device_lost", joined(where, traced));
    if ((crash || fatal_stop) && memory_near_end) return with("memory_failure", joined(where, traced));
    if (crash || fatal_stop) {
        const bool starting = !first_present;
        return with(starting ? "startup_crash" : "crash",
                    joined(joined(where, fatal_stop && !crash ? "fatal stop" : "crash report written"), traced));
    }
    if (black_screen) return with("black_screen", where);
    if (memory_near_end) return with("memory_failure", where);

    const bool game = handoff || game_child || first_present;
    if (!game) {
        if (install.failed && !play_starting) return with("install_failed", install.Detail());
        if (!launcher_shown && !play) return with("unknown", joined(where, closed_record ? "" : no_record));
        if (!play) {
            return with("launcher_no_play", joined(where, closed_record ? "the launcher was closed" : no_record));
        }
        if (!blocked.empty() && !play_starting) {
            if (blocked.starts_with("graphics preflight")) return with("graphics_preflight_failed", blocked);
            if (blocked.starts_with("settings save")) return with("settings_save_failed", blocked);
            return with("play_blocked", blocked);
        }
        if (play_starting && !launcher_closed) {
            return with("launcher_close_hang", joined(where, watchdog ? "watchdog: " + watchdog_stage : no_record));
        }
        if (launcher_closed) return with("runtime_handoff_failed", joined(where, closed_record ? "" : no_record));
        return with("play_blocked", joined(where, "PLAY did not start the game"));
    }
    const bool game_closed = normal_exit || game_quit || ended_by_signal;
    if (normal_exit) return with("normal_exit", where);
    if (!first_present) {
        if (watchdog || !game_closed) {
            return with(first_flip ? "black_screen" : "startup_hang",
                        joined(where, watchdog ? "watchdog: " + watchdog_stage : no_record));
        }
        return with("startup_hang", joined(where, ended_by_signal ? signal_line : "closed before the first frame"));
    }
    if (ended_by_signal) return with("ended_unexpectedly", joined(where, signal_line));
    if (game_quit) return with("normal_exit", joined(where, "quit requested by the system"));
    if (!modern) return with("unknown", joined(where, "a log from a version before 0.1.1, which wrote no closing record"));
    return with("ended_unexpectedly", joined(where, no_record));
}

} // namespace sos
