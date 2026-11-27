// SPDX-License-Identifier: GPL-2.0-or-later
// Sons of Sparta's overlay: the F10 cheat menu, the frame counter, the start-up and background shader-cache notes and
// short notices, drawn by the presenter into the swapchain image.
#include "dcp_overlay.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>
#include "dcp_overlay_box.h"
#include "dcp_settings.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include "runtime_cheats.h"
#include "sos_accent.h"
#include "sos_cheat_menu.h"
#ifdef _WIN32
#include "dcp_renderer.h"
#include "imgui_impl_gles3.h"
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace DcpOverlay {

namespace {

std::mutex imgui_mutex;
bool initialized = false;
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
std::string notice_text;
std::atomic<std::chrono::steady_clock::rep> notice_until{0};
float base_scale = 1.0f;
std::chrono::steady_clock::time_point last_present{}, last_game_frame{};
float frame_ms_avg = 0.0f;
float last_interval_ms = 0.0f;
std::atomic<u64> presented_frames{0};
// The F10 menu: open, or closing and still holding the game's input until what the menu received is released.
std::atomic<bool> cheat_open{false};
sos::cheats::CloseGate menu_close;
std::bitset<ImGuiKey_NamedKey_COUNT> menu_pressed;
bool cheat_focused = false;
sos::cheats::MenuList cheat_list;
sos::cheats::PadNav cheat_pad;
sos::cheats::PadState pad_state;

struct Panel {
    DcpOverlayBox::Box box;
    float size;
};

Panel PlacePanel(const char* text, int corner) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float size = ImGui::GetFontSize() * DcpSettings::Get().overlay_scale / 100.0f;
    const ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const DcpOverlayBox::Rect work{viewport->WorkPos.x, viewport->WorkPos.y, viewport->WorkPos.x + viewport->WorkSize.x,
                                   viewport->WorkPos.y + viewport->WorkSize.y};
    return {DcpOverlayBox::Place(corner, text_size.x, text_size.y, size, work), size};
}

void DrawPanel(const Panel& panel, const char* text) {
    constexpr ImU32 panel_color = IM_COL32(20, 12, 10, 200);
    constexpr ImU32 border_color = IM_COL32(sos::kAccentRed, sos::kAccentGreen, sos::kAccentBlue, 255);
    constexpr ImU32 text_color = IM_COL32(255, 255, 255, 255);
    const DcpOverlayBox::Rect& r = panel.box.rect;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), panel_color, panel.box.rounding);
    draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), border_color, panel.box.rounding, 0,
                  DcpOverlayBox::kBorderThickness);
    draw->AddText(ImGui::GetFont(), panel.size, ImVec2(r.x0 + panel.box.pad_x, r.y0 + panel.box.pad_y), text_color,
                  text);
}

std::string FrameCounterText() {
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                  frame_ms_avg);
    return text;
}

bool NoticeShown() {
    return std::chrono::steady_clock::now().time_since_epoch().count() < notice_until.load();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total, u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

ImVec4 Accent() {
    return ImVec4(sos::kAccentRed / 255.0f, sos::kAccentGreen / 255.0f, sos::kAccentBlue / 255.0f, 1.0f);
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_W: return ImGuiKey_W;
    case SDLK_S: return ImGuiKey_S;
    default: return ImGuiKey_None;
    }
}

uint32_t PadBitOf(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return sos::menu::kPadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return sos::menu::kPadDown;
    case SDL_GAMEPAD_BUTTON_SOUTH: return sos::menu::kPadActivate;
    case SDL_GAMEPAD_BUTTON_EAST: return sos::menu::kPadClose;
    default: return 0;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// The keys and buttons that went down while the menu was open, until they come up again.
void TrackMenuKeys() {
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const size_t index = size_t(key - ImGuiKey_NamedKey_BEGIN);
        if (ImGui::IsKeyPressed(ImGuiKey(key), false)) {
            menu_pressed.set(index);
        } else if (!ImGui::IsKeyDown(ImGuiKey(key))) {
            menu_pressed.reset(index);
        }
    }
}

