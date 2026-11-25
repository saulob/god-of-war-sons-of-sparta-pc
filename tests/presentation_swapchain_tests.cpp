// Swapchain health on the production state machine: SUCCESS, SUBOPTIMAL and OUT_OF_DATE are distinct, a persistently
// suboptimal surface is kept instead of recreated every frame, required recreations are never suppressed. No window or GPU.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "dcp_present.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

using DcpPresent::SwapchainHealth;
using DcpPresent::SwapResult;
using DcpPresent::SwapStep;

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

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

struct Frames {
    SwapchainHealth health;
    int recreations = 0;
    int drawn = 0;

    // The presenter's frame: an acquire that hands out an image is always drawn and presented, and a recreation is
    // taken after that; when no image was handed out it is taken at once.
    void Frame(SwapResult acquire, SwapResult present) {
        const SwapStep step = health.Acquire(acquire);
        if (!step.usable) {
            Recreate();
            return;
        }
        ++drawn;
        health.Present(present);
        if (health.NeedsRecreation()) {
            Recreate();
        }
    }

    void Recreate() {
        health.Recreated();
        ++recreations;
    }
};

void TestResults() {
    SwapchainHealth h;
    SwapStep step = h.Acquire(SwapResult::Success);
    Check(step.usable && !step.recreate, "SUCCESS acquire is usable and asks for nothing");
    step = h.Present(SwapResult::Success);
    Check(step.usable && !step.recreate && !h.NeedsRecreation(), "SUCCESS present queues the image");

    step = h.Acquire(SwapResult::Suboptimal);
    Check(step.usable, "a SUBOPTIMAL acquire still yields a usable image");
    Check(step.recreate && h.NeedsRecreation(), "SUBOPTIMAL asks for one recreation");
    step = h.Present(SwapResult::Suboptimal);
    Check(step.usable, "a SUBOPTIMAL present counts as queued");

    SwapchainHealth h2;
    step = h2.Acquire(SwapResult::OutOfDate);
    Check(!step.usable && step.recreate && h2.NeedsRecreation(), "OUT_OF_DATE acquire hands out no image and recreates now");
    SwapchainHealth h3;
    h3.Acquire(SwapResult::Success);
    step = h3.Present(SwapResult::OutOfDate);
    Check(!step.usable && step.recreate, "OUT_OF_DATE present recreates and is not queued");
    SwapchainHealth h4;
    step = h4.Acquire(SwapResult::Failure);
    Check(!step.usable && !h4.NeedsRecreation(), "a fatal result is a failure, not a recreation");
    step = h4.Present(SwapResult::Failure);
    Check(!step.usable && !h4.NeedsRecreation(), "a fatal present is a failure, not a recreation");
}

void TestPersistentSuboptimal() {
    Frames f;
    f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    Check(f.recreations == 1 && f.drawn == 1, "the first SUBOPTIMAL frame is drawn and asks for one recreation");
    for (int i = 0; i < 120; ++i) {
        f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    }
    Check(f.recreations == 1, "121 SUBOPTIMAL frames recreate once, not once per frame");
    Check(f.drawn == 121, "every persistently SUBOPTIMAL frame is drawn and presented");
    Check(!f.health.NeedsRecreation(), "a kept SUBOPTIMAL swapchain asks for nothing");

    Frames mixed;
    mixed.Frame(SwapResult::Suboptimal, SwapResult::Success);
    for (int i = 0; i < 50; ++i) {
        mixed.Frame(SwapResult::Success, SwapResult::Suboptimal);
    }
    Check(mixed.recreations == 1, "a SUBOPTIMAL present alone on the replacement is kept too");

    f.Frame(SwapResult::Success, SwapResult::Suboptimal);
    f.Frame(SwapResult::Suboptimal, SwapResult::Success);
    Check(f.recreations == 1, "a clean acquire without a clean present does not end the suppression");
    f.Frame(SwapResult::Success, SwapResult::Success);
    f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    Check(f.recreations == 2, "after a clean SUCCESS cycle a later SUBOPTIMAL recreates once more");
    for (int i = 0; i < 120; ++i) {
        f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    }
    Check(f.recreations == 2, "and is then kept again");

    Frames only_present;
    only_present.Frame(SwapResult::Success, SwapResult::Suboptimal);
    Check(only_present.recreations == 1, "a SUBOPTIMAL present recreates once");
}

