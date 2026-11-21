// Launcher robustness on the production helpers: placement and its diagnostics, startup stages and the show gate, Escape
// ownership, and the Bug Report session and its card, with synthetic displays and time. No window, GPU, network, launcher
// or game.
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_scancode.h>

#include "bug_report_session.h"
#include "launcher_escape.h"
#include "launcher_startup.h"
#include "sos_keys.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

using namespace sos;

namespace {

int g_checks = 0;
int g_failures = 0;
int g_placement = 0, g_startup = 0, g_escape = 0, g_report = 0;
int* g_group = &g_placement;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    ++*g_group;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

std::string Source(const char* path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    std::string unix_text;
    for (const char c : text) {
        if (c != '\r') {
            unix_text += c;
        }
    }
    return unix_text;
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int Count(const std::string& text, const std::string& part) {
    int count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

bool Before(const std::string& text, const std::string& first, const std::string& second) {
    const size_t a = text.find(first);
    const size_t b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t a = text.find(from);
    if (a == std::string::npos) {
        return "";
    }
    const size_t b = text.find(to, a + from.size());
    return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

DisplayFacts Display(const char* name, int x, int y, int w, int h, int usable_h, float scale, bool primary = false) {
    return {name, {x, y, x + w, y + h}, {x, y, x + w, y + usable_h}, scale, primary};
}

PlacementFacts Facts(int x, int y, int w, int h, float scale) {
    PlacementFacts facts;
    facts.window = {x, y, x + w, y + h};
    facts.pixel_width = int(w * scale);
    facts.pixel_height = int(h * scale);
    facts.pixel_density = scale;
    facts.content_scale = scale;
    return facts;
}

void TestPlacement() {
    g_group = &g_placement;
    const std::vector<DisplayFacts> single = {Display("Main", 0, 0, 1920, 1080, 1040, 1.0f, true)};
    const std::vector<DisplayFacts> pair = {Display("Left", 0, 0, 1920, 1080, 1040, 1.5f, true),
                                            Display("Right", 1920, 0, 2560, 1440, 1400, 1.0f)};
    const std::vector<DisplayFacts> negative = {Display("Main", 0, 0, 1920, 1080, 1040, 1.0f, true),
                                                Display("Side", -2560, -200, 2560, 1440, 1400, 2.25f)};
    {
        const std::string line = PlacementLine("shown", Facts(100, 80, 960, 720, 1.0f), single);
        Check(Has(line, "x=100 y=80 size=960x720"), "the line holds the window position and size");
        Check(Has(line, "display=1/1") && Has(line, "name=\"Main\""), "the line holds the display index, count and name");
        Check(Has(line, " scale=1.00 pixels=") && Has(line, "density=1.00") && Has(line, "content_scale=1.00"), "the line holds the scale at 100 percent");
        Check(Has(line, "bounds=0,0,1920x1080") && Has(line, "usable=0,0,1920x1040"), "the line holds the display bounds and work area");
        Check(Has(line, "stage=shown") && Has(line, "pixels=960x720"), "the line holds the stage and the pixel size");
    }
    {
        const std::string line = PlacementLine("shown", Facts(2200, 100, 1280, 720, 1.0f), pair);
        Check(Has(line, "display=2/2") && Has(line, "name=\"Right\""), "a window on the secondary monitor says display 2");
    }
    {
        const std::string line = PlacementLine("shown", Facts(-1500, 200, 960, 720, 2.25f), negative);
        Check(Has(line, "x=-1500 y=200") && Has(line, "display=2/2"), "negative coordinates find the monitor on the left");
        Check(Has(line, " scale=2.25 pixels=2160x1620"), "225 percent scale is reported with its pixel size");
    }
    Check(Has(PlacementLine("shown", Facts(0, 0, 960, 720, 1.5f), pair), " scale=1.50 pixels=1440x1080"), "150 percent scale is reported");
    {
        const std::string line = PlacementLine("created", Facts(300, 1060, 800, 500, 1.0f), single);
        Check(Has(line, "display=1/1") && Has(line, "usable=0,0,1920x1040"), "a window across the taskbar stays on its display and shows the smaller work area");
        const std::vector<Rect> work = {single[0].usable};
        WindowPlacement under{{300, 1030, 1100, 1530}, false, false};
        Check(!PlacementIsVisible(under, work), "a saved window below the work area is not visible");
        WindowPlacement partly{{300, 600, 1100, 1100}, false, false};
        Check(PlacementIsVisible(partly, work), "a saved window mostly above the taskbar is visible");
    }
    {
        const std::string line = PlacementLine("shown", Facts(0, 0, 960, 720, 1.0f), {});
        Check(Has(line, "display=none/0") && Has(line, "x=0 y=0"), "missing display information is stated, not guessed");
        const std::string off = PlacementLine("shown", Facts(9000, 9000, 960, 720, 1.0f), single);
        Check(Has(off, "display=none/1"), "a window on no display says so");
        PlacementFacts straddle = Facts(1000, 100, 1500, 700, 1.0f);
        Check(DisplayIndexOf(straddle.window, pair) == 1, "a window straddling two displays follows its centre");
        straddle.window = {1700, 100, 2700, 800};
        Check(DisplayIndexOf(straddle.window, pair) == 2, "a window with its centre on the second display is on it");
    }
    {
        PlacementFacts facts = Facts(100, 80, 960, 720, 1.0f);
        facts.saved = SavedPlacement::kStale;
        facts.maximized = true;
        const std::string line = PlacementLine("created", facts, single);
        Check(Has(line, "saved=stale") && Has(line, "maximized=1") && Has(line, "fullscreen=0"), "a corrected stale placement and the window state are named");
        facts.saved = SavedPlacement::kRestored;
        Check(Has(PlacementLine("shown", facts, single), "saved=restored"), "a restored placement is named");
        Check(Has(PlacementLine("shown", Facts(1, 2, 300, 300, 1.0f), single), "saved=none"), "no saved placement is named");
        const std::vector<DisplayFacts> hostile = {Display("A\"B\x01 very long monitor name that goes on and on and on and on and on and on and on and on and on and on", 0, 0, 1920, 1080, 1040, 1.0f)};
        const std::string bounded = PlacementLine("shown", Facts(10, 10, 800, 600, 1.0f), hostile);
        Check(bounded.size() < 400 && !Has(bounded, "\x01") && Count(bounded, "\"") == 2, "a long or hostile monitor name stays bounded and quoted once");
        Check(!Has(bounded, "title") && !Has(line, "user"), "no window title or user data is in the line");
    }
    {
        const std::vector<Rect> work = {{0, 0, 1920, 1040}};
        WindowPlacement stale{{5000, 5000, 5900, 5700}, true, true};
        const WindowPlacement kept = RestorablePlacement(stale, work);
        Check(!PlacementIsSet(kept) && kept.maximized && kept.fullscreen, "an off-screen rectangle is dropped but maximized and fullscreen stay");
        WindowPlacement fine{{100, 100, 1000, 800}, true, false};
        Check(RestorablePlacement(fine, work) == fine, "a visible placement is kept whole");
        Check(!PlacementIsSet(RestorablePlacement(WindowPlacement{}, work)), "an unset placement stays unset");
        const std::vector<Rect> second = {{0, 0, 1920, 1040}, {-2560, -200, 0, 1200}};
        WindowPlacement on_left{{-2000, 0, -1000, 700}, false, false};
        Check(RestorablePlacement(on_left, second).normal == on_left.normal, "a rectangle on a monitor at negative coordinates is restored");
    }
    {
        PlacementTracker tracker(WindowPlacement{{100, 100, 1000, 800}, true, false});
        WindowState minimized;
        minimized.minimized = true;
        minimized.maximized = false;
        minimized.rect = {-32000, -32000, -31000, -31200};
        tracker.Observe(minimized, false, 1.0);
        Check(tracker.Current()->maximized && tracker.Current()->normal == Rect({100, 100, 1000, 800}), "minimizing keeps the maximized flag and the rectangle");
        WindowState normal;
        normal.rect = {50, 60, 950, 760};
        tracker.Observe(normal, false, 2.0);
        Check(!tracker.Current()->maximized && tracker.Current()->normal == normal.rect, "restoring to a normal window records it");
    }
    {
        const std::string host = Source("launcher/host.cpp");
        const std::string frame = Between(host, "void LauncherHost::BeginFrame()", "bool LauncherHost::Fullscreen()");
        Check(Count(host, "LogPlacement(\"") == 2 && !Has(frame, "LogPlacement") && !Has(frame, "ListDisplays(") && !Has(frame, "SDL_GetDisplay"),
              "placement is logged twice at startup and never per frame");
        Check(Has(host, "restored_ = RestorablePlacement(*options.restore, usable);") && Has(host, "SDL_SyncWindow(window_);"),
              "the host restores through RestorablePlacement and syncs after a fullscreen change");
        Check(Count(host, "ListDisplays()") == 1, "the displays are listed once per window");
    }
}

void TestStartup() {
    g_group = &g_startup;
    {
        StartupTimeline timeline(1000 * kNsPerMs);
        const StartupStage order[] = {StartupStage::kSdlReady, StartupStage::kDisplaySelected, StartupStage::kWindowCreated,
                                      StartupStage::kRendererReady, StartupStage::kUiReady, StartupStage::kPagesReady,
                                      StartupStage::kFirstFramePresented, StartupStage::kShown, StartupStage::kLoopActive};
        int64_t now = 1000 * kNsPerMs;
        bool all = true;
        for (const StartupStage stage : order) {
            now += 40 * kNsPerMs;
            const auto reached = timeline.Reach(stage, now);
            all = all && reached && reached->took_ns == 40 * kNsPerMs && !reached->slow && timeline.Reached(stage);
        }
        Check(all, "a normal startup records every stage with its duration and flags none");
        Check(timeline.TotalNs() == 9 * 40 * kNsPerMs && *timeline.Last() == StartupStage::kLoopActive, "the total and the last stage follow the stages");
        const std::string line = timeline.SummaryLine("software");
        Check(Has(line, "total=360ms") && Has(line, "renderer=software") && Has(line, "sdl=40") && Has(line, "first_present=40") && Has(line, "loop=40"),
              "the summary lists the renderer and each stage");
        Check(Before(line, "sdl=", "display=") && Before(line, "renderer=software ", "ui=") && Before(line, "ui=", "first_present="),
              "the stages are in order in the summary");
        Check(!timeline.Reach(StartupStage::kShown, now + 5), "a stage is recorded once");
    }
    {
        StartupTimeline timeline(0);
        timeline.Reach(StartupStage::kRendererReady, 10 * kNsPerMs);
        Check(!timeline.Reach(StartupStage::kWindowCreated, 20 * kNsPerMs), "a stage earlier than the last one is ignored");
        Check(*timeline.Last() == StartupStage::kRendererReady && !timeline.Reached(StartupStage::kWindowCreated), "an ignored stage is not recorded");
    }
    {
        StartupTimeline timeline(0);
        timeline.Reach(StartupStage::kSdlReady, 5 * kNsPerMs);
        timeline.Reach(StartupStage::kWindowCreated, 30 * kNsPerMs);
        timeline.Reach(StartupStage::kRendererReady, 60 * kNsPerMs);
        Check(!timeline.Reached(StartupStage::kFirstFramePresented) && *timeline.Last() == StartupStage::kRendererReady,
              "startup that stops before the first present names the renderer stage as the last one");
        Check(std::string(StartupStagePhase(*timeline.Last())) == "launcher renderer ready", "the last stage is a crash-report phase");
        Check(!timeline.Reached(StartupStage::kShown), "nothing was shown");
    }
    {
        StartupTimeline timeline(0);
        const auto fast = timeline.Reach(StartupStage::kSdlReady, kSlowStageNs - 1);
        Check(fast && !fast->slow, "a stage just under the threshold is not slow");
        const auto slow = timeline.Reach(StartupStage::kRendererReady, kSlowStageNs - 1 + kSlowStageNs);
        Check(slow && slow->slow && slow->took_ns == kSlowStageNs, "a stage at the threshold is slow");
        const std::string line = timeline.SlowLine(*slow, "software", true);
        Check(Has(line, "stage=renderer") && Has(line, "took=2000ms") && Has(line, "renderer=software") && Has(line, "minimized=1"),
              "the slow line names the stage, the time, the renderer and the minimized state");
        Check(kSlowStageNs >= 2 * kNsPerSecond, "slow startup is only reported from two seconds");
        const auto backwards = StartupTimeline(100).Reach(StartupStage::kSdlReady, 50);
        Check(backwards && backwards->took_ns == 0 && !backwards->slow, "a clock that ran backwards gives a zero duration");
    }
    {
        ShowGate gate;
        gate.Opened(false);
        Check(!gate.TryShow(), "nothing is shown before a renderer exists");
        gate.FramePresented();
        Check(!gate.TryShow(), "a present without a renderer does not open the gate");
        gate.RendererReady();
        Check(!gate.TryShow(), "a renderer alone does not show the window");
        gate.FramePresented();
        Check(gate.TryShow() && gate.shown(), "the window is shown after the first presented frame");
        int shows = 0;
        for (int frame = 0; frame < 1000; ++frame) {
            shows += gate.TryShow() ? 1 : 0;
        }
        Check(shows == 0, "later frames never show the window again");
        ShowGate hidden;
        hidden.Opened(true);
        hidden.RendererReady();
        hidden.FramePresented();
        Check(!hidden.TryShow() && !hidden.shown(), "the hidden screenshot mode is never shown");
    }
    {
        LoopPacer pacer;
        Check(pacer.Next(5 * kNsPerSecond, false).kind == StepKind::kDraw, "the first frame after the window is shown is drawn at once");
        LoopPacer restored;
        restored.Next(0, false);
        restored.Next(kLauncherFrameNs, false);
        restored.Next(kLauncherFrameNs + 1, true);
        Check(restored.Next(kLauncherFrameNs + 2, false).kind == StepKind::kDraw, "the first frame after a restore is not delayed");
    }
    {
        const std::string host = Source("launcher/host.cpp");
        const std::string launcher = Source("launcher/launcher.cpp");
        const std::string open = Between(host, "bool LauncherHost::Open(", "void LauncherHost::Reach(");
        Check(Has(open, "SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true"), "the window is created hidden");
        Check(Before(open, "SDL_CreateWindowWithProperties", "CreateLauncherRenderer(") && Before(open, "CreateLauncherRenderer(", "ImGui_ImplSDLRenderer3_Init"),
              "the window, then the renderer, then the UI are created");
        Check(Count(host, "SDL_ShowWindow(") == 1 && Count(host, "SDL_RaiseWindow(") == 1 && !Has(open, "SDL_ShowWindow("), "the window is shown in one place, never in Open");
        const std::string show = Between(host, "void LauncherHost::Show() {", "ImTextureID LauncherHost::CreateTexture");
        Check(Has(show, "!show_gate_.TryShow()") && Before(show, "TryShow", "SDL_ShowWindow(") && Has(host, "show_gate_.FramePresented();") &&
                  Before(host, "SDL_RenderPresent(renderer_);", "show_gate_.FramePresented();"),
              "Show goes through the gate, which opens after SDL_RenderPresent");
        Check(Has(host, "show_gate_.RendererReady();") && Before(host, "ConfigureVsync();", "show_gate_.RendererReady();"), "the gate learns about the renderer after it is configured");
        Check(Has(launcher, "host_.Reach(StartupStage::kPagesReady);\n        host_.BeginFrame();\n        outcome = Frame();\n        host_.EndFrame(LauncherTheme().bg);\n        host_.Show();"),
              "the main launcher builds its pages and its first frame with the theme background before it is shown");
        Check(Has(launcher, "dialog.Draw(fonts, theme);\n    host.EndFrame(theme.bg);\n    host.Show();") && Has(launcher, "draw();\n            host.EndFrame(theme.bg);\n            host.Show();"),
              "the error and preparation windows also present a first frame before they are shown");
        bool themed = true;
        int ends = 0;
        for (size_t at = launcher.find("EndFrame("); at != std::string::npos; at = launcher.find("EndFrame(", at + 1)) {
            ++ends;
            const std::string argument = launcher.substr(at + 9, 24);
            themed = themed && (argument.rfind("LauncherTheme().bg)", 0) == 0 || argument.rfind("theme.bg)", 0) == 0);
        }
        Check(ends == 6 && themed && Count(launcher, ".Show()") == 3, "every launcher frame clears with the theme background and each window is shown once");
        Check(Count(host, "StartupStage::k") >= 9 && Has(host, "sos_startup_phase(StartupStagePhase(stage));") && Has(host, "monitor_.renderer()"),
              "stages are crash-report phases and share the Lote 6 renderer name");
        Check(Count(host, "SDL_GetTicksNS") == 1 && Has(host, "startup_ = StartupTimeline(NowNs());") && Has(host, "startup_.Reach(stage, NowNs())"),
              "startup uses the one clock of the launcher loop");
        Check(Has(host, "Reach(StartupStage::kLoopActive);") && Has(host, "startup_.SummaryLine(monitor_.renderer())"), "one startup summary is logged when the loop starts");
    }
}

void TestEscape() {
    g_group = &g_escape;
    {
        EscapeArbiter escape;
        escape.BeginFrame(true);
        Check(escape.Take() && !escape.Take() && escape.Pressed() && escape.Taken(), "one Escape press is claimed by exactly one owner");
        Check(!escape.QuitAllowed(), "a claimed Escape never closes the launcher");
        escape.BeginFrame(false);
        Check(!escape.Take() && !escape.Pressed() && !escape.Taken(), "without a press nothing can claim it");
        Check(escape.QuitAllowed(), "the Quit button and Alt+F4 work without Escape");
        escape.BeginFrame(true);
        Check(escape.Take(), "each frame has its own press");
    }
    {
        EscapeArbiter escape;
        escape.BeginFrame(true);
        Check(!escape.QuitAllowed(), "a normal page with Escape does not close the launcher");
        escape.BeginFrame(true);
        const bool dialog = escape.Take();
        const bool second = escape.Take();
        Check(dialog && !second && !escape.QuitAllowed(), "a dialog takes Escape and the launcher stays open");
        escape.BeginFrame(true);
        const bool nested = escape.Take();
        const bool outer = escape.Take();
        Check(nested && !outer, "a nested dialog closes alone, its parent does not also close");
        escape.BeginFrame(true);
        const bool rebind_cancel_quit = escape.QuitAllowed();
        Check(!rebind_cancel_quit, "a rebind cancelled by Escape does not also close the launcher");
        escape.BeginFrame(true);
        const bool settings_quit = escape.QuitAllowed();
        Check(!settings_quit, "Escape on the settings page does not close the launcher");
    }
    {
        char buffer[64] = "";
        std::string backup;
        const char* typed[] = {"d", "de", "des", "desc", "describe the bug"};
        for (const char* text : typed) {
            std::strcpy(buffer, text);
            KeepTextOnEscape(buffer, sizeof(buffer), backup, false, true);
        }
        std::strcpy(buffer, "");
        Check(KeepTextOnEscape(buffer, sizeof(buffer), backup, true, true) && std::string(buffer) == "describe the bug",
              "Escape in the Bug Report field keeps what was typed");
        std::strcpy(buffer, "kept");
        backup = "stale";
        Check(!KeepTextOnEscape(buffer, sizeof(buffer), backup, true, false) && std::string(buffer) == "kept" && backup == "kept",
              "Escape while the field is not being edited changes nothing");
        std::strcpy(buffer, "abc");
        backup = "abc";
        Check(!KeepTextOnEscape(buffer, sizeof(buffer), backup, true, true) && std::string(buffer) == "abc", "Escape with nothing lost is a no-op");
        char tiny[4];
        std::strcpy(tiny, "x");
        backup = "toolongtext";
        KeepTextOnEscape(tiny, sizeof(tiny), backup, true, true);
        Check(std::strlen(tiny) == 3, "a restored text never overruns the buffer");
    }
    Check(sos_key_bindable(SDL_SCANCODE_ESCAPE) == 0 && sos_key_bindable(SDL_SCANCODE_J) != 0, "Escape cannot be bound as a key, other keys can");
    {
        const std::string launcher = Source("launcher/launcher.cpp");
        const std::string page = Source("launcher/bug_report_page.cpp");
        const std::string host = Source("launcher/host.cpp");
        Check(Has(launcher, "if (!play_.starting() && LauncherEscape().QuitAllowed()) {"), "the TRG quit request is dropped when it coincides with Escape");
        Check(Count(launcher, "ImGuiKey_Escape") == 0 && Count(Source("launcher/problem_dialog.cpp"), "ImGuiKey_Escape") == 0 &&
                  Count(Source("launcher/trophy_page.cpp"), "ImGuiKey_Escape") == 0 && Count(Source("launcher/package_browser.cpp"), "ImGuiKey_Escape") == 0 &&
                  Count(page, "ImGuiKey_Escape") == 0,
              "no launcher layer reads Escape by itself: they use the arbiter");
        Check(Count(host, "ImGuiKey_Escape") == 1 && Has(host, "LauncherEscape().BeginFrame(ImGui::IsKeyPressed(ImGuiKey_Escape, false));"), "Escape is read once per frame, in the host");
        Check(Has(Source("launcher/problem_dialog.cpp"), "LauncherEscape().Take()") && Has(Source("launcher/trophy_page.cpp"), "LauncherEscape().Take()") &&
                  Has(Source("launcher/package_browser.cpp"), "LauncherEscape().Take()"),
              "dialogs claim Escape: the error dialog, the trophy modals and the package chooser's Cancel");
        Check(Has(page, "KeepTextOnEscape(description_, sizeof(description_), description_backup_, LauncherEscape().Pressed(), description_active_)") &&
                  Has(page, "description_active_ = ImGui::IsItemActive();"),
              "the Bug Report field keeps its text on Escape");
        Check(!Has(page, "Cancel") && !Has(page, ".Take()") && !Has(Source("launcher/bug_report_session.h"), "Cancel") && !Has(Source("launcher/bug_report_session.h"), "Escape"),
              "Escape cancels nothing in the Bug Report: no upload or report is abandoned by it");
    }
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool released = false;

    void Reach() {
        std::lock_guard lock(mutex);
        reached = true;
        changed.notify_all();
    }
    void WaitReached() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return reached; });
    }
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
    void WaitRelease() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return released; });
    }
    void Reset() {
        std::lock_guard lock(mutex);
        reached = released = false;
    }
};

