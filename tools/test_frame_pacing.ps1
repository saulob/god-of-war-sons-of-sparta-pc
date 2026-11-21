# Headless proof of frame pacing in simulated time on the production limiter arithmetic and Frame rate policy: absolute
# slots, no drift over 10 minutes, a stall reanchors without a burst, every rate against every display refresh, original
# 30 and 60, the guest-driven high rate and its wiring, plus mutated copies the same test must reject.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$balance = 'gpu/shadps4/common/pacing_balance.h'
$policy = 'gpu/shim/dcp_frame_pacing.h'
$driver = 'gpu/shadps4/core/libraries/videoout/driver.cpp'
$menu = 'gpu/shim/dcp_options_menu.h'
$thread = 'gpu/shadps4/common/thread.cpp'
$mutations = @(
    @{ Name = 'deadline from the actual wake: late wakes accumulate drift'; File = $balance; Old = 'balance -= elapsed;'; New = 'balance = std::chrono::nanoseconds::zero();' },
    @{ Name = 'a late frame is repaid as a burst of catch-up frames'; File = $balance; Old = 'if (balance < -kMaxLateIntervals * interval) {'; New = 'if (false) {' },
    @{ Name = 'a known display refresh is ignored'; File = $policy; Old = 'return vsync && display_hz > 0.0;'; New = 'return false;' },
    @{ Name = 'the 90 FPS cadence is changed'; File = $policy; Old = 'std::clamp(hz == 0 ? kUnlimitedVblankHz : hz, kGameHz, kUnlimitedVblankHz)'; New = 'std::clamp(hz == 0 ? kUnlimitedVblankHz : (hz == 90 ? 89 : hz), kGameHz, kUnlimitedVblankHz)' },
    @{ Name = '30 FPS halves a game that already flips every second vblank'; File = $policy; Old = 'return RenderFps(setting) != 30 || flip_rate > 0 || flip_index % 2 == 0;'; New = 'return RenderFps(setting) != 30 || flip_index % 2 == 0;' },
    @{ Name = '60 FPS drops flips'; File = $policy; Old = 'return RenderFps(setting) != 30 ||'; New = 'return RenderFps(setting) == 60 ? (flip_index % 2 == 0) : RenderFps(setting) != 30 ||' },
    @{ Name = 'a redraw can win over a real flip'; File = $driver; Old = 'const auto request = receive_request();'; New = 'const auto request = Request{};' },
    @{ Name = 'the Frame rate changes at runtime without resetting the limiter'; File = $menu; Old = 'Apply::Restart, kFrameRates'; New = 'Apply::Live, kFrameRates' },
    @{ Name = 'the limiter reads a wall clock'; File = $thread; Old = 'const auto begin_sleep = std::chrono::steady_clock::now();'; New = 'const auto begin_sleep = std::chrono::system_clock::now();' },
    @{ Name = 'a timer handle is created per sleep'; File = $thread; Old = 'HANDLE timer = thread_timer.Get();'; New = 'HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);' }
)
$code = Invoke-HostMutationTest -Name 'frame_pacing' -Root $root -Test 'tests/frame_pacing_tests.cpp' `
    -Copy @($balance, $policy, $driver, $menu, $thread) -Mutations $mutations
exit $code
