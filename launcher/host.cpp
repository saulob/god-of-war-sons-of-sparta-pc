#include "host.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "sos_displays.h"
#include "startup_phase.h"
#ifndef _WIN32
#include "sos_video_backend.h"
#endif

namespace sos {
namespace {

Rect ToRect(const SDL_Rect& rect) {
    return {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
}

#ifdef _WIN32
constexpr bool kWindows = true;

bool PinModule(std::string_view name) {
    const std::wstring wide(name.begin(), name.end());
    if (!LoadLibraryW(wide.c_str())) {
        return false;
    }
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, wide.c_str(), &pinned) != 0;
}

int64_t ThreadCpuNs() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return -1;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
}
#else
constexpr bool kWindows = false;

bool PinModule(std::string_view) {
    return false;
}

int64_t ThreadCpuNs() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
        return -1;
    }
    return static_cast<int64_t>(time.tv_sec) * kNsPerSecond + time.tv_nsec;
}
#endif

int64_t NowNs() {
    return static_cast<int64_t>(SDL_GetTicksNS());
}

SDL_Renderer* CreateLauncherRenderer(SDL_Window* window, RendererRequest request, RendererSelection& selection) {
    SDL_Renderer* renderer = nullptr;
    selection = SelectRenderer(
        LauncherRendererOrder(kWindows, request),
        [](std::string_view module) {
            if (PinModule(module)) {
                return true;
            }
            std::printf("Launcher: cannot keep %.*s loaded, skipping that renderer\n", int(module.size()), module.data());
            return false;
        },
        [&](const std::string& name) {
            renderer = SDL_CreateRenderer(window, name.empty() ? nullptr : name.c_str());
            if (!renderer) {
                std::printf("Launcher: no %s renderer (%s)\n", name.empty() ? "accelerated" : name.c_str(), SDL_GetError());
            }
            return renderer != nullptr;
        });
    return renderer;
}

} // namespace

