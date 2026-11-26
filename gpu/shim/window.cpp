// The game window (SDL3) the renderer presents to.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "sos_displays.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "sos_sdl_compat.h"
#ifndef _WIN32
#include "sos_video_backend.h"
#endif
#include "sos_window_fit.h"
#include "dcp_overlay.h"
#include "dcp_graphics.h"
#include "dcp_latency_trace.h"
#include "dcp_mouse.h"
#include "dcp_present.h"
#include "dcp_settings.h"
#include "dcp_title.h"
#include "dcpgpu.h"
#include "sos_gamescope_touch.h"
#include "sos_touch_route.h"
#include "sos_touchscreen.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#ifdef _WIN32
#include "video_core/renderer_opengl/gl_angle.h"
#endif

extern "C" void runtime_log_flush(void);

namespace Frontend {
namespace {

// ---- SteamOS Gamescope touchscreen (src/sos_touch_gamescope.h). Off Gamescope, on Windows and with
// SOS_TOUCH_GAMESCOPE=0 none of it runs: a touch stays whatever the system makes of it, as before.
std::atomic<bool> g_touch_routed{false}; ///< set once by the window constructor; the pad sampler loads it
SosGamescopeTouch::Claim g_gamescope_touch; ///< window thread, and Release from every exit path
SosTouchRouteLog g_touch_route_log;         ///< window thread
std::uint64_t g_touch_counts[SOS_TOUCH_EVENT_COUNT]; ///< window thread
std::atomic_flag g_touch_lock = ATOMIC_FLAG_INIT;
SosTouchscreen g_touchscreen; ///< under g_touch_lock: the window thread writes, the pad sampler reads

template <typename Fn>
void WithTouchscreen(Fn&& fn) {
    while (g_touch_lock.test_and_set(std::memory_order_acquire)) {
    }
    fn(g_touchscreen);
    g_touch_lock.clear(std::memory_order_release);
}

bool GamescopeTouchWanted() {
    SosTouchGamescopeEnv env{};
#ifdef __linux__
    env.linux_host = 1;
#endif
    env.game_window = 1; // only the game's window: the launcher never claims Passthrough
    env.gamescope = DcpPresent::GamescopeSession() ? 1 : 0;
    env.display = std::getenv("DISPLAY");
    env.opt_out = std::getenv("SOS_TOUCH_GAMESCOPE");
    return sos_touch_gamescope_wanted(&env) != 0;
}

int DirectTouchDevices() {
    int total = 0, direct = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&total);
    for (int i = 0; devices && i < total; ++i) {
        direct += SosTouchRoute::AcceptedTouch(devices[i], SDL_GetTouchDeviceType(devices[i])) ? 1 : 0;
    }
    SDL_free(devices);
    return direct;
}

void LogTouchRoute(SosTouchRouteEvent event) {
    const SosTouchRouteInfo route{SDL_GetCurrentVideoDriver(),
                                  DcpPresent::GamescopeSession() ? 1 : 0,
                                  g_touch_routed.load(std::memory_order_relaxed) ? 1 : 0,
                                  DirectTouchDevices(),
                                  g_gamescope_touch.Roots(),
                                  g_gamescope_touch.CompositorRoot() ? 1 : 0,
                                  int(g_gamescope_touch.Failure())};
    char line[SOS_TOUCH_ROUTE_LINE_SIZE];
    if (sos_touch_route_note(&g_touch_route_log, event, &route, line, sizeof(line))) {
        std::printf("%s\n", line);
    }
}

void NoteTouch(SosTouchEvent event) {
    char line[96];
    if (sos_touch_event_note(g_touch_counts, event, line, sizeof(line))) {
        std::printf("%s\n", line);
    }
}

// Window thread: the game window took focus (or was created with it).
void ClaimGamescopeTouch() {
    if (!g_touch_routed.load(std::memory_order_relaxed)) {
        return;
    }
    const auto result = g_gamescope_touch.Acquire(SosGamescopeTouch::LoadXApi(),
                                                  SosGamescopeTouch::LocalDisplays(std::getenv("DISPLAY")), SDL_GetTicks());
    LogTouchRoute(result == SosGamescopeTouch::Result::Applied ? SOS_TOUCH_ROUTE_CLAIMED : SOS_TOUCH_ROUTE_CLAIM_FAILED);
}

// Window thread: focus loss gives each Gamescope root its own touch mode back and drops every contact.
void ReleaseGamescopeTouch() {
    if (g_gamescope_touch.Claimed()) {
        g_gamescope_touch.Release();
        LogTouchRoute(SOS_TOUCH_ROUTE_RELEASED);
    }
    WithTouchscreen([](SosTouchscreen& s) { sos_touchscreen_release_all(&s); });
}

// The presented picture inside the window: the presenter letterboxes and centres the frame in the swapchain.
SosTouchArea GameTouchArea() {
    const auto& progress = DcpPresent::g_progress;
    const DcpPresent::Size picture = DcpPresent::Unpack(progress.destination_size.load(std::memory_order_relaxed));
    const DcpPresent::Size surface = DcpPresent::Unpack(progress.swapchain_size.load(std::memory_order_relaxed));
    return sos_touch_area_centred(int(picture.width), int(picture.height), int(surface.width), int(surface.height));
}

// No window means no game: say why and exit with an error, not through UNREACHABLE (no crash, no abort).
[[noreturn]] void ExitNoWindow(const char* what, const char* sdl_error) {
    std::fprintf(stderr,
                 "Error: %s: %s\nNo display is available. Run from a graphical session "
                 "(DISPLAY for X11 or WAYLAND_DISPLAY for Wayland must be set).\n",
                 what, sdl_error && *sdl_error ? sdl_error : "unknown SDL error");
    std::fflush(stderr);
    runtime_log_flush();
    std::_Exit(1);
}

void TracePress(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)) {
        DcpLatencyTrace::press_ns.store(event.common.timestamp, std::memory_order_relaxed);
    }
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title, GraphicsApi api)
    : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    sos_prepare_sdl_gamepad();
