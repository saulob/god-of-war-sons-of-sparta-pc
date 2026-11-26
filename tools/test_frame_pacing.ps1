# Headless proof of frame pacing in simulated time on the production limiter arithmetic and Frame rate policy: absolute
# slots, no drift over 10 minutes, a stall reanchors without a burst, every rate against every display refresh, original
# 30 and 60, the guest-driven high rate and its wiring, the Gamescope display-paced rule, the swap queue (a redraw is
# never flip backlog and never runs ahead of a flip), display refresh changes at runtime (debounced, retimed without a
# burst or drift, the guest clock unaffected, the reported refresh fixed), plus mutated copies the same test must reject.
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
$present = 'gpu/shim/dcp_present.h'
$mutations = @(
    @{ Name = 'deadline from the actual wake: late wakes accumulate drift'; File = $balance; Old = 'balance -= elapsed;'; New = 'balance = std::chrono::nanoseconds::zero();' },
    @{ Name = 'a late frame is repaid as a burst of catch-up frames'; File = $balance; Old = 'if (balance < -kMaxLateIntervals * interval) {'; New = 'if (false) {' },
    @{ Name = 'a known display refresh is ignored'; File = $policy; Old = 'return display_paced && display_hz > 0.0;'; New = 'return false;' },
    @{ Name = 'Gamescope does not pace the guest vblank'; File = $policy; Old = 'return vsync || compositor_paced;'; New = 'return vsync;' },
    @{ Name = 'the driver ignores Gamescope for the guest vblank'; File = $driver; Old = 'DcpFramePacing::DisplayPacesPresents(settings.vsync.load(), DcpPresent::GamescopeSession());'; New = 'settings.vsync.load();' },
    @{ Name = 'the flip gate counts a queued redraw as backlog'; File = $driver; Old = 'if (!swap_queue.FlipQueued()) {'; New = 'if (swap_queue.Empty()) {' },
    @{ Name = 'a redraw is queued ahead of a real flip'; File = $present; Old = "        if (queued_flips == 0) {`n            entries.clear();"; New = "        if (false) {`n            entries.clear();" },
    @{ Name = 'a redraw is queued while a flip presents'; File = $present; Old = 'if (busy || !entries.empty()) {'; New = 'if (!entries.empty()) {' },
    @{ Name = 'a taken flip stays counted as backlog'; File = $present; Old = 'queued_flips -= entry.flip ? 1 : 0;'; New = '' },
    @{ Name = 'a flip is queued as a redraw'; File = $driver; Old = "        if (!redraw) {`n            swap_queue.PushFlip(std::move(work));`n        } else if"; New = "        if (!redraw && !swap_queue.PushRedraw(std::move(work))) {`n        } else if" },
    @{ Name = 'the 90 FPS cadence is changed'; File = $policy; Old = 'std::clamp(hz == 0 ? kUnlimitedVblankHz : hz, kGameHz, kUnlimitedVblankHz)'; New = 'std::clamp(hz == 0 ? kUnlimitedVblankHz : (hz == 90 ? 89 : hz), kGameHz, kUnlimitedVblankHz)' },
    @{ Name = '30 FPS halves a game that already flips every second vblank'; File = $policy; Old = 'return RenderFps(setting) != 30 || flip_rate > 0 || flip_index % 2 == 0;'; New = 'return RenderFps(setting) != 30 || flip_index % 2 == 0;' },
    @{ Name = '60 FPS drops flips'; File = $policy; Old = 'return RenderFps(setting) != 30 ||'; New = 'return RenderFps(setting) == 60 ? (flip_index % 2 == 0) : RenderFps(setting) != 30 ||' },
    @{ Name = 'a redraw can win over a real flip'; File = $driver; Old = 'const auto request = receive_request();'; New = 'const auto request = Request{};' },
    @{ Name = 'the Frame rate changes at runtime without resetting the limiter'; File = $menu; Old = 'Apply::Restart, kFrameRates'; New = 'Apply::Live, kFrameRates' },
    @{ Name = 'the limiter reads a wall clock'; File = $thread; Old = 'const auto begin_sleep = std::chrono::steady_clock::now();'; New = 'const auto begin_sleep = std::chrono::system_clock::now();' },
    @{ Name = 'a timer handle is created per sleep'; File = $thread; Old = 'HANDLE timer = thread_timer.Get();'; New = 'HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);' },
    @{ Name = 'a refresh change is applied without the debounce'; File = $policy; Old = 'if (now_ms - since_ms_ < kRefreshSettleMs) {'; New = 'if (false) {' },
    @{ Name = 'a refresh that goes back before settling keeps its timer'; File = $policy; Old = "        if (nominal <= 0 || nominal == applied_) {`n            pending_ = 0;"; New = "        if (nominal <= 0) {`n            pending_ = 0;" },
    @{ Name = 'the new period is not reapplied by the limiter'; File = $balance; Old = "        interval = new_interval;`n"; New = "        (void)new_interval;`n" },
    @{ Name = 'the driver never retimes the present thread'; File = $driver; Old = 'timer.Retime(std::chrono::nanoseconds(1000000000 / to));'; New = '' },
    @{ Name = 'the present thread never reads the published refresh'; File = $driver; Old = 'if (const u64 live = DcpFramePacing::g_live_vblank.target.load(std::memory_order_relaxed);'; New = 'if (const u64 live = applied_refresh;' },
    @{ Name = 'lateness at a refresh change is repaid as a catch-up burst'; File = $balance; Old = "        if (balance < std::chrono::nanoseconds::zero()) {`n            balance = std::chrono::nanoseconds::zero();`n        } else if"; New = "        if (false) {`n        } else if" },
    @{ Name = 'a refresh change restarts the vblank with a full wait'; File = $balance; Old = "        } else if (balance > new_interval) {`n            balance = new_interval;"; New = "        } else if (balance > new_interval || true) {`n            balance = new_interval * 3;" },
    @{ Name = 'the retime ignores an equal rate'; File = $policy; Old = 'return live != 0 && to >= static_cast<unsigned>(kGameHz) && to != current_hz ? to : 0u;'; New = 'return live != 0 ? to : 0u;' },
    @{ Name = 'refresh change lines are unbounded'; File = $policy; Old = "    if (lines > kRefreshLogLines) {`n        return false;"; New = "    if (false) {`n        return false;" },
    @{ Name = 'the reported refresh rate flips with the vblank'; File = $driver; Old = "        EmulatorSettings.SetVblankFrequency(to);`n    }"; New = "        EmulatorSettings.SetVblankFrequency(to);`n        (void)SCE_VIDEO_OUT_REFRESH_RATE_119_88HZ; // main_port.resolution.refresh_rate`n    }" },
    @{ Name = 'Gamescope does not let the vblank follow the panel'; File = $policy; Old = '        display_paced_ = ((policy >> 62) & 1u) != 0;'; New = '        display_paced_ = false;' },
    @{ Name = 'the display mode is never read again without an event'; File = 'gpu/shim/window.cpp'; Old = '    if (now - refresh_polled_at >= 2000) {'; New = '    if (false) {' }
)
$window = 'gpu/shim/window.cpp'
$code = Invoke-HostMutationTest -Name 'frame_pacing' -Root $root -Test 'tests/frame_pacing_tests.cpp' `
    -Copy @($balance, $policy, $driver, $menu, $thread, $present, $window) -Mutations $mutations
exit $code