struct RowText {
    const char* state;
    bool lit;
};

RowText CheatRowText(const sos::cheats::MenuRow& row) {
    using sos::cheats::RowKind;
    using sos::cheats::RowState;
    switch (row.kind) {
    case RowKind::Heading: return {nullptr, false};
    case RowKind::Toggle: {
        const auto state = RowState(runtime_cheat_toggle_state(int(row.toggle)));
        return {sos::cheats::StateText(state, "On"), state == RowState::On};
    }
    case RowKind::Multiplier: {
        const auto state = RowState(runtime_cheat_multiplier_state(int(row.multiplier)));
        const char* value = sos::cheats::ChoiceLabel(row.multiplier, runtime_cheat_multiplier_value(int(row.multiplier)));
        return {sos::cheats::StateText(state, value), state == RowState::On};
    }
    case RowKind::Action: {
        if (const char* notice = runtime_cheat_action_notice(int(row.action))) {
            return {notice, true};
        }
        const auto state = RowState(runtime_cheat_action_state(int(row.action)));
        return {state == RowState::Unavailable ? sos::cheats::kUnavailable
                : state == RowState::Pending   ? sos::cheats::kBusy
                                               : "",
                false};
    }
    }
    return {nullptr, false};
}

// True when the row was clicked.
bool DrawCheatRow(const sos::cheats::MenuRow& row, int index, bool selected, float row_height, const ImVec4& accent,
                  const ImVec4& focus, const ImVec4& dim) {
    ImGui::PushID(index);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (row.kind == sos::cheats::RowKind::Heading) {
        ImGui::Dummy(ImVec2(0.0f, row_height));
        const ImVec2 min = ImGui::GetItemRectMin();
        draw->AddText(ImVec2(min.x, min.y + (row_height - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(accent), row.label);
        ImGui::PopID();
        return false;
    }
    const bool pressed = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, row_height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (selected) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(focus), 3.0f);
    }
    const float pad = ImGui::GetFontSize() * 0.4f;
    const float y = min.y + (row_height - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(min.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text), row.label);
    const RowText text = CheatRowText(row);
    if (text.state != nullptr && *text.state != '\0') {
        draw->AddText(ImVec2(max.x - pad - ImGui::CalcTextSize(text.state).x, y), ImGui::GetColorU32(text.lit ? accent : dim),
                      text.state);
    }
    ImGui::PopID();
    return pressed;
}

void CloseCheatMenu() {
    cheat_open = false;
    menu_close = {};
    menu_pressed.reset();
    cheat_pad = {};
    cheat_focused = false;
    ImGui::GetIO().ClearInputKeys();
}

void OpenCheatMenu() {
    cheat_open = true;
    menu_close = {};
    menu_pressed.reset();
    cheat_pad = {};
    cheat_focused = false;
    cheat_list.Open();
}

void CheatKeyboardInput() {
    const auto pressed = [](ImGuiKey key, bool repeat) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_UpArrow, true) || pressed(ImGuiKey_W, true)) {
        cheat_list.Step(-1);
    }
    if (pressed(ImGuiKey_DownArrow, true) || pressed(ImGuiKey_S, true)) {
        cheat_list.Step(1);
    }
    if (pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false) || pressed(ImGuiKey_Space, false)) {
        sos::cheats::ActivateRow(sos::cheats::kMenuRows[size_t(cheat_list.selected)]);
    }
}

sos::cheats::Box FpsBox() {
    const DcpOverlayBox::Rect r = PlacePanel(FrameCounterText().c_str(), DcpSettings::Get().overlay_corner).box.rect;
    return {r.x0, r.y0, r.x1, r.y1};
}

