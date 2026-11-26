// Swapchain health on the production state machine: SUCCESS, SUBOPTIMAL and OUT_OF_DATE are distinct, a persistently
// suboptimal surface is kept instead of recreated every frame, required recreations are never suppressed. The window
// lifecycle on the production present gate: a minimized or hidden window is never acquired from or presented to, the
// restore recreates once, a lost surface is recreated (and replaced) instead of crashing. No window or GPU.
#include <cstdint>
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

bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
    const size_t at = text.find(first);
    return at != std::string::npos && text.find(second, at + first.size()) != std::string::npos;
}

// The text of one function: from its signature to the next top-level signature given.
std::string Body(const std::string& text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at == std::string::npos) {
        return {};
    }
    const size_t end = text.find(to, at + from.size());
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
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
    for (std::uint32_t i = 0; i < SwapchainHealth::kOptionalSpacing; ++i) {
        f.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    }
    Check(f.recreations == 2,
          "after a clean SUCCESS cycle a later SUBOPTIMAL recreates once more, kOptionalSpacing presents after the last");
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
    for (size_t at = between.find("Recreate"); at != std::string::npos; at = between.find("Recreate", at + 1)) {
        ++recreates;
    }
    Check(recreates == 1 && between.find("if (!acquired) {") < between.find("RecreateSwapchain()"),
          "the only recreation between an acquire and its present is the retry after an acquire with no image");
    Check(InOrder(presenter, "presented = swapchain.Present();", "if (presented) {") &&
              InOrder(presenter, "if (presented) {", "} else {\n        RecreateSwapchain();"),
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
    const std::string present_result = Body(swapchain, "DcpPresent::SwapResult Swapchain::PresentResult(vk::Result result) {",
                                            "\n}\n");
    Check(Contains(present_result, "    case vk::Result::eErrorSurfaceLostKHR:\n    case vk::Result::eErrorOutOfDateKHR:\n"
                                   "        return DcpPresent::SwapResult::OutOfDate;"),
          "a lost surface at present is a recreation, not the fatal failure assert");
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
    Check(Contains(presenter, "if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||\n"
                              "        swapchain.NeedsRecreation()) {\n        if (!RecreateSwapchain()) {"),
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
    const size_t guard = body.find("if (state != DcpPresent::SurfaceState::HasArea) {\n"
                                   "        surface_lost = state == DcpPresent::SurfaceState::Lost;\n"
                                   "        health.RequestRecreation();\n        return false;");
    Check(guard != std::string::npos && guard < body.find("Destroy();") && guard < body.find("health.Recreated();"),
          "an empty surface keeps the old swapchain and leaves the recreation pending");
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::string skip = "if (!RecreateSwapchain()) {\n            free_frame();\n            return;";
    size_t skips = 0;
    for (size_t at = presenter.find(skip); at != std::string::npos; at = presenter.find(skip, at + 1)) {
        ++skips;
    }
    Check(skips == 2 && Contains(presenter, "if (!swapchain.Recreate(window.GetWidth(), window.GetHeight())) {\n            return;"),
          "a frame that cannot be presented goes back to the free queue, a loading frame is skipped");
}

// A surface that alternates clean and suboptimal frames: before the spacing every second frame recreated the swapchain
// (a device wait each time); now at most one optional recreation per kOptionalSpacing presents, every image presented.
void TestAlternatingSuboptimalIsBounded() {
    const int spacing = int(SwapchainHealth::kOptionalSpacing);
    const int frames = 10 * spacing;
    Frames f;
    for (int i = 0; i < frames; ++i) {
        const SwapResult result = i % 2 ? SwapResult::Success : SwapResult::Suboptimal;
        f.Frame(result, result);
    }
    Check(f.recreations >= 2 && f.recreations <= frames / spacing + 1,
          "alternating clean and SUBOPTIMAL frames recreate at most once per kOptionalSpacing presents");
    Check(f.drawn == frames, "and every image is still drawn and presented");

    // Inside the spacing an OUT_OF_DATE or an explicit recreation is honoured at once.
    SwapchainHealth h;
    h.Acquire(SwapResult::Suboptimal);
    h.Present(SwapResult::Suboptimal);
    h.Recreated();
    h.Acquire(SwapResult::Success);
    h.Present(SwapResult::Success);
    SwapStep step = h.Acquire(SwapResult::Suboptimal);
    Check(step.usable && !step.recreate, "a SUBOPTIMAL result inside the spacing is kept");
    step = h.Acquire(SwapResult::OutOfDate);
    Check(!step.usable && step.recreate && h.NeedsRecreation(), "OUT_OF_DATE inside the spacing still recreates at once");
    h.Recreated();
    step = h.Acquire(SwapResult::Suboptimal);
    Check(step.usable && step.recreate, "after a required recreation SUBOPTIMAL may ask again");
}

using DcpPresent::Lifecycle;
using DcpPresent::PresentGate;
using DcpPresent::SurfaceRecovery;
using DcpPresent::WindowEvent;
using DcpPresent::WindowVisibility;

// The presenter's frame (Presenter::Present, CanPresent, RecreateSwapchain, Swapchain::Create) on the production gate,
// health machine and surface recovery. A lost surface at present is classified as Swapchain::PresentResult does it.
struct LifecycleModel {
    PresentGate gate;
    SwapchainHealth health;
    SurfaceRecovery recovery;
    WindowVisibility visibility;
    bool gamescope = false;
    std::uint32_t window_width = 1920;
    std::uint32_t window_height = 1080;
    bool surface_zero = false;
    bool surface_lost = false; ///< Swapchain::surface_lost
    bool vsync_changed = false;
    bool fatal = false;
    std::uint32_t swapchain_width = 1920;
    std::uint32_t swapchain_height = 1080;
    int acquires = 0;
    int presents = 0;
    int recreations = 0;
    int zero_surface_recreations = 0;
    int acquires_while_unpresentable = 0;
    int resumed = 0;
    int freed = 0;
    int surfaces_replaced = 0;

    void Apply(WindowEvent event) {
        visibility.Apply(event);
    }

    bool Unpresentable() const {
        return (!gamescope && !visibility.Visible()) || surface_zero || window_width == 0 || window_height == 0;
    }

    // Swapchain::Create: a lost surface is replaced first, an empty one keeps the recreation pending.
    bool Create() {
        if (surface_lost) {
            if (!recovery.Lost()) {
                fatal = true;
                return false;
            }
            surface_lost = false;
            ++surfaces_replaced;
        }
        if (surface_zero) {
            health.RequestRecreation();
            return false;
        }
        health.Recreated();
        swapchain_width = window_width;
        swapchain_height = window_height;
        ++recreations;
        return true;
    }

    // Presenter::RecreateSwapchain.
    bool Recreate() {
        if (window_width == 0 || window_height == 0) {
            health.RequestRecreation();
            return false;
        }
        if (!Create()) {
            gate.SurfaceUnavailable();
            return false;
        }
        return true;
    }

    // `lost_at_present`: vkQueuePresentKHR returned VK_ERROR_SURFACE_LOST_KHR.
    void Frame(SwapResult acquire = SwapResult::Success, SwapResult present = SwapResult::Success,
               bool lost_at_present = false) {
        if (fatal) {
            return;
        }
        const bool check_surface = gate.Suspended() || health.NeedsRecreation();
        const auto step = gate.Update({gamescope ? WindowVisibility{} : visibility, window_width, window_height,
                                       check_surface && surface_zero});
        if (step.recreate) {
            health.RequestRecreation();
        }
        if (!step.present) {
            ++freed;
            return;
        }
        if (vsync_changed) {
            vsync_changed = false;
            Create();
        }
        if (window_width != swapchain_width || window_height != swapchain_height || health.NeedsRecreation()) {
            if (!Recreate()) {
                ++freed;
                return;
            }
        }
        ++acquires;
        acquires_while_unpresentable += Unpresentable() ? 1 : 0;
        if (!health.Acquire(acquire).usable) {
            if (!Recreate()) {
                ++freed;
                return;
            }
            health.Acquire(SwapResult::Success);
        }
        if (lost_at_present) {
            surface_lost = true;
            present = SwapResult::OutOfDate; // Swapchain::PresentResult
        }
        if (health.Present(present).usable) {
            recovery.Presented();
            ++presents;
        }
        // Swapchain::Present reports a pending recreation as not presented.
        if (!health.NeedsRecreation()) {
            resumed += gate.Presented() ? 1 : 0;
        } else {
            Recreate();
        }
        ++freed;
    }

    void Frames(int count) {
        for (int i = 0; i < count; ++i) {
            Frame();
        }
    }
};

void TestWindowVisibilityEvents() {
    WindowVisibility v;
    v.Apply(WindowEvent::Minimized);
    Check(v.minimized && !v.hidden && !v.Visible(), "MINIMIZED hides the window from the presenter");
    v.Apply(WindowEvent::Restored);
    Check(!v.minimized && v.Visible(), "RESTORED shows it again");
    v.Apply(WindowEvent::Hidden);
    Check(v.hidden && !v.minimized && !v.Visible(), "HIDDEN hides it");
    v.Apply(WindowEvent::Shown);
    Check(!v.hidden && v.Visible(), "SHOWN shows it");
    v.Apply(WindowEvent::Minimized);
    v.Apply(WindowEvent::Hidden);
    v.Apply(WindowEvent::Shown);
    Check(!v.Visible(), "a minimized window stays unpresentable when only SHOWN arrives");
    v.Apply(WindowEvent::Restored);
    Check(v.Visible(), "and is presentable after RESTORED too");
    bool round_trip = true;
    for (int bits = 0; bits < 4; ++bits) {
        round_trip = round_trip && WindowVisibility::Unpack(std::uint8_t(bits)).Pack() == bits;
    }
    Check(round_trip, "the visibility crosses threads as two bits");
}

// Alt+Tab without a minimize is FOCUS_LOST / FOCUS_GAINED only: focus is not an input of the gate.
void TestFocusAloneKeepsPresenting() {
    LifecycleModel m;
    m.Frames(305);
    Check(m.presents == 305 && m.recreations == 0 && m.gate.Current() == Lifecycle::Presentable,
          "a visible window presents every frame and never recreates");
}

void TestMinimizeRestore() {
    LifecycleModel m;
    m.Frames(10);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.Frames(120);
    Check(m.presents == 10 && m.acquires == 10 && m.acquires_while_unpresentable == 0,
          "a minimized window is never acquired from or presented to");
    Check(m.freed == 130, "every frame still goes back to the free queue: the guest never waits for one");
    Check(m.recreations == 0 && m.gate.Current() == Lifecycle::Minimized, "minimizing recreates nothing");
    m.Apply(WindowEvent::Restored);
    m.Apply(WindowEvent::Shown);
    m.surface_zero = false;
    m.Frames(120);
    Check(m.presents == 130 && m.recreations == 1 && m.resumed == 1 && m.zero_surface_recreations == 0,
          "restoring recreates once and presents again");
    Check(m.gate.Current() == Lifecycle::Presentable, "and the lifecycle is presentable again");
}

// A window that was maximized before it was minimized: some SDL3 backends bring it back with MAXIMIZED (and SHOWN) only,
// never RESTORED. The window thread (WindowSDL::ApplyWindowEvent) applies the event and then re-reads the window flags;
// either must end the minimize, or the gate pauses presenting for good (a frozen or black window).
void TestMaximizedOnlyRestore() {
    LifecycleModel events;
    events.Frames(10);
    events.Apply(WindowEvent::Minimized);
    events.surface_zero = true;
    events.Frames(60);
    Check(events.presents == 10 && events.gate.Current() == Lifecycle::Minimized, "a maximized window is minimized");
    events.Apply(WindowEvent::Maximized);
    events.surface_zero = false;
    events.Frames(60);
    Check(events.presents == 70 && events.recreations == 1 && events.resumed == 1 &&
              events.gate.Current() == Lifecycle::Presentable,
          "a MAXIMIZED-only restore presents again, with one recreation");

    // The flags path: only SHOWN arrives, but SDL already reports the window neither minimized nor hidden.
    LifecycleModel flags;
    flags.Frames(5);
    flags.Apply(WindowEvent::Minimized);
    flags.visibility.Sync(true, false);
    flags.surface_zero = true;
    flags.Frames(30);
    flags.Apply(WindowEvent::Shown);
    flags.visibility.Sync(false, false); // SDL_GetWindowFlags after the event: not SDL_WINDOW_MINIMIZED
    flags.surface_zero = false;
    flags.Frames(30);
    Check(flags.presents == 35 && flags.gate.Current() == Lifecycle::Presentable,
          "the window flags end a minimize no event reported");
    WindowVisibility still;
    still.Apply(WindowEvent::Minimized);
    still.Apply(WindowEvent::Shown);
    still.Sync(true, false);
    Check(!still.Visible(), "a window SDL still reports minimized stays paused whatever event came");

    const std::string window = Source("gpu/shim/window.cpp");
    Check(Contains(window, "case SDL_EVENT_WINDOW_MAXIMIZED:\n            ApplyWindowEvent(DcpPresent::WindowEvent::Maximized);"),
          "the window passes MAXIMIZED to the presenter");
    Check(Contains(window, "visibility.Apply(event);\n") &&
              Contains(window, "visibility.Sync((flags & SDL_WINDOW_MINIMIZED) != 0, (flags & SDL_WINDOW_HIDDEN) != 0);"),
          "every window event re-derives minimized and hidden from SDL_GetWindowFlags");
}

void TestHiddenShown() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Hidden);
    m.Frames(50);
    Check(m.presents == 5 && m.acquires_while_unpresentable == 0 && m.gate.Current() == Lifecycle::Hidden,
          "a hidden window is not presented to (a FIFO present could block)");
    m.Apply(WindowEvent::Shown);
    m.Frames(50);
    Check(m.presents == 55 && m.recreations == 1, "SHOWN resumes with one recreation");
}

