// Neutral host overlay: Sons of Sparta needs the GPU presenter's overlay hook, not a game menu.
#include "dcp_overlay.h"

#include <atomic>

namespace DcpOverlay {
namespace {
std::atomic<u64> presented{0};
}

void Init(const Vulkan::Instance&, vk::Format, u32) {}
void InitOpenGL() {}
bool HandleEvent(const SDL_Event&) { return false; }
void SetLoading(bool, u32, u32) {}
void SetBackgroundLoading(u32, u32) {}
void ShowNotice(const std::string&) {}
bool Visible() { return false; }
void LogPresent() { presented.fetch_add(1, std::memory_order_relaxed); }
u64 PresentedFrames() { return presented.load(std::memory_order_relaxed); }
void Render(vk::CommandBuffer, vk::ImageView, vk::Extent2D) {}
void RenderOpenGL(u32, u32) {}
void SetDisplayScale(float) {}
bool CapturesInput() { return false; }
void SetChoiceInput(uint32_t (*)()) {}
void PollChoiceInput() {}
void SetTextEntry(bool, const std::string&, const std::string&) {}
void BeginChoice(const std::string&, const std::vector<std::string>&, std::vector<DcpPng::Image>, int) {}
int PollChoice() { return -2; }

} // namespace DcpOverlay