#ifndef _WIN32
    // The launcher's SDL_Quit cleared the video driver hint (Steam on a Wayland desktop asks for X11 first).
    sos_video_backend_prepare(0);
#endif
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        ExitNoWindow("Cannot initialize SDL video", SDL_GetError());
    }
    // The launcher's Window size and Monitor settings; the same display list as its Monitor row.
    const auto& settings = DcpSettings::Get();
    const std::vector<sos::Display> displays = sos::ListDisplays();
    const sos::Display* display = sos::DisplayForSetting(displays, settings.monitor);
    int window_width = settings.requested_width > 0 ? settings.requested_width.load() : width_;
    int window_height = settings.requested_height > 0 ? settings.requested_height.load() : height_;
    int window_x = SDL_WINDOWPOS_CENTERED, window_y = SDL_WINDOWPOS_CENTERED;
    if (display) {
        sos::FitToDisplay(window_width, window_height, *display);
        window_x = window_y = SDL_WINDOWPOS_CENTERED_DISPLAY(display->id);
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, window_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, window_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
#ifdef _WIN32
    if (api == GraphicsApi::Angle) {
        OpenGL::RequestAngleDirect3D11();
    }
#endif
    if (api == GraphicsApi::OpenGL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    }
    SDL_SetBooleanProperty(props, api == GraphicsApi::Vulkan ? SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN
                                                             : SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN,
                           true);
    base_title = title;
    title_shown = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
#ifdef _WIN32
        if (api == GraphicsApi::Angle) {
            OpenGL::ExitAngleUnavailable(SDL_GetError());
        }
#endif
        ExitNoWindow("Cannot create the game window", SDL_GetError());
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        // offscreen, kmsdrm or dummy: no window the Vulkan presenter can draw to (Dragon d298fa9).
        ExitNoWindow("Unsupported SDL video driver for the game window", driver ? driver : "(none)");
    }
    if (display) {
        // Hidden until the real title bar and frame are known, so the decorated window is fitted and centred once.
        sos::WindowBorders borders;
        SDL_GetWindowBordersSize(window, &borders.top, &borders.left, &borders.bottom, &borders.right);
        const sos::WindowFit fit = sos::FitWindowToArea(window_width, window_height, borders, display->usable.x,
                                                        display->usable.y, display->usable.w, display->usable.h);
        window_width = fit.width;
        window_height = fit.height;
        SDL_SetWindowSize(window, fit.width, fit.height);
        SDL_SetWindowPosition(window, fit.x, fit.y);
    }
    ApplyDisplayMode(DcpSettings::Get().display_mode);
    SDL_ShowWindow(window);
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
        DcpSettings::Get().display_refresh_hz = mode->refresh_rate;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    LogGamescopeWindow();
    g_touch_routed.store(GamescopeTouchWanted(), std::memory_order_relaxed);
    ClaimGamescopeTouch();
    LogTouchRoute(SOS_TOUCH_ROUTE_READY);
    const std::string display_name = display && !display->name.empty() ? " (" + display->name + ")" : "";
    std::printf("Window: display %d of %zu%s, size %dx%d, VSync %s, frame rate in title %s, "
                "resolution in title %s\n",
                display ? int(display - displays.data()) + 1 : 0, displays.size(), display_name.c_str(),
                window_width, window_height, settings.vsync ? "on" : "off", settings.show_fps_title ? "on" : "off",
                settings.show_resolution_title ? "on" : "off");
    // dcp: the size above is the Windowed size setting; in Borderless the window covers the display. The surface the
    // swapchain is sized from, in pixels, next to the logical size and scale (Wayland fractional scaling, HiDPI).
    LogSurfaceGeometry("created");
    const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    std::printf("Display: refresh %.2f Hz\n", current ? current->refresh_rate : 0.0f);