// SDL keeps the last pixel size while minimized on Windows, so a window size check alone never fires: the Vulkan
// surface decides, even before the window thread has pumped the minimize.
void TestSurfaceZeroBeforeMinimizeEvent() {
    LifecycleModel m;
    m.Frames(3);
    m.surface_zero = true;
    m.Frame(SwapResult::OutOfDate);
    Check(m.recreations == 0 && m.gate.Current() == Lifecycle::SurfaceSizeZero,
          "an empty surface met by a recreation suspends presenting");
    m.Frames(100);
    Check(m.presents == 3 && m.recreations == 0, "and nothing is acquired while it stays empty");
    m.surface_zero = false;
    m.Frames(10);
    Check(m.presents == 13 && m.recreations == 1, "the surface coming back recreates once");
}

void TestZeroWindowSizeThenValid() {
    LifecycleModel m;
    m.Frames(5);
    m.window_width = 0;
    m.window_height = 0;
    m.Frames(30);
    Check(m.presents == 5 && m.recreations == 0 && m.gate.Current() == Lifecycle::WindowSizeZero,
          "a 0x0 window is not presented to");
    m.window_width = 1280;
    m.window_height = 720;
    m.Frames(30);
    Check(m.presents == 35 && m.recreations == 1 && m.swapchain_width == 1280, "a valid size recreates once at it");
}