void TestRequiredOverrides() {
    Frames f;
    f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    const int before = f.recreations;
    f.Frame(SwapResult::OutOfDate, SwapResult::Success);
    Check(f.recreations == before + 1, "OUT_OF_DATE acquire overrides the persistent SUBOPTIMAL suppression");

    Frames g;
    g.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    g.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    g.Frame(SwapResult::Success, SwapResult::OutOfDate);
    Check(g.recreations == 2, "OUT_OF_DATE present overrides the suppression");

    Frames h;
    h.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    h.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    h.Recreate();
    Check(h.recreations == 2, "an explicit resize, VSync, HDR or surface recreation always happens");
    h.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    Check(h.recreations == 3, "and the new swapchain may ask for one SUBOPTIMAL recreation again");
    for (int i = 0; i < 50; ++i) {
        h.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    }
    Check(h.recreations == 3, "which is then kept");

    Frames o;
    o.Frame(SwapResult::Suboptimal, SwapResult::Success);
    o.Frame(SwapResult::OutOfDate, SwapResult::Success);
    o.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    Check(o.recreations == 3, "a replacement made for OUT_OF_DATE is not a kept SUBOPTIMAL one");
}

void TestAcquiredImageLifetime() {
    SwapchainHealth h;
    const SwapStep acquire = h.Acquire(SwapResult::Suboptimal);
    Check(acquire.usable && h.NeedsRecreation(),
          "a SUBOPTIMAL acquire leaves the recreation pending while its image is still to be presented");
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const size_t acquire_at = presenter.find("acquired = swapchain.AcquireNextImage();");
    const size_t present_at = presenter.find("presented = swapchain.Present();");
    Check(acquire_at != std::string::npos && present_at > acquire_at, "the frame path acquires, then presents");
    const std::string between = presenter.substr(acquire_at, present_at - acquire_at);
    size_t recreates = 0;
    for (size_t at = between.find("Recreate("); at != std::string::npos; at = between.find("Recreate(", at + 1)) {
        ++recreates;
    }
    Check(recreates == 1 && between.find("if (!acquired) {") < between.find("Recreate("),
          "the only recreation between an acquire and its present is the retry after an acquire with no image");
    Check(Contains(presenter, "if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {"),
          "the pending recreation is taken right after the present");
    const size_t loading = presenter.find("void Presenter::PresentLoadingFrame() {");
    const size_t loading_end = presenter.find("swapchain.AcquireNextImage()", loading);
    const std::string loading_head = presenter.substr(loading, loading_end - loading);
    const size_t loading_present = presenter.find("swapchain.Present();", loading_end);
    Check(loading_present != std::string::npos &&
              !Contains(presenter.substr(loading_end, loading_present - loading_end), "Recreate("),
          "the loading frame does not recreate between its acquire and its present");
    Check(loading != std::string::npos && Contains(loading_head, "window.GetWidth() != swapchain.GetWidth()") &&
              Contains(loading_head, "swapchain.NeedsRecreation()"),
          "the loading frame honours a pending recreation and a resize before it acquires");
}

