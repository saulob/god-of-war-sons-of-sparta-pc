// SPDX-License-Identifier: GPL-2.0-or-later
// Sons of Sparta's overlay: the frame counter, the start-up and background shader-cache notes and short notices, drawn
// by the presenter into the swapchain image. The game has no menus or dialogs of its own here.
#include "dcp_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "dcp_overlay_box.h"
#include "dcp_settings.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include "sos_accent.h"
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
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
float last_interval_ms = 0.0f;
std::atomic<u64> presented_frames{0};

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

void CreateImGuiContext() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "dcp";
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
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
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

bool HandleEvent(const SDL_Event&) { return false; }

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
    return initialized && (loading || background_total || DcpSettings::Get().show_fps || NoticeShown());
}

void LogPresent() { presented_frames.fetch_add(1, std::memory_order_relaxed); }
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
bool CapturesInput() { return false; }
void SetChoiceInput(uint32_t (*)()) {}
void PollChoiceInput() {}
void SetTextEntry(bool, const std::string&, const std::string&) {}
void BeginChoice(const std::string&, const std::vector<std::string>&, std::vector<DcpPng::Image>, int) {}
int PollChoice() { return -2; }

} // namespace DcpOverlay