#ifndef _WIN32
    std::printf("Session: %s\n", DcpPresent::DescribeSession(DcpPresent::GamescopeSession(), std::getenv).c_str());
#endif
}

// The window's logical size, its pixel size (what the swapchain follows), density, display scale and display bounds:
// at creation and on each size change, at most 8 lines, the same line again never.
void WindowSDL::LogSurfaceGeometry(const char* what) {
    int logical_w = 0, logical_h = 0, pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
    const bool fullscreen = display_mode == DcpGraphics::Borderless; // the mode ApplyDisplayMode requested
    const float density = SDL_GetWindowPixelDensity(window);
    const float scale = SDL_GetWindowDisplayScale(window);
    const std::array<int, 9> state{logical_w, logical_h, pixel_w, pixel_h, bounds.w, bounds.h, fullscreen,
                                   int(density * 100.0f + 0.5f), int(scale * 100.0f + 0.5f)};
    static std::array<int, 9> last{};
    static unsigned lines = 0;
    constexpr unsigned kMaxLines = 8;
    if (state == last || lines >= kMaxLines) {
        return;
    }
    last = state;
    ++lines;
    const char* driver = SDL_GetCurrentVideoDriver();
    std::printf("Window: surface %s, pixels %dx%d, logical %dx%d, density %.2f, display scale %.2f, display %dx%d, "
                "%s, video %s\n",
                what, pixel_w, pixel_h, logical_w, logical_h, density, scale, bounds.w, bounds.h,
                fullscreen ? "borderless" : "windowed", driver ? driver : "?");
}