bool LauncherHost::Open(const HostOptions& options) {
    startup_ = StartupTimeline(NowNs());
#ifndef _WIN32
    // A second launcher window after an SDL_Quit (problem dialog, icon preparation) would have lost the hint.
    sos_video_backend_prepare(0);
#endif
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::printf("Launcher: SDL video failed: %s\n", SDL_GetError());
        return false;
    }
    video_ = true;
    const bool background_gamepad =
        AllowBackgroundGamepadEvents(std::getenv("GAMESCOPE_WAYLAND_DISPLAY"), std::getenv("XDG_CURRENT_DESKTOP"));
    if (background_gamepad) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!gamepad_) {
        std::printf("Launcher: gamepad support unavailable: %s\n", SDL_GetError());
    }
    {
        const char* driver = SDL_GetCurrentVideoDriver();
        std::printf("Launcher: SDL %d.%d.%d video driver %s, gamepad subsystem %s, background gamepad events %s\n",
                    SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                    SDL_VERSIONNUM_MICRO(SDL_GetVersion()), driver ? driver : "none", gamepad_ ? "ready" : "unavailable",
                    background_gamepad ? "on (Gamescope)" : "off");
        if (gamepad_) {
            int count = 0;
            SDL_JoystickID* pads = SDL_GetGamepads(&count);
            std::printf("Launcher input: %d gamepad(s) at start\n", count);
            SDL_free(pads);
        }
    }
    Reach(StartupStage::kSdlReady);

    const std::vector<Display> displays = ListDisplays();
    if (displays.empty()) {
        std::printf("Launcher: no display found\n");
        Close();
        return false;
    }
    std::vector<Rect> usable;
    for (const Display& display : displays) {
        usable.push_back(ToRect(display.usable));
    }

    if (options.restore) {
        restored_ = RestorablePlacement(*options.restore, usable);
    }
    const bool restore = restored_ && PlacementIsSet(*restored_);
    saved_placement_ = !options.restore ? SavedPlacement::kNone : restore ? SavedPlacement::kRestored : SavedPlacement::kStale;
    for (const Display& display : displays) {
        display_facts_.push_back({display.name, ToRect(display.bounds), ToRect(display.usable), display.scale, display.primary});
    }
    const Display* target = &displays.front();
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    int width = 0, height = 0;
    if (restore) {
        const Rect& normal = restored_->normal;
        const SDL_Point center{(normal.left + normal.right) / 2, (normal.top + normal.bottom) / 2};
        const SDL_DisplayID id = SDL_GetDisplayForPoint(&center);
        for (const Display& display : displays) {
            if (display.id == id) {
                target = &display;
            }
        }
        x = normal.left;
        y = normal.top;
        width = normal.width();
        height = normal.height();
    } else {
        x = SDL_WINDOWPOS_CENTERED_DISPLAY(target->id);
        y = SDL_WINDOWPOS_CENTERED_DISPLAY(target->id);
        width = static_cast<int>(options.width * target->scale);
        height = static_cast<int>(options.height * target->scale);
        FitWindowSize(width, height, *target);
    }

    Reach(StartupStage::kDisplaySelected);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window_) {
        std::printf("Launcher: cannot create the window: %s\n", SDL_GetError());
        Close();
        return false;
    }
    SDL_SetWindowMinimumSize(window_, std::min(static_cast<int>(options.min_width * target->scale), width),
                             std::min(static_cast<int>(options.min_height * target->scale), height));
    Reach(StartupStage::kWindowCreated);
    LogPlacement("created", saved_placement_);

    const RendererRequest request = ParseRendererRequest(std::getenv(kLauncherRendererVariable));
    RendererSelection selection;
    renderer_ = CreateLauncherRenderer(window_, request, selection);
    if (!renderer_) {
        std::printf("Launcher: cannot create a renderer: %s\n", SDL_GetError());
        Close();
        return false;
    }
    monitor_.SetRenderer(RendererLabel(SDL_GetRendererName(renderer_)));
    std::printf("%s\n",
                RendererSelectedLine(SDL_GetRendererName(renderer_), request, selection.fallback, selection.pinned).c_str());
    ConfigureVsync();
    show_gate_.RendererReady();
    Reach(StartupStage::kRendererReady);

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    hidden_ = options.hidden;
    show_gate_.Opened(hidden_);
    loop_ = LoopPacer(hidden_ ? 0 : kLauncherFrameNs);
    if (restored_ && restored_->maximized) {
        SDL_MaximizeWindow(window_);
    }
    if (restored_ && restored_->fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
    Reach(StartupStage::kUiReady);
    std::printf("Launcher: window %dx%d at %.2fx display scale, %s renderer\n", width, height, target->scale,
                SDL_GetRendererName(renderer_));
    return true;
}

// The software renderer's VSync is only simulated by SDL on a Windows GDI window, so it is turned off there and the frame
// cap alone paces the loop.
void LauncherHost::ConfigureVsync() {
    const bool set = SDL_SetRenderVSync(renderer_, 1);
    int value = 0;
    SDL_GetRenderVSync(renderer_, &value);
    const bool software = std::string_view(SDL_GetRendererName(renderer_)) == SDL_SOFTWARE_RENDERER;
    int surface_value = 0;
    const bool real = !software || (SDL_GetWindowSurfaceVSync(window_, &surface_value) && surface_value != 0);
    const bool effective = set && value == 1 && real;
    if (set && !effective) {
        SDL_SetRenderVSync(renderer_, 0);
    }
    std::printf("Launcher renderer: pacing=%dhz_deadline vsync_effective=%d\n", kLauncherTargetHz, effective ? 1 : 0);
}

void LauncherHost::Reach(StartupStage stage) {
    const std::optional<StageReached> reached = startup_.Reach(stage, NowNs());
    if (!reached) {
        return;
    }
    sos_startup_phase(StartupStagePhase(stage));
    if (reached->slow) {
        std::printf("%s\n", startup_.SlowLine(*reached, monitor_.renderer(), window_ && Minimized()).c_str());
    }
}

