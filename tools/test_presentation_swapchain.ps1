# Headless proof of the swapchain health state machine on the production header: SUCCESS, SUBOPTIMAL and OUT_OF_DATE stay
# distinct, a persistently suboptimal surface is kept, required recreations are never suppressed, the presenter's frame and
# loading paths consume an acquired image before recreating, plus mutated copies the same test must reject.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$swapchain = 'gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp'
$presenter = 'gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp'
$present = 'gpu/shim/dcp_present.h'
$mutations = @(
    @{ Name = 'SUBOPTIMAL acquire is collapsed into OUT_OF_DATE'; File = $present; Old = "RequestForSuboptimal();`n            return {true, recreate};"; New = "RequestRequired();`n            return {false, true};" },
    @{ Name = 'SUBOPTIMAL acquire marks the image unusable'; File = $present; Old = "RequestForSuboptimal();`n            return {true, recreate};"; New = "RequestForSuboptimal();`n            return {false, recreate};" },
    @{ Name = 'a persistent SUBOPTIMAL swapchain is recreated every frame'; File = $present; Old = 'if (!replaced_suboptimal && !recreate) {'; New = 'if (!recreate) {' },
    @{ Name = 'the suppression never resets after a clean SUCCESS cycle'; File = $present; Old = "if (clean_acquire) {`n                replaced_suboptimal = false;"; New = "if (false) {`n                replaced_suboptimal = false;" },
    @{ Name = 'the suppression blocks OUT_OF_DATE'; File = $present; Old = "void RequestRequired() {`n        recreate = true;"; New = "void RequestRequired() {`n        recreate = !replaced_suboptimal;" },
    @{ Name = 'an acquired SUBOPTIMAL image is discarded by the acquire'; File = $swapchain; Old = 'return step.usable;'; New = 'return step.usable && !step.recreate;' },
    @{ Name = 'the swapchain is recreated before the acquired SUBOPTIMAL image is consumed'; File = $presenter; Old = "    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddWait(frame->ready_semaphore, frame->ready_tick);"; New = "    if (swapchain.NeedsRecreation()) {`n        swapchain.Recreate(window.GetWidth(), window.GetHeight());`n    }`n    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddWait(frame->ready_semaphore, frame->ready_tick);" },
    @{ Name = 'the loading frame recreates before its acquired SUBOPTIMAL image is consumed'; File = $presenter; Old = "    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddSignal(swapchain.GetPresentReadySemaphore());`n    present_scheduler.Flush(info);"; New = "    if (swapchain.NeedsRecreation()) {`n        swapchain.Recreate(window.GetWidth(), window.GetHeight());`n    }`n    SubmitInfo info{};`n    info.AddWait(swapchain.GetImageAcquiredSemaphore());`n    info.AddSignal(swapchain.GetPresentReadySemaphore());`n    present_scheduler.Flush(info);" },
    @{ Name = 'the loading frame ignores a pending recreation'; File = $presenter; Old = "window.GetHeight() != swapchain.GetHeight() ||`n        swapchain.NeedsRecreation()) {"; New = 'window.GetHeight() != swapchain.GetHeight()) {' },
    @{ Name = 'device lost is classified as a swapchain recreation'; File = $swapchain; Old = "    case vk::Result::eErrorUnknown:`n        return DcpPresent::SwapResult::OutOfDate;"; New = "    case vk::Result::eErrorDeviceLost:`n    case vk::Result::eErrorUnknown:`n        return DcpPresent::SwapResult::OutOfDate;" },
    @{ Name = 'a VSync change is held back by the suppression'; File = $swapchain; Old = "FindPresentMode();`n    Recreate(width, height);"; New = "FindPresentMode();`n    if (!health.NeedsRecreation()) {`n        Recreate(width, height);`n    }" }
)
$code = Invoke-HostMutationTest -Name 'presentation_swapchain' -Root $root -Test 'tests/presentation_swapchain_tests.cpp' `
    -Copy @($present, $swapchain, $presenter) -Mutations $mutations
exit $code