void TestRestoreEventsBeforeValidSize() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.Frames(10);
    for (int i = 0; i < 5; ++i) {
        m.Apply(WindowEvent::Restored);
        m.Apply(WindowEvent::Shown);
        m.Frames(5);
    }
    Check(m.recreations == 0 && m.zero_surface_recreations == 0 && m.presents == 5 && m.acquires_while_unpresentable == 0,
          "RESTORED before the surface has area creates no 0x0 swapchain");
    m.surface_zero = false;
    m.Frames(20);
    Check(m.recreations == 1 && m.presents == 25 && m.resumed == 1, "the surface getting its area recreates once");
}

void TestRestoreClusterRecreatesOnce() {
    LifecycleModel m;
    m.Frames(5);
    m.Apply(WindowEvent::Minimized);
    m.Frames(5);
    // RESTORED + SHOWN + RESIZED + PIXEL_SIZE_CHANGED between two frames: one recreation, not four.
    m.Apply(WindowEvent::Restored);
    m.Apply(WindowEvent::Shown);
    m.window_width = 2560;
    m.window_height = 1440;
    m.Frames(60);
    Check(m.recreations == 1 && m.presents == 65, "a restore cluster of events recreates once");
}

void TestMinimizeWithPendingRecreation() {
    LifecycleModel m;
    m.Frames(5);
    m.Frame(SwapResult::Suboptimal, SwapResult::Suboptimal);
    m.Apply(WindowEvent::Minimized);
    m.surface_zero = true;
    m.health.RequestRecreation();
    m.Frames(40);
    Check(m.recreations == 1 && m.acquires_while_unpresentable == 0,
          "a recreation pending at minimize waits for the restore");
    m.Apply(WindowEvent::Restored);
    m.surface_zero = false;
    m.Frames(40);
    Check(m.recreations == 2 && m.presents == 5 + 1 + 40, "and is taken once then");
}