void LauncherHost::LogPlacement(const char* stage, SavedPlacement saved) {
    PlacementFacts facts;
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    facts.window = {x, y, x + width, y + height};
    SDL_GetWindowSizeInPixels(window_, &facts.pixel_width, &facts.pixel_height);
    facts.pixel_density = SDL_GetWindowPixelDensity(window_);
    facts.content_scale = SDL_GetWindowDisplayScale(window_);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    facts.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    facts.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    facts.saved = saved;
    std::printf("%s\n", PlacementLine(stage, facts, display_facts_).c_str());
}

void LauncherHost::Show() {
    if (!window_ || !show_gate_.TryShow()) {
        return;
    }
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
    Reach(StartupStage::kShown);
    LogPlacement("shown", saved_placement_);
}

ImTextureID LauncherHost::CreateTexture(const uint8_t* rgba, int width, int height) {
    if (!renderer_ || !rgba || width <= 0 || height <= 0) {
        return ImTextureID{};
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return ImTextureID{};
    }
    if (!SDL_UpdateTexture(texture, nullptr, rgba, width * 4)) {
        SDL_DestroyTexture(texture);
        return ImTextureID{};
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    textures_.push_back(texture);
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
}

void LauncherHost::DestroyTexture(ImTextureID texture) {
    SDL_Texture* sdl_texture = reinterpret_cast<SDL_Texture*>(static_cast<uintptr_t>(texture));
    const auto found = std::find(textures_.begin(), textures_.end(), sdl_texture);
    if (found != textures_.end()) {
        SDL_DestroyTexture(sdl_texture);
        textures_.erase(found);
    }
}

void LauncherHost::Close() {
    // Close runs again from the destructor after an explicit Close (and after a failed Open): with nothing left open it
    // does nothing, so the teardown phases are logged once.
    if (!window_ && !renderer_ && !imgui_ && !video_ && !gamepad_ && textures_.empty()) {
        return;
    }
    sos_startup_phase("launcher host closing");
    for (SDL_Texture* texture : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    if (imgui_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    sos_startup_phase("launcher destroying renderer");
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    sos_startup_phase("launcher destroying window");
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    sos_startup_phase("launcher SDL shutdown");
    if (video_ || gamepad_) {
        // Nothing else uses SDL yet: the game window thread starts it again from scratch.
        SDL_Quit();
        video_ = gamepad_ = false;
    }
    sos_startup_phase("launcher host closed");
}

void LauncherHost::NoteInput(const char* event, const std::string& detail, InputKind kind) {
    if (const std::optional<std::string> line = input_.Note(event, detail, kind)) {
        std::printf("%s\n", line->c_str());
    }
}

void LauncherHost::HandleEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
    case SDL_EVENT_QUIT:
        // SIGTERM or SIGINT (Steam's "Exit game"), or the session ending.
        std::printf("Launcher: quit requested by the system (SDL quit event)\n");
        quit_ = true;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == SDL_GetWindowID(window_)) {
            std::printf("Launcher: window close requested\n");
            quit_ = true;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat) {
            NoteInput("key_down", {}, InputKind::kKeyboard);
        }
        if (event.key.key == SDLK_F11 && !event.key.repeat) {
            SetFullscreen(!Fullscreen());
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const bool touch = event.button.which == SDL_TOUCH_MOUSEID;
        NoteInput(touch ? "touch_click" : "mouse_click", "button=" + std::to_string(event.button.button),
                  touch ? InputKind::kTouch : InputKind::kMouse);
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
        NoteInput("finger_down", {}, InputKind::kTouch);
        break;
    case SDL_EVENT_GAMEPAD_ADDED: {
        const char* name = SDL_GetGamepadNameForID(event.gdevice.which);
        NoteInput("gamepad_added", std::string("name=\"") + (name ? name : "unknown") + "\"");
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        NoteInput("gamepad_removed");
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const char* button = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button));
        NoteInput("gamepad_button", std::string("button=") + (button ? button : "unknown"), InputKind::kGamepad);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        NoteInput("focus_gained");
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        NoteInput("focus_lost");
        break;
    default:
        break;
    }
}

