# Headless proof of the swapchain health state machine on the production header: SUCCESS, SUBOPTIMAL and OUT_OF_DATE stay
# distinct, a persistently suboptimal surface is kept, optional recreations are spaced, required recreations are never
# suppressed, the presenter's frame and loading paths consume an acquired image before recreating, a minimized or hidden
# window is never acquired from or presented to and its restore recreates once, a lost surface is recreated and replaced
# instead of crashing, plus mutated copies the same test must reject.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$swapchain = 'gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp'
$presenter = 'gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp'
$present = 'gpu/shim/dcp_present.h'
$window = 'gpu/shim/window.cpp'
$windowHeader = 'gpu/shim/sdl_window.h'
$watch = 'gpu/shim/dcp_startup_watch.cpp'
$mutations = @(
    @{ Name = 'SUBOPTIMAL acquire is collapsed into OUT_OF_DATE'; File = $present; Old = "RequestForSuboptimal();`n            return {true, recreate};"; New = "RequestRequired();`n            return {false, true};" },
    @{ Name = 'SUBOPTIMAL acquire marks the image unusable'; File = $present; Old = "RequestForSuboptimal();`n            return {true, recreate};"; New = "RequestForSuboptimal();`n            return {false, recreate};" },
    @{ Name = 'a persistent SUBOPTIMAL swapchain is recreated every frame'; File = $present; Old = 'if (!replaced_suboptimal && !recreate && presents_since_optional >= kOptionalSpacing) {'; New = 'if (!recreate && presents_since_optional >= kOptionalSpacing) {' },
    @{ Name = 'optional recreations are not spaced'; File = $present; Old = ' && presents_since_optional >= kOptionalSpacing) {'; New = ') {' },
    @{ Name = 'the spacing also holds back OUT_OF_DATE'; File = $present; Old = "void RequestRequired() {`n        recreate = true;"; New = "void RequestRequired() {`n        recreate = presents_since_optional >= kOptionalSpacing;" },
    @{ Name = 'the suppression never resets after a clean SUCCESS cycle'; File = $present; Old = "if (clean_acquire) {`n                replaced_suboptimal = false;"; New = "if (false) {`n                replaced_suboptimal = false;" },
    @{ Name = 'the suppression blocks OUT_OF_DATE'; File = $present; Old = "void RequestRequired() {`n        recreate = true;"; New = "void RequestRequired() {`n        recreate = !replaced_suboptimal;" },
    @{ Name = 'an acquired SUBOPTIMAL image is discarded by the acquire'; File = $swapchain; Old = 'return step.usable;'; New = 'return step.usable && !step.recreate;' },
    @{ Name = 'the swapchain is recreated before the acquired SUBOPTIMAL image is consumed'; File = $presenter; Old = "    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddWait(frame->ready_semaphore, frame->ready_tick);"; New = "    if (swapchain.NeedsRecreation()) {`n        swapchain.Recreate(window.GetWidth(), window.GetHeight());`n    }`n    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddWait(frame->ready_semaphore, frame->ready_tick);" },
    @{ Name = 'the loading frame recreates before its acquired SUBOPTIMAL image is consumed'; File = $presenter; Old = "    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddSignal(swapchain.GetPresentReadySemaphore());`n    present_scheduler.Flush(info);"; New = "    if (swapchain.NeedsRecreation()) {`n        swapchain.Recreate(window.GetWidth(), window.GetHeight());`n    }`n    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddSignal(swapchain.GetPresentReadySemaphore());`n    present_scheduler.Flush(info);" },
    @{ Name = 'the loading frame ignores a pending recreation'; File = $presenter; Old = "window.GetHeight() != swapchain.GetHeight() ||`n        swapchain.NeedsRecreation()) {`n        if (!swapchain.Recreate("; New = "window.GetHeight() != swapchain.GetHeight()) {`n        if (!swapchain.Recreate(" },
    @{ Name = 'Present ignores a pending recreation'; File = $presenter; Old = "window.GetHeight() != swapchain.GetHeight() ||`n        swapchain.NeedsRecreation()) {`n        if (!RecreateSwapchain()) {"; New = "window.GetHeight() != swapchain.GetHeight()) {`n        if (!RecreateSwapchain()) {" },
    @{ Name = 'device lost is classified as a swapchain recreation'; File = $swapchain; Old = "    case vk::Result::eErrorUnknown:`n        return DcpPresent::SwapResult::OutOfDate;"; New = "    case vk::Result::eErrorDeviceLost:`n    case vk::Result::eErrorUnknown:`n        return DcpPresent::SwapResult::OutOfDate;" },
    @{ Name = 'PresentResult drops VK_ERROR_SURFACE_LOST_KHR'; File = $swapchain; Old = "    case vk::Result::eErrorSurfaceLostKHR:`n    case vk::Result::eErrorOutOfDateKHR:`n        return DcpPresent::SwapResult::OutOfDate;"; New = "    case vk::Result::eErrorOutOfDateKHR:`n        return DcpPresent::SwapResult::OutOfDate;" },
    @{ Name = 'a lost surface is reused for the recreation'; File = $swapchain; Old = "if (surface_lost || state == DcpPresent::SurfaceState::Lost) {"; New = "if (false) {" },
    @{ Name = 'a lost surface at present is forgotten'; File = $swapchain; Old = "    ReportResult(true, result);`n    surface_lost |= result == vk::Result::eErrorSurfaceLostKHR;"; New = "    ReportResult(true, result);" },
    @{ Name = 'a lost surface is replaced forever'; File = $present; Old = 'if (in_a_row >= kMaxInARow) {'; New = 'if (false) {' },
    @{ Name = 'a present never refills the surface budget'; File = $present; Old = "    void Presented() {`n        in_a_row = 0;"; New = "    void Presented() {`n        in_a_row = in_a_row;" },
    @{ Name = 'a lost surface counts as an empty one'; File = $present; Old = "        return SurfaceState::Lost;"; New = "        return SurfaceState::NoArea;" },
    @{ Name = 'swapchain creation bypasses the device-lost monitor'; File = $swapchain; Old = 'Common::DeviceLost::Operation::CreateSwapchain'; New = 'Common::DeviceLost::Operation::Checked' },
    @{ Name = 'a minimized surface gets a 0x0 swapchain'; File = $swapchain; Old = 'if (state != DcpPresent::SurfaceState::HasArea) {'; New = 'if (false) {' },
    @{ Name = 'a recreation held back by an empty surface is forgotten'; File = $swapchain; Old = "        surface_lost = state == DcpPresent::SurfaceState::Lost;`n        health.RequestRecreation();`n        return false;"; New = "        surface_lost = state == DcpPresent::SurfaceState::Lost;`n        return false;" },
    @{ Name = 'an empty surface counts as usable'; File = $present; Old = '(current_width == UINT32_MAX || (current_width > 0 && current_height > 0))'; New = 'true' },
    @{ Name = 'a frame that cannot be presented is never freed'; File = $presenter; Old = "        if (!RecreateSwapchain()) {`n            free_frame();`n            return;"; New = "        if (!RecreateSwapchain()) {`n            return;" },
    @{ Name = 'a VSync change is held back by the suppression'; File = $swapchain; Old = "FindPresentMode();`n    Recreate(width, height);"; New = "FindPresentMode();`n    if (!health.NeedsRecreation()) {`n        Recreate(width, height);`n    }" },
    @{ Name = 'the present gate is removed'; File = $presenter; Old = "    if (!CanPresent()) {`n        free_frame();`n        return;`n    }"; New = '' },
    @{ Name = 'a minimized window is presented to'; File = $present; Old = "        if (in.visibility.minimized) {`n            return Lifecycle::Minimized;`n        }"; New = '' },
    @{ Name = 'a hidden window is presented to'; File = $present; Old = "        if (in.visibility.hidden) {`n            return Lifecycle::Hidden;`n        }"; New = '' },
    @{ Name = 'the restore recreates nothing'; File = $present; Old = 'return {true, true, true, current};'; New = 'return {true, false, true, current};' },
    @{ Name = 'every frame of a minimized window is a transition'; File = $present; Old = 'const bool changed = !suspended || current != reason;'; New = 'const bool changed = true;' },
    @{ Name = 'lifecycle lines are unbounded'; File = $present; Old = 'if (!transition || lines > kMaxLines) {'; New = 'if (!transition) {' },
    @{ Name = 'Gamescope pauses for SDL visibility'; File = $presenter; Old = 'return DcpPresent::GamescopeSession() ? DcpPresent::WindowVisibility{} : window.Visibility();'; New = 'return window.Visibility();' },
    @{ Name = 'the loading frame presents to a hidden window'; File = $presenter; Old = 'if (!GateVisibility().Visible() || window.GetWidth() == 0'; New = 'if (window.GetWidth() == 0' },
    @{ Name = 'window_active ignores a hidden window'; File = $window; Old = 'DcpPresent::g_progress.window_active = visibility.Visible();'; New = 'DcpPresent::g_progress.window_active = !visibility.minimized;' },
    @{ Name = 'SHOWN is not passed to the presenter'; File = $window; Old = 'ApplyWindowEvent(DcpPresent::WindowEvent::Shown);'; New = 'ApplyWindowEvent(DcpPresent::WindowEvent::Hidden);' },
    @{ Name = 'MAXIMIZED does not end a minimize'; File = $present; Old = "        case WindowEvent::Restored:`n        case WindowEvent::Maximized:`n            minimized = false;"; New = "        case WindowEvent::Restored:`n            minimized = false;`n            break;`n        case WindowEvent::Maximized:" },
    @{ Name = 'the window drops MAXIMIZED'; File = $window; Old = "        case SDL_EVENT_WINDOW_MAXIMIZED:`n            ApplyWindowEvent(DcpPresent::WindowEvent::Maximized);`n            break;`n"; New = '' },
    @{ Name = 'the window flags never correct the event stream'; File = $window; Old = 'visibility.Sync((flags & SDL_WINDOW_MINIMIZED) != 0, (flags & SDL_WINDOW_HIDDEN) != 0);'; New = '(void)flags;' },
    @{ Name = 'Sync keeps a stale minimize'; File = $present; Old = "        minimized = minimized_now;`n        hidden = hidden_now;"; New = "        minimized = minimized || minimized_now;`n        hidden = hidden_now;" },
    @{ Name = 'a Wayland swapchain ignores the surface limits'; File = $present; Old = 'return {std::max(min_width, std::min(max_width, pixel_width)), std::max(min_height, std::min(max_height, pixel_height))};'; New = 'return {pixel_width, pixel_height};' },
    @{ Name = 'a fixed surface extent is overridden by the window size'; File = $present; Old = "    if (current_width != UINT32_MAX) {`n        return {current_width, current_height};`n    }"; New = '' },
    @{ Name = 'a resize stores the logical size'; File = $window; Old = "            SDL_GetWindowSizeInPixels(window, &w, &h);`n            width = w;`n            height = h;`n            LogGamescopeWindow();"; New = "            SDL_GetWindowSize(window, &w, &h);`n            width = w;`n            height = h;`n            LogGamescopeWindow();" }
)
$code = Invoke-HostMutationTest -Name 'presentation_swapchain' -Root $root -Test 'tests/presentation_swapchain_tests.cpp' `
    -Copy @($present, $swapchain, $presenter, $window, $windowHeader, $watch) -Mutations $mutations
exit $code