void TestSourceWiring() {
    const std::string swapchain = Source("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    Check(Contains(swapchain, "health.Acquire(kind)") && Contains(swapchain, "health.Present(kind)") &&
              Contains(swapchain, "health.Recreated()"),
          "every acquire, present and creation goes through the health machine");
    Check(Contains(swapchain, "return step.usable;") && Contains(swapchain, "return !health.NeedsRecreation();"),
          "acquire reports the image usable, present reports a pending recreation");
    Check(Contains(swapchain, "case vk::Result::eErrorOutOfDateKHR:\n        return DcpPresent::SwapResult::OutOfDate;"),
          "present classifies OUT_OF_DATE");
    Check(Contains(swapchain, "case vk::Result::eSuboptimalKHR:\n        return DcpPresent::SwapResult::Suboptimal;"),
          "SUBOPTIMAL is its own class");
    Check(!Contains(swapchain, "eErrorDeviceLost"), "device lost is never classified as a swapchain result");
    Check(Contains(swapchain, "Common::DeviceLost::Operation::AcquireNextImage") &&
              Contains(swapchain, "Common::DeviceLost::Operation::QueuePresent") &&
              Contains(swapchain, "Common::DeviceLost::Timeline::Present"),
          "acquire and present stay behind the device-lost guard on the Present timeline");
    const size_t present = swapchain.find("bool Swapchain::Present() {");
    const size_t present_end = swapchain.find("DcpPresent::SwapResult Swapchain::AcquireResult", present);
    const std::string present_body = swapchain.substr(present, present_end - present);
    Check(!Contains(present_body, "WaitDeviceIdle") && !Contains(present_body, "waitIdle"),
          "no device or queue wait on the present path");
    Check(Contains(swapchain, "if (DcpSettings::Get().vsync == vsync) {\n        return;\n    }") &&
              Contains(swapchain, "FindPresentMode();\n    Recreate(width, height);"),
          "a VSync change recreates directly, outside the suppression");
    Check(Contains(swapchain, "needs_hdr = hdr;\n    Recreate(width, height);"), "an HDR change recreates directly");
    Check(Contains(swapchain, "if (kind != DcpPresent::SwapResult::Suboptimal || step.recreate || logged_kept_suboptimal)"),
          "a kept SUBOPTIMAL swapchain is logged once");
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    Check(Contains(presenter, "if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {\n        if (!swapchain.Recreate("),
          "a window resize recreates directly, outside the suppression");
}

void TestSurfaceWithoutArea() {
    using DcpPresent::SurfaceHasArea;
    Check(SurfaceHasArea(1920, 1080, 1920, 1080), "a normal surface can hold a swapchain");
    Check(!SurfaceHasArea(0, 0, 0, 0) && !SurfaceHasArea(0, 0, 16384, 16384), "a minimized surface cannot");
    Check(!SurfaceHasArea(1920, 0, 1920, 1080) && !SurfaceHasArea(0, 1080, 1920, 1080), "one empty dimension is enough");
    Check(SurfaceHasArea(UINT32_MAX, UINT32_MAX, 16384, 16384), "a surface that follows the swapchain can");
    Check(!SurfaceHasArea(UINT32_MAX, UINT32_MAX, 0, 0), "unless its maximum is empty");

    SwapchainHealth h;
    h.RequestRecreation();
    h.Acquire(SwapResult::Success);
    h.Present(SwapResult::Success);
    Check(h.NeedsRecreation(), "a recreation held back by an empty surface stays pending");
    h.Recreated();
    Check(!h.NeedsRecreation(), "until the swapchain is recreated");

    const std::string swapchain = Source("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    const size_t create = swapchain.find("bool Swapchain::Create(");
    const std::string body = swapchain.substr(create, swapchain.find("SetSurfaceProperties();", create) - create);
    const size_t guard = body.find("if (!SurfaceHasArea()) {\n        health.RequestRecreation();\n        return false;");
    Check(guard != std::string::npos && guard < body.find("Destroy();") && guard < body.find("health.Recreated();"),
          "an empty surface keeps the old swapchain and leaves the recreation pending");
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::string skip = "if (!swapchain.Recreate(window.GetWidth(), window.GetHeight())) {\n            ";
    size_t skips = 0;
    for (size_t at = presenter.find(skip + "free_frame();\n            return;"); at != std::string::npos;
         at = presenter.find(skip + "free_frame();\n            return;", at + 1)) {
        ++skips;
    }
    Check(skips == 2 && Contains(presenter, skip + "return;"),
          "a frame that cannot be presented goes back to the free queue, a loading frame is skipped");
}

} // namespace

int main() {
    TestSurfaceWithoutArea();
    TestResults();
    TestPersistentSuboptimal();
    TestRequiredOverrides();
    TestAcquiredImageLifetime();
    TestSourceWiring();
    std::printf("presentation swapchain: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