void TestRepeatedMinimizeRestoreNoStorm() {
    LifecycleModel m;
    for (int cycle = 0; cycle < 25; ++cycle) {
        m.Frames(5);
        m.Apply(WindowEvent::Minimized);
        m.surface_zero = true;
        m.Frames(30);
        m.Apply(WindowEvent::Restored);
        m.Apply(WindowEvent::Shown);
        m.surface_zero = false;
        m.Frames(5);
    }
    Check(m.recreations == 25 && m.resumed == 25 && m.acquires_while_unpresentable == 0,
          "25 minimize/restore cycles recreate 25 times, never a storm");
}

// Gamescope has no minimize: SDL visibility is ignored there and presenting never pauses for it.
void TestGamescopeIgnoresVisibility() {
    LifecycleModel m;
    m.gamescope = true;
    m.Frames(5);
    m.Apply(WindowEvent::Hidden);
    m.Apply(WindowEvent::Minimized);
    m.Frames(20);
    Check(m.presents == 25 && m.recreations == 0 && m.gate.Current() == Lifecycle::Presentable,
          "under Gamescope a hidden or minimized SDL window keeps presenting");
}

void TestLifecycleLogBounded() {
    DcpPresent::LifecycleLog log;
    int lines = 0;
    int notes = 0;
    for (int i = 0; i < 500; ++i) {
        switch (log.Observe(true)) {
        case DcpPresent::LifecycleLog::Action::Log:
            ++lines;
            break;
        case DcpPresent::LifecycleLog::Action::LastNote:
            ++notes;
            break;
        case DcpPresent::LifecycleLog::Action::None:
            break;
        }
    }
    Check(lines == int(DcpPresent::LifecycleLog::kMaxLines) && notes == 1,
          "lifecycle lines stop after kMaxLines with one closing note");
    DcpPresent::LifecycleLog quiet;
    Check(quiet.Observe(false) == DcpPresent::LifecycleLog::Action::None, "no transition, no line");

    PresentGate gate;
    const PresentGate::Inputs visible{{}, 1920, 1080, false};
    PresentGate::Inputs minimized = visible;
    minimized.visibility.minimized = true;
    int transitions = 0;
    for (int i = 0; i < 100; ++i) {
        transitions += gate.Update(minimized).transition ? 1 : 0;
    }
    Check(transitions == 1, "a window that stays minimized is one transition, not one per frame");
    const auto restore = gate.Update(visible);
    Check(restore.transition && restore.recreate && restore.present, "the restore is one transition with one recreation");
    Check(!gate.Update(visible).recreate && gate.Presented() && !gate.Presented(),
          "the restore asks for one recreation and ends with the first present");
}