void WindowSDL::LogGamescopeWindow() {
    if (!DcpPresent::GamescopeSession()) {
        return;
    }
    int logical_w = 0, logical_h = 0, pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const std::array<int, 7> state{logical_w, logical_h, pixel_w, pixel_h, bounds.w, bounds.h, fullscreen};
    static std::array<int, 7> last{};
    static unsigned lines = 0;
    constexpr unsigned kMaxLines = 8;
    if (state == last || lines >= kMaxLines) {
        return;
    }
    last = state;
    std::printf("Display diagnostic: Gamescope window%s logical=%dx%d pixels=%dx%d display=%dx%d fullscreen=%s scale=%.2f\n",
                lines++ ? " changed" : "", logical_w, logical_h, pixel_w, pixel_h, bounds.w, bounds.h,
                fullscreen ? "borderless" : "windowed", SDL_GetWindowDisplayScale(window));
}

// Borderless is a desktop-sized fullscreen window, without a display mode change. Window thread only (SDL); the
// swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    display_mode = mode;
    const bool borderless = mode == DcpGraphics::Borderless;
    if (borderless && !SDL_SetWindowFullscreenMode(window, nullptr)) {
        LOG_WARNING(Frontend, "Borderless fullscreen: {}", SDL_GetError());
    }
    ReleaseMouse();
    SDL_SetWindowFullscreen(window, borderless);
    SDL_SyncWindow(window);
    std::printf("Window: %s\n", DcpGraphics::DisplayModeNames[borderless ? DcpGraphics::Borderless : DcpGraphics::Windowed]);
}

WindowSDL::~WindowSDL() {
    ReleaseMouse();
    SDL_ShowCursor();
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    RefreshTitle();
    DcpOverlay::SetTextEntry(text_active, text_prompt, text);
}

// Window thread. The title is the game's, then the render resolution, then the text being typed or the frame rate.
void WindowSDL::RefreshTitle() {
    const auto& settings = DcpSettings::Get();
    std::string entry;
    if (text_active) {
        entry = " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)";
    }
    const std::string title = DcpTitle::Compose({
        .base = base_title,
        .show_resolution = settings.show_resolution_title,
        .width = settings.image_width,
        .height = settings.image_height,
        .show_fps = settings.show_fps_title,
        .fps = fps,
        .entry = entry,
    });
    if (title != title_shown) {
        SDL_SetWindowTitle(window, title.c_str());
        title_shown = title;
    }
}

// The frame rate of the presented game frames, measured about once a second.
void WindowSDL::UpdateFpsTitle() {
    if (!DcpSettings::Get().show_fps_title) {
        fps = -1.0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (fps_started == std::chrono::steady_clock::time_point{}) {
        fps_started = now;
        fps_frames = DcpOverlay::PresentedFrames();
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - fps_started).count();
    if (elapsed < 1.0) {
        return;
    }
    const u64 frames = DcpOverlay::PresentedFrames();
    fps = double(frames - fps_frames) / elapsed;
    fps_started = now;
    fps_frames = frames;
}

// F11: Windowed and Borderless, saved as the Display mode like the launcher's row.
void WindowSDL::ToggleBorderless() {
    auto& mode = DcpSettings::Get().display_mode;
    mode = mode.load() == DcpGraphics::Windowed ? DcpGraphics::Borderless : DcpGraphics::Windowed;
    DcpSettings::SaveDisplayMode();
}

void WindowSDL::UpdateCursor() {
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (const auto visible = cursor.Update(true, focused, DcpOverlay::CapturesInput(), mouse_capture.Captured())) {
        if (*visible) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
        }
    }
}

// Window thread. Relative mouse mode is on only while Pointer Control is held and nothing else needs the mouse; whenever
// the game stops taking mouse input, held buttons and the pointer are dropped.
void WindowSDL::UpdateMouseCapture() {
    DcpMouseState* const mouse = dcp_mouse_global();
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const DcpMouseCapture::Frame frame{dcp_mouse_pointer_active(mouse) != 0, focused, DcpOverlay::CapturesInput(),
                                       text_active};
    const bool blocked = DcpMouseCapture::Blocked(frame);
    if (blocked != mouse_blocked) {
        mouse_blocked = blocked;
        dcp_mouse_block(mouse, blocked);
        WithTouchscreen([blocked](SosTouchscreen& s) { sos_touchscreen_block(&s, blocked); });
    }
    if (const auto relative = mouse_capture.Update(frame)) {
        if (!SDL_SetWindowRelativeMouseMode(window, *relative)) {
            LOG_WARNING(Frontend, "Relative mouse mode: {}", SDL_GetError());
        }
    }
}

