// The launcher's window: SDL3, an SDL renderer and Dear ImGui. It exists only until Play or Quit, before the loader
// creates the game window and its Vulkan presenter, so only one SDL window and one ImGui context are alive at a time.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_frame_loop.h"
#include "launcher_escape.h"
#include "launcher_input.h"
#include "launcher_renderer.h"
#include "launcher_startup.h"
#include "placement.h"

union SDL_Event;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace sos {

struct HostOptions {
    std::string title;
    int width = 1280;  // logical, scaled by the display's content scale
    int height = 720;
    int min_width = 900;
    int min_height = 560;
    std::optional<WindowPlacement> restore;  // used when it lies on a display
    bool hidden = false;                     // page screenshots without showing the window
};

class LauncherHost {
public:
    LauncherHost() = default;
    ~LauncherHost() { Close(); }
    LauncherHost(const LauncherHost&) = delete;
    LauncherHost& operator=(const LauncherHost&) = delete;

    // False when no window or renderer could be created; nothing is left open then.
    bool Open(const HostOptions& options);
    // Shows the window, once, after its first frame was presented. Does nothing when it was opened hidden.
    void Show();
    // Records a launcher startup stage outside the host (the pages are built by the caller).
    void Reach(StartupStage stage);
    void Close();

    // Handles the window's events (F11 switches the launcher's own fullscreen) and returns when the next frame is due,
    // at most 60 a second; a minimized window only wakes at a low rate and a hidden one is not limited. False once the
    // window was closed.
    bool WaitForFrame();

    // Between these two the caller draws the launcher with ImGui. A capture path saves the frame as a PNG.
    void BeginFrame();
    void EndFrame(const ImVec4& clear, const char* capture_path = nullptr);

    bool Fullscreen() const;
    void SetFullscreen(bool fullscreen);
    std::optional<WindowState> State() const;

    // A picture for ImGui to draw (RGBA8, width * height * 4 bytes). The host owns it until Close; 0 when it could not be made.
    ImTextureID CreateTexture(const uint8_t* rgba, int width, int height);
    void DestroyTexture(ImTextureID texture);

    // The saved placement the window was opened with, when it lay on a display.
    const std::optional<WindowPlacement>& Restored() const { return restored_; }

    // The kind of the last input event (mouse, touchscreen, keyboard, gamepad): the source of a PLAY.
    InputKind LastInput() const { return input_.last(); }

private:
    struct LoopEnv;

    bool PumpEvents();
    void HandleEvent(const SDL_Event& event);
    void WaitForEvent(int64_t timeout_ns);
    bool Minimized() const;
    bool Focused() const;
    void ConfigureVsync();
    void NoteFrameDrawn(int64_t now_ns, const FrameTimes& times);
    void LogPlacement(const char* stage, SavedPlacement saved);

    std::optional<WindowPlacement> restored_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    std::vector<SDL_Texture*> textures_;
    bool video_ = false;
    bool gamepad_ = false;
    bool hidden_ = false;
    bool quit_ = false;
    LoopPacer loop_;
    PerformanceMonitor monitor_;
    StartupTimeline startup_;
    ShowGate show_gate_;
    std::vector<DisplayFacts> display_facts_;
    SavedPlacement saved_placement_ = SavedPlacement::kNone;
    bool first_present_ = false;
    int64_t frame_begin_ns_ = 0;
    InputTelemetry input_;
    void NoteInput(const char* event, const std::string& detail = {}, InputKind kind = InputKind::kNone);
};

} // namespace sos