void TestSurfaceLostAtPresent() {
    using DcpPresent::ClassifySurface;
    using DcpPresent::SurfaceState;
    Check(ClassifySurface(DcpPresent::kSurfaceLostResult, 0, 0, 0, 0) == SurfaceState::Lost,
          "a capabilities query that returns VK_ERROR_SURFACE_LOST_KHR is a lost surface, not an empty one");
    Check(ClassifySurface(0, 0, 0, 0, 0) == SurfaceState::NoArea && ClassifySurface(0, 1920, 1080, 1920, 1080) ==
                                                                        SurfaceState::HasArea,
          "a successful query is judged by its extent");
    Check(ClassifySurface(-1, 0, 0, 0, 0) == SurfaceState::HasArea,
          "another query error is left to the creation, which reports it");

    LifecycleModel m;
    m.Frames(10);
    m.Frame(SwapResult::Success, SwapResult::Success, true);
    Check(!m.fatal && m.presents == 10 && m.surfaces_replaced == 1 && m.recreations == 1,
          "VK_ERROR_SURFACE_LOST_KHR at present replaces the surface and recreates instead of failing");
    m.Frames(50);
    Check(m.presents == 60 && m.recreations == 1, "and presenting goes on on the new surface");
}

void TestSurfaceRecoveryBounded() {
    SurfaceRecovery recovery;
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        allowed += recovery.Lost() ? 1 : 0;
    }
    Check(allowed == int(SurfaceRecovery::kMaxInARow), "a surface lost again and again is replaced kMaxInARow times");
    recovery.Presented();
    Check(recovery.Lost() && recovery.Replaced() == SurfaceRecovery::kMaxInARow + 1,
          "a frame on the screen gives the next loss a fresh budget");

    LifecycleModel m;
    m.Frames(3);
    for (int i = 0; i < 20 && !m.fatal; ++i) {
        m.Frame(SwapResult::Success, SwapResult::Success, true);
    }
    Check(m.fatal && m.surfaces_replaced == int(SurfaceRecovery::kMaxInARow),
          "a surface lost on every present ends in a diagnosed failure, not a loop");

    LifecycleModel ok;
    for (int i = 0; i < 20; ++i) {
        ok.Frame(SwapResult::Success, SwapResult::Success, true);
        ok.Frames(2);
    }
    Check(!ok.fatal && ok.surfaces_replaced == 20, "occasional losses with frames in between are always recovered");
}