// Window thread. A window mode switch and closing the game must not leave the mouse captured or a button held.
void WindowSDL::ReleaseMouse() {
    dcp_mouse_release_all(dcp_mouse_global());
    WithTouchscreen([](SosTouchscreen& s) { sos_touchscreen_release_all(&s); });
    if (mouse_capture.Release()) {
        SDL_SetWindowRelativeMouseMode(window, false);
    }
}

// Window thread. Only the gamepads runtime_pad.c opened produce these events.
bool WindowSDL::TrackControllerEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_activity.erase(event.gdevice.which);
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        PadActivity& pad = pad_activity[event.gbutton.which];
        const u32 bit = event.gbutton.button < 32 ? 1u << event.gbutton.button : 0u;
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            pad.state.buttons |= bit;
        } else {
            pad.state.buttons &= ~bit;
        }
        return pad.filter.Update(pad.state);
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        PadActivity& pad = pad_activity[event.gaxis.which];
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: pad.state.left_x = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_LEFTY: pad.state.left_y = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: pad.state.right_x = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: pad.state.right_y = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: pad.state.left_trigger = event.gaxis.value; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: pad.state.right_trigger = event.gaxis.value; break;
        default: return false;
        }
        return pad.filter.Update(pad.state);
    }
    default:
        return false;
    }
}

// Window thread. The presenter reads the visibility itself (Presenter::CanPresent); window_active stays the start-up
// watch's flag: no stall is reported while the window is minimized or hidden.
void WindowSDL::ApplyWindowEvent(DcpPresent::WindowEvent event) {
    visibility.Apply(event);
    // The flags are the truth: a backend that restores without RESTORED (only MAXIMIZED or SHOWN) must not leave the
    // presenter paused for good.
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    visibility.Sync((flags & SDL_WINDOW_MINIMIZED) != 0, (flags & SDL_WINDOW_HIDDEN) != 0);
    visibility_bits.store(visibility.Pack(), std::memory_order_relaxed);
    DcpPresent::g_progress.window_active = visibility.Visible();
}

// Window thread, only while the Gamescope touch route is on. A finger on the game picture is a touchpad contact; while an
// overlay or text entry holds the input, no new contact starts (the touchscreen is blocked with the mouse).
void WindowSDL::HandleTouch(const SDL_Event& event, bool ui) {
    const SDL_TouchFingerEvent& finger = event.tfinger;
    if (!SosTouchRoute::AcceptedTouch(finger.touchID, SDL_GetTouchDeviceType(finger.touchID))) {
        return;
    }
    const SosTouchArea area = GameTouchArea();
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
        if (ui) {
            return;
        }
        NoteTouch(SOS_TOUCH_EVENT_DETECTED);
        NoteTouch(SOS_TOUCH_EVENT_TOUCHPAD_USED);
        cursor.ControllerActivity(); // a finger is not the mouse: the pointer hides as for a controller
        WithTouchscreen([&](SosTouchscreen& s) {
            sos_touchscreen_down(&s, finger.fingerID, area, finger.x, finger.y, finger.timestamp);
        });
        break;
    case SDL_EVENT_FINGER_MOTION:
        WithTouchscreen([&](SosTouchscreen& s) { sos_touchscreen_motion(&s, finger.fingerID, area, finger.x, finger.y); });
        break;
    case SDL_EVENT_FINGER_UP:
        WithTouchscreen([&](SosTouchscreen& s) {
            sos_touchscreen_up(&s, finger.fingerID, area, finger.x, finger.y, finger.timestamp);
        });
        break;
    default:
        WithTouchscreen([&](SosTouchscreen& s) { sos_touchscreen_cancel(&s, finger.fingerID); });
        break;
    }
}