bool LauncherHost::PumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        HandleEvent(event);
    }
    return !quit_;
}

void LauncherHost::WaitForEvent(int64_t timeout_ns) {
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, static_cast<Sint32>(std::max<int64_t>(timeout_ns / kNsPerMs, 1)))) {
        HandleEvent(event);
    }
}

bool LauncherHost::Minimized() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

bool LauncherHost::Focused() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

struct LauncherHost::LoopEnv {
    LauncherHost& host;

    bool PumpEvents() { return host.PumpEvents(); }
    bool Minimized() { return host.Minimized(); }
    int64_t NowNs() { return sos::NowNs(); }
    void SleepNs(int64_t ns) { SDL_DelayNS(static_cast<Uint64>(ns)); }
    void WaitForEventNs(int64_t ns) { host.WaitForEvent(ns); }
    int64_t ThreadCpuNs() { return sos::ThreadCpuNs(); }
    void Emit(const std::string& line) { std::printf("%s\n", line.c_str()); }
};

bool LauncherHost::WaitForFrame() {
    if (!startup_.Reached(StartupStage::kLoopActive)) {
        Reach(StartupStage::kLoopActive);
        std::printf("%s\n", startup_.SummaryLine(monitor_.renderer()).c_str());
    }
    LoopEnv env{*this};
    return sos::WaitForFrame(env, loop_, monitor_);
}

void LauncherHost::NoteFrameDrawn(int64_t now_ns, const FrameTimes& times) {
    if (loop_.capped()) {
        if (const std::optional<StallEvent> stall = monitor_.FrameDrawn(now_ns, times)) {
            std::printf("%s\n", monitor_.StallLine(*stall, Focused()).c_str());
        }
    }
}

void LauncherHost::BeginFrame() {
    frame_begin_ns_ = NowNs();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    const float scale = SDL_GetWindowDisplayScale(window_);
    ImGui::GetStyle().FontScaleDpi = scale > 0.0f ? scale : 1.0f;
    ImGui::NewFrame();
    LauncherEscape().BeginFrame(ImGui::IsKeyPressed(ImGuiKey_Escape, false));
}

void LauncherHost::EndFrame(const ImVec4& clear, const char* capture_path) {
    const int64_t submitting = NowNs();
    ImGui::Render();
    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer_, clear.x, clear.y, clear.z, 1.0f);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (capture_path) {
        if (SDL_Surface* frame = SDL_RenderReadPixels(renderer_, nullptr)) {
            SDL_SavePNG(frame, capture_path);
            SDL_DestroySurface(frame);
        }
    }
    const int64_t presenting = NowNs();
    SDL_RenderPresent(renderer_);
    const int64_t presented = NowNs();
    NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});
    if (!first_present_) {
        first_present_ = true;
        show_gate_.FramePresented();
        Reach(StartupStage::kFirstFramePresented);
    }
}

bool LauncherHost::Fullscreen() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void LauncherHost::SetFullscreen(bool fullscreen) {
    SDL_SetWindowFullscreen(window_, fullscreen);
    // X11 and Wayland apply it later; a frame in between could record the fullscreen size as the window's own.
    SDL_SyncWindow(window_);
}

std::optional<WindowState> LauncherHost::State() const {
    if (!window_) {
        return std::nullopt;
    }
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    return WindowState{{x, y, x + width, y + height}, (flags & SDL_WINDOW_MAXIMIZED) != 0,
                       (flags & SDL_WINDOW_MINIMIZED) != 0};
}

} // namespace sos