void CheatMenu() {
    TrackMenuKeys();
    const sos::cheats::NavStep step = cheat_pad.Poll(pad_state.Take(), ImGui::GetTime());
    if (menu_close.requested()) {
        if (menu_close.Step(menu_pressed.any() || cheat_pad.Held() != 0, ImGui::GetTime())) {
            CloseCheatMenu();
        }
        return;
    }
    CheatKeyboardInput();
    sos::cheats::ApplyPadStep(step, cheat_list, menu_close);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec4 accent = Accent();
    const ImVec4 dim(0.70f, 0.66f, 0.62f, 1.0f);
    const ImVec4 focus(accent.x, accent.y, accent.z, 0.40f);
    const float em = ImGui::GetFontSize();
    const float margin = em * 0.8f;
    const float row_height = em * 1.6f;
    const float width = em * 20.0f;
    const float right = io.DisplaySize.x - margin;
    const bool fps_shown = DcpSettings::Get().show_fps && frame_ms_avg > 0.0f;
    const float top = sos::cheats::MenuTop(margin, fps_shown, fps_shown ? FpsBox() : sos::cheats::Box{},
                                           right - width, right, io.DisplaySize.y);
    ImGui::SetNextWindowPos(ImVec2(right, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(io.DisplaySize.y - top - margin, em)));
    if (!cheat_focused) {
        cheat_focused = true;
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.05f, 0.04f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, focus);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, focus);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##sos_cheat_menu", nullptr, flags)) {
        ImGui::TextColored(accent, "%s", sos::cheats::kMenuTitle);
        ImGui::Separator();
        const ImGuiStyle& style = ImGui::GetStyle();
        const size_t count = sos::cheats::kMenuRows.size();
        const float body = count * row_height + (count - 1) * style.ItemSpacing.y;
        const float footer = 3.0f * (em + style.ItemSpacing.y) + 2.0f * style.WindowPadding.y;
        const float room = std::max(io.DisplaySize.y - top - margin - footer - em, row_height);
        ImGui::BeginChild("##cheat_rows", ImVec2(0.0f, std::min(body, room)), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        const bool reveal = cheat_list.TakeReveal();
        int clicked = -1;
        for (size_t i = 0; i < count; ++i) {
            const bool selected = int(i) == cheat_list.selected;
            if (DrawCheatRow(sos::cheats::kMenuRows[i], int(i), selected, row_height, accent, focus, dim)) {
                clicked = int(i);
            }
            if (selected && reveal && !ImGui::IsItemVisible()) {
                ImGui::SetScrollHereY(0.5f);
            }
        }
        ImGui::EndChild();
        if (clicked >= 0) {
            cheat_list.Click(clicked);
            sos::cheats::ActivateRow(sos::cheats::kMenuRows[size_t(clicked)]);
        }
        ImGui::Separator();
        ImGui::TextColored(dim, "%s", sos::cheats::kMenuCloseHint);
        ImGui::TextColored(dim, "%s", sos::cheats::kMenuNote);
    }
    ImGui::End();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(4);
}

void PressCheatMenuKey(bool repeat) {
    switch (sos::cheats::TogglePress(repeat, cheat_open, loading)) {
    case sos::cheats::ToggleAction::Open: OpenCheatMenu(); break;
    case sos::cheats::ToggleAction::Close: menu_close.Request(); break;
    case sos::cheats::ToggleAction::None: break;
    }
}

void CreateImGuiContext() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "dcp";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::StyleColorsDark();
    ImFontConfig font_config;
    font_config.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&font_config);
}

bool BeginPresent() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_interval_ms = ms;
    runtime_cheats_frame();
    return Visible();
}