struct Script {
    Gate gate;
    BugReportReceipt receipt;
    bool accept = true;
    uint64_t dump_sent = 0, dump_total = 0;
    CrashUpload retry_outcome = CrashUpload::kUploaded;
    int file_calls = 0;
    int upload_calls = 0;
    std::vector<std::thread> threads;

    SessionHooks Hooks() {
        SessionHooks hooks;
        hooks.file = [this](const BugReportServiceConfig&, const BugReportPackage&, const BugReportProgressSink& progress) {
            ++file_calls;
            if (accept) progress({BugReportProgress::Kind::kTextAccepted, receipt.ticket, 0, 0});
            if (dump_total) progress({BugReportProgress::Kind::kDumpBytes, receipt.ticket, dump_sent, dump_total});
            gate.Reach();
            gate.WaitRelease();
            return receipt;
        };
        hooks.upload = [this](const BugReportServiceConfig&, const std::string& ticket, const std::string&, const CrashDump&,
                              const BugReportProgressSink& progress) {
            ++upload_calls;
            progress({BugReportProgress::Kind::kDumpBytes, ticket, 100, 400});
            gate.Reach();
            gate.WaitRelease();
            return retry_outcome;
        };
        hooks.run = [this](std::function<void()> job) { threads.emplace_back(std::move(job)); };
        return hooks;
    }

