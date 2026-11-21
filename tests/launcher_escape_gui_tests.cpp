// The launcher's own dialogs on the production code with a headless Dear ImGui context (no window, no renderer): one
// Escape press closes exactly one dialog and is never left over for another owner or for the launcher.
#include <cstdio>
#include <filesystem>
#include <string>

#include <imgui.h>

#include "launcher_escape.h"
#include "package_browser.h"
#include "problem_dialog.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

template <typename Draw>
void Frame(bool escape_down, Draw&& draw) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Escape, escape_down);
    ImGui::NewFrame();
    sos::LauncherEscape().BeginFrame(ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    draw();
    ImGui::EndFrame();
}

} // namespace

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    trg::Fonts fonts;
    fonts.size = 16.0f;
    trg::Theme theme;
    {
        sos::ProblemDialog dialog;
        dialog.Show("cause", "advice");
        for (int i = 0; i < 5; ++i) {
            Frame(false, [&] { dialog.Draw(fonts, theme); });
        }
        Check(dialog.open(), "the error dialog stays open without a key");
        bool second_claim = true, quit_allowed = true;
        Frame(true, [&] {
            dialog.Draw(fonts, theme);
            second_claim = sos::LauncherEscape().Take();
            quit_allowed = sos::LauncherEscape().QuitAllowed();
        });
        Check(!dialog.open(), "Escape closes the error dialog");
        Check(!second_claim, "the press the dialog took is not left for another owner");
        Check(!quit_allowed, "the same press does not close the launcher");
        bool leftover = true;
        Frame(true, [&] { leftover = sos::LauncherEscape().Take(); });
        Check(!leftover, "a held Escape is not a second press");
        bool pressed_again = false;
        Frame(false, [&] {});
        Frame(true, [&] { pressed_again = sos::LauncherEscape().Take(); });
        Check(pressed_again, "a new press is claimable again");
    }
    {
        sos::PackageBrowser browser;
        browser.Open(std::filesystem::current_path());
        for (int i = 0; i < 4; ++i) {
            Frame(false, [&] { browser.Draw(nullptr, 16.0f, theme); });
        }
        Check(browser.active(), "the package chooser stays open without a key");
        std::optional<std::filesystem::path> picked;
        bool second_claim = true;
        Frame(true, [&] {
            picked = browser.Draw(nullptr, 16.0f, theme);
            second_claim = sos::LauncherEscape().Take();
        });
        Check(!browser.active() && !picked, "Escape is Cancel in the package chooser");
        Check(!second_claim, "the chooser took the press alone");
    }
    {
        sos::ProblemDialog first;
        first.Show("a", "b");
        sos::PackageBrowser second;
        second.Open(std::filesystem::current_path());
        for (int i = 0; i < 4; ++i) {
            Frame(false, [&] {
                first.Draw(fonts, theme);
            });
        }
        Frame(true, [&] {
            first.Draw(fonts, theme);
            second.Draw(nullptr, 16.0f, theme);
        });
        Check(!first.open() && second.active(), "when two owners draw in one frame, the first one drawn takes the press and the other stays");
    }
    std::printf("launcher escape dialogs: %d checks, %d failures\n", g_checks, g_failures);
    ImGui::DestroyContext();
    return g_failures == 0 ? 0 : 1;
}