// Window thread, every poll: the display refresh the display events reported (display_refresh_hz) is debounced and,
// once settled, published for the present thread, which retimes the guest vblank (DcpFramePacing::RefreshFollower).
void WindowSDL::FollowDisplayRefresh() {
    if (!refresh_follower.Started()) {
        const u64 policy = DcpFramePacing::g_live_vblank.policy.load(std::memory_order_acquire);
        if (!policy) {
            return; // the video driver has not chosen the startup vblank yet
        }
        refresh_follower.Start(policy);
    }
    // A refresh change some sessions report without a display event (a Steam Deck switched between 60 and 90 Hz, or
    // a mode set behind SDL's back) is still seen: the current mode is read again every 2 s, on this thread only.
    const u64 now = SDL_GetTicks();
    static u64 refresh_polled_at = 0; // window thread only
    if (now - refresh_polled_at >= 2000) {
        refresh_polled_at = now;
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
            if (mode->refresh_rate > 0.0f) {
                DcpSettings::Get().display_refresh_hz = mode->refresh_rate;
            }
        }
    }
    if (const u64 live = refresh_follower.Observe(DcpSettings::Get().display_refresh_hz.load(std::memory_order_relaxed),
                                                  now)) {
        DcpFramePacing::g_live_vblank.target.store(live, std::memory_order_relaxed);
    }
}