// A recreation an empty surface deferred is retried by the next frame, not only after an OUT_OF_DATE acquire.
void TestPresentHonoursPendingRecreation() {
    LifecycleModel m;
    m.Frames(5);
    m.health.RequestRecreation();
    m.Frame();
    Check(m.recreations == 1 && m.presents == 6, "a pending recreation is taken before the next acquire");
}

void TestPresenterGateWiring() {
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::string present = Body(presenter, "void Presenter::Present(Frame* frame", "\n}\n");
    Check(InOrder(present, "if (!CanPresent()) {\n        free_frame();\n        return;\n    }", "swapchain.UpdateVsync();") &&
              InOrder(present, "swapchain.UpdateVsync();", "acquired = swapchain.AcquireNextImage();"),
          "Present asks the gate before it recreates or acquires, and frees the frame when it may not present");
    Check(InOrder(present, "if (!CanPresent()) {", "std::scoped_lock submit_lock{Scheduler::submit_mutex};"),
          "no present under the submit lock on a gated window");
    Check(Contains(present, "window.GetHeight() != swapchain.GetHeight() ||\n        swapchain.NeedsRecreation()) {\n"
                            "        if (!RecreateSwapchain()) {"),
          "Present takes a pending recreation before it acquires");
    Check(InOrder(present, "if (presented) {", "if (gate.Presented()) {"), "the first present after a restore ends it");
    const std::string can = Body(presenter, "bool Presenter::CanPresent() {", "\n}\n");
    Check(Contains(can, "gate.Update({GateVisibility(),") &&
              Contains(can, "if (step.recreate) {\n        swapchain.RequestRecreation();") &&
              Contains(can, "const bool check_surface = gate.Suspended() || swapchain.NeedsRecreation();"),
          "the gate sees the visibility and the window size; the surface only while suspended or pending");
    Check(Contains(presenter, "return DcpPresent::GamescopeSession() ? DcpPresent::WindowVisibility{} : window.Visibility();"),
          "Gamescope never pauses for SDL visibility");
    const std::string recreate = Body(presenter, "bool Presenter::RecreateSwapchain() {", "\n}\n");
    Check(InOrder(recreate, "if (!swapchain.Recreate(window.GetWidth(), window.GetHeight())) {", "gate.SurfaceUnavailable()"),
          "a recreation without surface area suspends presenting");
    const std::string loading = Body(presenter, "void Presenter::PresentLoadingFrame() {", "\n}\n");
    Check(InOrder(loading, "if (!GateVisibility().Visible() ||", "swapchain.AcquireNextImage()"),
          "the loading frame is not presented to a minimized or hidden window");
    Check(Contains(presenter, "switch (lifecycle_log.Observe(true)) {"), "lifecycle lines are bounded");
}