void BuildFrame(u32 width, u32 height, void (*new_frame)()) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = last_interval_ms > 0.0f && last_interval_ms < 1000.0f ? last_interval_ms / 1000.0f : 1.0f / 60.0f;
    const float scale = std::max(float(height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGui::GetStyle().ScaleAllSizes(scale / base_scale);
        ImGui::GetStyle().FontScaleMain = scale;
        base_scale = scale;
    }
    new_frame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (DcpSettings::Get().show_fps && frame_ms_avg > 0.0f) {
        const std::string label = FrameCounterText();
        DrawPanel(PlacePanel(label.c_str(), DcpSettings::Get().overlay_corner), label.c_str());
    }
    if (background_total && !loading) {
        BackgroundLoading();
    }
    if (NoticeShown()) {
        const int corner = DcpOverlayBox::NoticeCorner(DcpSettings::Get().overlay_corner);
        DrawPanel(PlacePanel(notice_text.c_str(), corner), notice_text.c_str());
    }
    if (cheat_open) {
        CheatMenu();
    }
    ImGui::Render();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    CreateImGuiContext();
    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(*static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));
    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

void InitOpenGL() {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    CreateImGuiContext();
#ifdef _WIN32
    const bool started = DcpRenderer::Active() == DcpRenderer::Backend::Direct3D11
                             ? InitImGuiGles3()
                             : ImGui_ImplOpenGL3_Init("#version 330 core");
#else
    const bool started = ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    if (!started) {
        std::printf("Overlay: ImGui OpenGL backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

// F10 opens and closes the cheat menu. While it is open it takes the keyboard, mouse and controller events; F11 and
// every other window shortcut still reach the window.
bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (event.key.scancode == DcpSettings::Get().menu_key) {
            if (down) {
                PressCheatMenuKey(event.key.repeat);
            }
            return true;
        }
        if (!cheat_open || event.key.scancode == DcpSettings::Get().window_mode_key) {
            return false;
        }
        if (event.key.key == SDLK_ESCAPE && down && !event.key.repeat) {
            menu_close.Request();
        }
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (const uint32_t bit = PadBitOf(event.gbutton.button)) {
            pad_state.Button(event.gbutton.which, bit, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        }
        return cheat_open;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            pad_state.StickY(event.gaxis.which, event.gaxis.value);
        }
        return cheat_open;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_state.Remove(event.gdevice.which);
        return false;
    case SDL_EVENT_MOUSE_MOTION: {
        if (!cheat_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!cheat_open) {
            return false;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            io.AddMouseButtonEvent(0, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!cheat_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    case SDL_EVENT_TEXT_INPUT: return cheat_open;
    default: return false;
    }
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void ShowNotice(const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    notice_text = text;
    notice_until = (std::chrono::steady_clock::now() + std::chrono::seconds(2)).time_since_epoch().count();
}

bool Visible() {
    return initialized && (loading || background_total || cheat_open || DcpSettings::Get().show_fps || NoticeShown());
}

void LogPresent() {
    presented_frames.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_game_frame).count();
    last_game_frame = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
}
u64 PresentedFrames() { return presented_frames.load(std::memory_order_relaxed); }

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    if (!BeginPresent()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    BuildFrame(extent.width, extent.height, ImGui_ImplVulkan_NewFrame);
    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

void RenderOpenGL(u32 width, u32 height) {
    if (!BeginPresent()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
#ifdef _WIN32
    if (DcpRenderer::Active() == DcpRenderer::Backend::Direct3D11) {
        BuildFrame(width, height, ImGuiGles3NewFrame);
        ImGuiGles3RenderDrawData(ImGui::GetDrawData());
        return;
    }
#endif
    BuildFrame(width, height, ImGui_ImplOpenGL3_NewFrame);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void SetDisplayScale(float) {}
bool CapturesInput() { return cheat_open; }
void SetChoiceInput(uint32_t (*)()) {}
void PollChoiceInput() {}
void SetTextEntry(bool, const std::string&, const std::string&) {}
void BeginChoice(const std::string&, const std::vector<std::string>&, std::vector<DcpPng::Image>, int) {}
int PollChoice() { return -2; }

} // namespace DcpOverlay