    void Finish() {
        gate.Release();
        for (std::thread& thread : threads) thread.join();
        threads.clear();
        gate.Reset();
    }
};

std::shared_ptr<BugReportPackage> Package(bool with_dump) {
    auto package = std::make_shared<BugReportPackage>();
    if (with_dump) {
        package->crash_dump = CrashDump{"a.dmp", "a.dmp", 400};
        package->crash_dump_requested = true;
    }
    return package;
}

BugReportReceipt Sent(CrashUpload crash, const char* credential = "0123456789abcdef0123456789abcdef") {
    BugReportReceipt receipt;
    receipt.outcome = BugReportOutcome::kSent;
    receipt.ticket = "SOS-1234";
    receipt.credential = credential;
    receipt.crash = crash;
    return receipt;
}

bool Leaks(const ReportView& view, const ReportCard& card, const std::string& secret) {
    return Has(view.ticket, secret) || Has(card.title, secret) || Has(card.detail, secret) || Has(card.dump_text, secret);
}

void TestReport() {
    g_group = &g_report;
    const BugReportServiceConfig config;
    const std::string secret = "0123456789abcdef0123456789abcdef";
    {
        Script script;
        BugReportSession session(script.Hooks());
        const ReportView idle = session.View();
        Check(idle.phase == ReportPhase::kIdle && !idle.busy && CardFor(idle, false).tone == CardTone::kNone, "an idle page shows no card and can send");
    }
    {
        Script script;
        script.accept = false;
        script.receipt = Sent(CrashUpload::kNotRequested);
        BugReportSession session(script.Hooks());
        Check(session.Submit(Package(false), config), "a report is submitted");
        script.gate.WaitReached();
        const ReportView view = session.View();
        const ReportCard card = CardFor(view, false);
        Check(view.phase == ReportPhase::kSendingText && view.busy && view.ticket.empty(), "before the service answers there is no ticket and nothing is called sent");
        Check(card.tone == CardTone::kBusy && std::string(card.title) == "Sending your report..." && !card.copy_ticket, "submitting shows a bounded busy card");
        Check(!session.Submit(Package(false), config) && !session.Submit(Package(false), config), "a second submit while one is running is blocked");
        script.Finish();
        Check(script.file_calls == 1, "repeated clicks send one report");
        Check(session.View().phase == ReportPhase::kSendingText, "a finished report is not applied until it is collected");
        Check(session.Collect() == Collected::kDelivery && session.Collect() == Collected::kNothing, "a result is applied once");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kNotRequested);
        BugReportSession session(script.Hooks());
        session.Submit(Package(false), config);
        script.Finish();
        session.Collect();
        const ReportView view = session.View();
        const ReportCard card = CardFor(view, false);
        Check(view.phase == ReportPhase::kSent && !view.busy && !view.can_retry_dump, "a report without a dump is simply sent");
        Check(card.tone == CardTone::kReady && std::string(card.title) == "Report sent successfully" && Has(card.detail, "SOS-1234") && card.copy_ticket &&
                  card.dump == DumpLine::kNone && !card.retry_dump,
              "the success card shows the ticket and offers to copy it");
        Check(session.View().ticket == "SOS-1234" && session.View().ticket == view.ticket, "the ticket stays visible after success");
        Check(!Leaks(view, card, secret), "the receipt is not part of what is shown");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kUploaded);
        script.dump_sent = 1000;
        script.dump_total = 4000;
        BugReportSession session(script.Hooks());
        session.Submit(Package(true), config);
        script.gate.WaitReached();
        const ReportView view = session.View();
        const ReportCard card = CardFor(view, false);
        Check(view.phase == ReportPhase::kUploadingDump && view.ticket == "SOS-1234" && view.busy, "once the text is accepted the report is sent and the dump is uploading");
        Check(card.tone == CardTone::kReady && std::string(card.title) == "Report sent successfully" && card.dump == DumpLine::kUploading &&
                  std::string(card.dump_text) == "Uploading the crash report...",
              "the card says the report was sent and the crash report is uploading");
        Check(card.dump_progress > 0.249f && card.dump_progress < 0.251f, "the dump progress is the bytes the service confirmed");
        Check(!session.Submit(Package(true), config) && !session.RetryDump(config), "nothing else starts during the upload");
        Check(!Leaks(view, card, secret), "the upload state does not show the receipt");
        script.Finish();
        session.Collect();
        const ReportView done = session.View();
        const ReportCard done_card = CardFor(done, session.DumpUploaded());
        Check(done.phase == ReportPhase::kSent && done_card.dump == DumpLine::kUploaded && std::string(done_card.dump_text) == "The crash report was uploaded with it.",
              "a finished dump upload is reported");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kFailed);
        script.dump_total = 4000;
        BugReportSession session(script.Hooks());
        session.Submit(Package(true), config);
        script.Finish();
        session.Collect();
        const ReportView view = session.View();
        const ReportCard card = CardFor(view, false);
        Check(view.phase == ReportPhase::kSentDumpFailed && view.can_retry_dump, "a failed dump after an accepted report is a partial success");
        Check(card.tone == CardTone::kReady && std::string(card.title) == "Report sent successfully" && Has(card.detail, "SOS-1234"), "the card still says the report was sent");
        Check(card.dump == DumpLine::kFailed && Has(card.dump_text, "The report was sent, but its crash report could not be uploaded.") && card.retry_dump,
              "the warning names the crash report and the retry refers to it");
        Check(!Leaks(view, card, secret), "the failure state does not show the receipt");
        Check(session.Submit(Package(false), config) == true, "a new report can be sent after a partial success");
        script.Finish();
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kFailed);
        BugReportSession session(script.Hooks());
        session.Submit(Package(true), config);
        script.Finish();
        session.Collect();
        Check(session.RetryDump(config), "the crash dump retry starts");
        script.gate.WaitReached();
        const ReportView view = session.View();
        const ReportCard card = CardFor(view, false);
        Check(view.phase == ReportPhase::kRetryingDump && view.busy && view.ticket == "SOS-1234", "while retrying, the state is a dump retry and the ticket stays");
        Check(card.tone == CardTone::kReady && card.dump == DumpLine::kUploading && Has(card.detail, "SOS-1234") && card.dump_progress > 0.24f && card.dump_progress < 0.26f,
              "the retry shows the ticket card and a bounded progress");
        Check(!session.RetryDump(config) && !session.Submit(Package(true), config), "a retry cannot be doubled or overlapped by a new report");
        script.Finish();
        Check(session.Collect() == Collected::kDumpRetry && session.View().phase == ReportPhase::kSent && session.DumpUploaded(), "a successful retry leaves a sent report with its dump uploaded");
        Check(script.upload_calls == 1 && !session.RetryDump(config), "nothing is retried once the dump is uploaded");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kFailed);
        script.retry_outcome = CrashUpload::kFailed;
        BugReportSession session(script.Hooks());
        session.Submit(Package(true), config);
        script.Finish();
        session.Collect();
        session.RetryDump(config);
        script.Finish();
        session.Collect();
        const ReportView view = session.View();
        Check(view.phase == ReportPhase::kSentDumpFailed && view.can_retry_dump && CardFor(view, false).retry_dump, "a failed retry keeps the retry available and the report sent");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kFailed, "");
        BugReportSession session(script.Hooks());
        session.Submit(Package(true), config);
        script.Finish();
        session.Collect();
        Check(session.View().phase == ReportPhase::kSentDumpFailed && !session.View().can_retry_dump && !session.RetryDump(config), "without a receipt the dump cannot be retried");
        Script none;
        none.receipt = Sent(CrashUpload::kFailed);
        BugReportSession without(none.Hooks());
        without.Submit(Package(false), config);
        none.Finish();
        without.Collect();
        Check(!without.RetryDump(config), "a report that had no dump has nothing to retry");
    }
    {
        const BugReportOutcome failures[] = {BugReportOutcome::kUnreachable, BugReportOutcome::kRefused, BugReportOutcome::kServiceError,
                                             BugReportOutcome::kRecentLimit, BugReportOutcome::kDailyLimit};
        std::vector<std::string> titles;
        bool ok = true;
        for (const BugReportOutcome outcome : failures) {
            Script script;
            script.receipt.outcome = outcome;
            BugReportSession session(script.Hooks());
            session.Submit(Package(false), config);
            script.Finish();
            session.Collect();
            const ReportView view = session.View();
            const ReportCard card = CardFor(view, false);
            ok = ok && view.phase == ReportPhase::kFailed && card.tone == CardTone::kAttention && view.ticket.empty() && !card.copy_ticket &&
                 !view.busy && std::string(card.title).size() > 0 && !card.retry_dump && card.dump == DumpLine::kNone;
            titles.push_back(card.title);
            ok = ok && session.Submit(Package(false), config);
            script.Finish();
        }
        Check(ok, "a failed submission is an attention card without ticket, and the report can be sent again");
        bool distinct = titles[0] != titles[1] && titles[1] != titles[2] && titles[0] != titles[2] && titles[3] != titles[4];
        Check(distinct, "unreachable, refused and service errors are told apart from each other and from the limits");
        Check(!Has(titles[0] + titles[1] + titles[2], "HTTP") && !Has(titles[0] + titles[1] + titles[2], "status"), "failure text carries no raw service output");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kNotRequested);
        {
            BugReportSession session(script.Hooks());
            session.Submit(Package(false), config);
        }
        script.Finish();
        Check(script.file_calls == 1, "a page that goes away while the service answers leaves the worker harmless");
        Script later;
        later.receipt = Sent(CrashUpload::kUploaded);
        BugReportSession session(later.Hooks());
        session.Submit(Package(true), config);
        later.Finish();
        Check(session.View().phase == ReportPhase::kUploadingDump && session.busy(), "a result that arrives while the page is away waits");
        Check(session.Collect() == Collected::kDelivery && session.View().phase == ReportPhase::kSent && session.View().ticket == "SOS-1234",
              "returning to the page shows the completed report");
    }
    {
        Script script;
        script.receipt = Sent(CrashUpload::kNotRequested);
        BugReportSession session(script.Hooks());
        session.Submit(Package(false), config);
        script.Finish();
        session.Collect();
        script.receipt = Sent(CrashUpload::kNotRequested);
        script.receipt.ticket = "SOS-9999";
        session.Submit(Package(false), config);
        Check(session.View().ticket.empty() && session.View().phase == ReportPhase::kSendingText, "a new report does not show the previous ticket while it is being sent");
        script.Finish();
        session.Collect();
        Check(session.View().ticket == "SOS-9999", "the new ticket replaces the old one");
    }
    {
        const std::string page = Source("launcher/bug_report_page.cpp");
        const std::string session = Source("launcher/bug_report_session.h");
        Check(!Has(page, "credential") && !Has(page, "receipt") && !Has(Between(session, "struct ReportView", "struct SessionHooks"), "credential"),
              "the page and the view never touch the receipt");
        Check(Has(page, "if (session_.busy() || !AcceptDraft()) return;") && Has(page, "ImGui::BeginDisabled(sending);"), "the Send button is disabled and Send itself refuses while busy");
        Check(Has(page, "const ReportCard card = CardFor(view, session_.DumpUploaded());") && !Has(page, "std::thread"), "the page draws the production card and starts no thread itself");
    }
}

} // namespace

int main() {
    TestPlacement();
    TestStartup();
    TestEscape();
    TestReport();
    std::printf("launcher robustness: %d checks (placement %d, startup %d, escape %d, bug report %d), %d failures\n", g_checks, g_placement,
                g_startup, g_escape, g_report, g_failures);
    return g_failures == 0 ? 0 : 1;
}