void TestSurfaceLostWiring() {
    const std::string swapchain = Source("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    const std::string acquire = Body(swapchain, "bool Swapchain::AcquireNextImage() {", "\n}\n");
    const std::string present = Body(swapchain, "bool Swapchain::Present() {", "\n}\n");
    Check(Contains(acquire, "surface_lost |= result == vk::Result::eErrorSurfaceLostKHR;") &&
              Contains(present, "surface_lost |= result == vk::Result::eErrorSurfaceLostKHR;"),
          "a lost surface met by an acquire or a present is remembered");
    Check(InOrder(present, "if (step.usable) {", "surface_recovery.Presented();"), "a present refills the recovery budget");
    const std::string create = Body(swapchain, "bool Swapchain::Create(u32 width_, u32 height_) {", "\n}\n");
    Check(InOrder(create, "if (surface_lost || state == DcpPresent::SurfaceState::Lost) {\n        ReplaceSurface();",
                  "Destroy();"),
          "the next creation replaces a lost surface before anything else");
    const std::string replace = Body(swapchain, "void Swapchain::ReplaceSurface() {", "\n}\n");
    Check(InOrder(replace, "if (!surface_recovery.Lost()) {", "UNREACHABLE_MSG(") &&
              InOrder(replace, "UNREACHABLE_MSG(", "Destroy();") &&
              InOrder(replace, "Destroy();", "destroySurfaceKHR(surface);") &&
              InOrder(replace, "destroySurfaceKHR(surface);", "surface = CreateSurface(instance.GetInstance(), window);") &&
              InOrder(replace, "surface = CreateSurface(instance.GetInstance(), window);", "getSurfaceSupportKHR("),
          "the old swapchain, then the surface, go before a new surface is made and checked for the present queue");
    Check(InOrder(create, "Common::DeviceLost::Operation::CreateSwapchain", ".createSwapchainKHR(swapchain_info)") &&
              InOrder(create, "if (swapchain_result == vk::Result::eErrorSurfaceLostKHR) {", "surface_lost = true;") &&
              InOrder(create, "\"Failed to create swapchain: {} ({}x{}, {} images", "vk::to_string(composite_alpha)"),
          "swapchain creation is under the device-lost monitor, a lost surface retries, any other failure says what was asked");
    const std::string destroy = Body(swapchain, "void Swapchain::Destroy() {", "\n}\n");
    Check(InOrder(destroy, "device.destroySwapchainKHR(swapchain);", "swapchain = nullptr;"),
          "a failed creation never destroys the old swapchain twice");
}

void TestWindowVisibilityWiring() {
    const std::string window = Source("gpu/shim/window.cpp");
    Check(Contains(window, "case SDL_EVENT_WINDOW_MINIMIZED:\n            ApplyWindowEvent(DcpPresent::WindowEvent::Minimized);") &&
              Contains(window, "case SDL_EVENT_WINDOW_HIDDEN:\n            ApplyWindowEvent(DcpPresent::WindowEvent::Hidden);") &&
              Contains(window, "case SDL_EVENT_WINDOW_RESTORED:\n            ApplyWindowEvent(DcpPresent::WindowEvent::Restored);") &&
              Contains(window, "case SDL_EVENT_WINDOW_SHOWN:\n            ApplyWindowEvent(DcpPresent::WindowEvent::Shown);"),
          "each SDL visibility event reaches the presenter");
    Check(Contains(window, "DcpPresent::g_progress.window_active = visibility.Visible();") &&
              Contains(Source("gpu/shim/dcp_startup_watch.cpp"), "progress.window_active.load()"),
          "the stall watch reports nothing while the window is minimized or hidden");
    Check(Contains(Source("gpu/shim/sdl_window.h"), "DcpPresent::WindowVisibility Visibility() const {"),
          "the presenter reads the visibility with one atomic load");
}

// SOS-BXG9UT (KDE Wayland, Gamescope off): the window was reported 1755x987 (the Windowed size setting) while the
// swapchain was 2194x1234. A Wayland surface has no currentExtent: the swapchain takes the window's pixel size, clamped
// to the surface limits, never the logical size; a surface with a fixed size keeps it.
void TestWaylandExtentFollowsPixels() {
    constexpr std::uint32_t undefined = UINT32_MAX;
    // 125 % fractional scaling: logical 1755x987, pixels 2194x1234 (what SDL_GetWindowSizeInPixels gives).
    const auto wayland = DcpPresent::SwapchainExtent(undefined, undefined, 1, 1, 16384, 16384, 2194, 1234);
    Check(wayland.width == 2194 && wayland.height == 1234, "Wayland: the swapchain takes the window's pixel size");
    const auto clamped = DcpPresent::SwapchainExtent(undefined, undefined, 64, 64, 2048, 1152, 2194, 1234);
    Check(clamped.width == 2048 && clamped.height == 1152, "Wayland: the pixel size is clamped to the surface maximum");
    const auto tiny = DcpPresent::SwapchainExtent(undefined, undefined, 64, 64, 16384, 16384, 1, 1);
    Check(tiny.width == 64 && tiny.height == 64, "Wayland: the pixel size is raised to the surface minimum");
    const auto fixed = DcpPresent::SwapchainExtent(2194, 1234, 1, 1, 16384, 16384, 1755, 987);
    Check(fixed.width == 2194 && fixed.height == 1234, "X11/Win32/Gamescope: the surface's own extent wins");

    const std::string swapchain = Source("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    Check(Contains(swapchain, "const DcpPresent::Extent chosen = DcpPresent::SwapchainExtent(") &&
              Contains(swapchain, "extent = vk::Extent2D{chosen.width, chosen.height};"),
          "the swapchain uses SwapchainExtent");
    const std::string window = Source("gpu/shim/window.cpp");
    const size_t resize = window.find("case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:\n        case SDL_EVENT_WINDOW_RESIZED: {");
    const size_t pixels = window.find("SDL_GetWindowSizeInPixels(window, &w, &h);\n            width = w;", resize);
    Check(resize != std::string::npos && pixels != std::string::npos && pixels - resize < 200,
          "a resize or scale change stores the window's pixel size, which the swapchain follows");
    Check(Contains(window, "LogSurfaceGeometry(\"created\");") && Contains(window, "LogSurfaceGeometry(\"changed\");") &&
              Contains(window, "\"Window: surface %s, pixels %dx%d, logical %dx%d, density %.2f, display scale %.2f, "),
          "the log shows pixel and logical size, density and scale at creation and on change");
}

} // namespace

int main() {
    TestWaylandExtentFollowsPixels();
    TestSurfaceWithoutArea();
    TestResults();
    TestPersistentSuboptimal();
    TestRequiredOverrides();
    TestAcquiredImageLifetime();
    TestSourceWiring();
    TestAlternatingSuboptimalIsBounded();
    TestWindowVisibilityEvents();
    TestFocusAloneKeepsPresenting();
    TestMinimizeRestore();
    TestMaximizedOnlyRestore();
    TestHiddenShown();
    TestSurfaceZeroBeforeMinimizeEvent();
    TestZeroWindowSizeThenValid();
    TestRestoreEventsBeforeValidSize();
    TestRestoreClusterRecreatesOnce();
    TestMinimizeWithPendingRecreation();
    TestRepeatedMinimizeRestoreNoStorm();
    TestGamescopeIgnoresVisibility();
    TestLifecycleLogBounded();
    TestSurfaceLostAtPresent();
    TestSurfaceRecoveryBounded();
    TestPresentHonoursPendingRecreation();
    TestPresenterGateWiring();
    TestSurfaceLostWiring();
    TestWindowVisibilityWiring();
    std::printf("presentation swapchain: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