bool WindowSDL::PollEvents() {
    if (close_requested.load(std::memory_order_relaxed)) {
        if (is_open.exchange(false)) {
            std::printf("Exit Game: hard exit\n");
            ReleaseMouse();
            ReleaseGamescopeTouch(); // the window thread ends the process next: no destructor runs
        }
    }
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (const int mode = DcpSettings::Get().display_mode; mode != display_mode) {
        ApplyDisplayMode(mode);
    }
    UpdateFpsTitle();
    RefreshTitle();
    DcpOverlay::SetDisplayScale(SDL_GetWindowDisplayScale(window));
    cursor.SetMouseControls(dcp_mouse_enabled(dcp_mouse_global()) != 0);
    // Steam writes its own touch mode on its Gamescope root, so Passthrough is rewritten while the game has focus.
    if (g_gamescope_touch.Claimed() && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&
        g_gamescope_touch.Maintain(SDL_GetTicks())) {
        LogTouchRoute(SOS_TOUCH_ROUTE_REASSERT);
    }
    const bool touch_routed = g_touch_routed.load(std::memory_order_relaxed);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (DcpLatencyTrace::enabled) {
            TracePress(event);
        }
        const bool ui = DcpOverlay::CapturesInput();
        const bool controller_activity = TrackControllerEvent(event);
        const SDL_WindowID game_window = SDL_GetWindowID(window);
        // Off the Gamescope touch route every event is Unchanged; on it SDL's mouse copy of a touch is not the mouse.
        const SosTouchRoute::Use touch = SosTouchRoute::Classify(event, touch_routed);
        if (controller_activity) {
            cursor.ControllerActivity();
        }
        switch (touch == SosTouchRoute::Use::DropMouse ? Uint32(SDL_EVENT_FIRST) : event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            cursor.MouseMoved(event.motion.windowID, game_window, event.motion.xrel, event.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            cursor.MouseButtonPressed(event.button.windowID, game_window);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            cursor.MouseWheelMoved(event.wheel.windowID, game_window, event.wheel.x, event.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN: {
            const bool host_shortcut = event.key.scancode == DcpSettings::Get().window_mode_key ||
                                       event.key.scancode == DcpSettings::Get().menu_key ||
                                       event.key.scancode == SDL_SCANCODE_F1 ||
                                       (ui && event.key.key == SDLK_ESCAPE);
            if (event.key.windowID == game_window && DcpCursor::IsGameplayKey(event.key.repeat, host_shortcut)) {
                cursor.KeyboardActivity();
            }
            break;
        }
        default:
            break;
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (DcpOverlay::HandleEvent(event)) {
            continue;
        }
        if (controller_activity && !ui) {
            dcp_mouse_controller_activity(dcp_mouse_global());
        }
        if (touch == SosTouchRoute::Use::Finger) {
            HandleTouch(event, ui);
            continue;
        }
        if (touch == SosTouchRoute::Use::DropMouse) { // never a mouse press as well as a touchpad contact
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                NoteTouch(SOS_TOUCH_EVENT_MOUSE_SUPPRESSED);
            }
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (!ui) {
                const float to_game = SDL_GetWindowPixelDensity(window) * dcp_mouse_game_scale(GetWidth(), GetHeight());
                dcp_mouse_motion(dcp_mouse_global(), event.motion.xrel * to_game, event.motion.yrel * to_game);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!ui) {
                dcp_mouse_button(dcp_mouse_global(), event.button.button, 1);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            dcp_mouse_button(dcp_mouse_global(), event.button.button, 0);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (!ui) {
                dcp_mouse_wheel(dcp_mouse_global(), event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y
                                                                                                   : event.wheel.y);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            ReleaseMouse();
            ReleaseGamescopeTouch();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            ClaimGamescopeTouch();
            break;
        case SDL_EVENT_KEY_DOWN: {
            const bool window_shortcut = event.key.scancode == DcpSettings::Get().window_mode_key;
            if (window_shortcut && !event.key.repeat) {
                ToggleBorderless();
            }
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            LogGamescopeWindow();
            LogSurfaceGeometry("changed");
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
                DcpSettings::Get().display_refresh_hz = mode->refresh_rate;
            }
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            ApplyWindowEvent(DcpPresent::WindowEvent::Minimized);
            break;
        case SDL_EVENT_WINDOW_HIDDEN:
            ApplyWindowEvent(DcpPresent::WindowEvent::Hidden);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            ApplyWindowEvent(DcpPresent::WindowEvent::Restored);
            break;
        case SDL_EVENT_WINDOW_SHOWN:
            ApplyWindowEvent(DcpPresent::WindowEvent::Shown);
            break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
            ApplyWindowEvent(DcpPresent::WindowEvent::Maximized);
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            ReleaseMouse();
            ReleaseGamescopeTouch(); // the window thread ends the process next: no destructor runs
            break;
        default:
            break;
        }
    }
    FollowDisplayRefresh();
    UpdateMouseCapture();
    UpdateCursor();
    return is_open;
}

} // namespace Frontend

// The pad sampler (runtime_pad.c), one call per game read of player 1: the screen's contacts as touchpad points. Off the
// Gamescope touch route this is one atomic load and 0 contacts.
extern "C" int dcpgpu_touchscreen_sample(uint16_t* x, uint16_t* y, uint8_t* id, uint32_t* buttons) {
    *buttons = 0;
    if (!Frontend::g_touch_routed.load(std::memory_order_relaxed)) {
        return 0;
    }
    SosTouchSample sample;
    Frontend::WithTouchscreen([&sample](SosTouchscreen& s) { sos_touchscreen_sample(&s, &sample); });
    for (int i = 0; i < sample.count; ++i) {
        x[i] = sample.x[i];
        y[i] = sample.y[i];
        id[i] = sample.id[i];
    }
    *buttons = sample.buttons;
    return sample.count;
}

// Every exit path, any thread, before the process ends with _Exit/_exit (no destructor or atexit handler runs): each
// Gamescope root gets its own touch click mode back. Nothing happens when nothing was claimed.
extern "C" void dcpgpu_touch_release(void) {
    if (const int restored = Frontend::g_gamescope_touch.Release(); restored > 0) {
        std::printf("Touchscreen telemetry: event=gamescope_touch_release_exit roots=%d count=1\n", restored);
    }
}
