# Headless proof of the launcher's renderer policy, 60 Hz deadline pacing, minimized wake policy and telemetry on the
# production headers in simulated time, the host and loop wiring, and the Direct3D 11 teardown gate, plus the host and
# launcher sources compiled (syntax only) and mutated copies the same test must reject. No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$loop = 'launcher/launcher_frame_loop.h'
$renderer = 'launcher/launcher_renderer.h'
$host_cpp = 'launcher/host.cpp'
$launcher = 'launcher/launcher.cpp'
$mutations = @(
    @{ Name = 'the 60 Hz cap is removed'; File = $loop; Old = 'if (now_ns < deadline_) {'; New = 'if (false) {' },
    @{ Name = 'the target rate is raised to 144 Hz'; File = $loop; Old = 'inline constexpr int kLauncherTargetHz = 60;'; New = 'inline constexpr int kLauncherTargetHz = 144;' },
    @{ Name = 'relative sleeps reintroduce drift'; File = $loop; Old = 'deadline_ = late > interval_ / 2 ? now_ns + interval_ : deadline_ + interval_;'; New = 'deadline_ = now_ns + interval_;' },
    @{ Name = 'a stall is repaid with a catch-up burst'; File = $loop; Old = 'deadline_ = late > interval_ / 2 ? now_ns + interval_ : deadline_ + interval_;'; New = 'deadline_ += interval_;' },
    @{ Name = 'a minimized launcher keeps rendering'; File = $loop; Old = 'if (minimized) {
            pacer_.Reset();'; New = 'if (false) {
            pacer_.Reset();' },
    @{ Name = 'the minimized low wake rate is a 1 ms poll'; File = $loop; Old = 'inline constexpr int64_t kMinimizedWakeNs = 100 * kNsPerMs;'; New = 'inline constexpr int64_t kMinimizedWakeNs = 1 * kNsPerMs;' },
    @{ Name = 'the software fallback is removed'; File = $renderer; Old = 'return {"direct3d11", "software"};'; New = 'return {"direct3d11"};' },
    @{ Name = 'Windows reverts to software as the primary renderer'; File = $renderer; Old = '    return {"direct3d11", "software"};
}

// System modules'; New = '    return {"software"};
}

// System modules' },
    @{ Name = 'Direct3D 11 no longer pins its modules'; File = $renderer; Old = 'return {"d3d11.dll", "dxgi.dll"};'; New = 'return {};' },
    @{ Name = 'an unknown override selects Direct3D 11'; File = $renderer; Old = '        return RendererRequest::kDirect3D11;
    }
    return RendererRequest::kAuto;'; New = '        return RendererRequest::kDirect3D11;
    }
    return RendererRequest::kDirect3D11;' },
    @{ Name = 'Direct3D 11 is created without pinning its modules'; File = $renderer; Old = 'if (!pin(module)) {'; New = 'if (false && !pin(module)) {' },
    @{ Name = 'a failed pin no longer skips the renderer'; File = $renderer; Old = 'if (!pinned) {
            continue;'; New = 'if (false) {
            continue;' },
    @{ Name = 'the renderer selection is no longer logged'; File = $host_cpp; Old = 'std::printf("%s\n",
                RendererSelectedLine('; New = '(void)(
                RendererSelectedLine(' },
    @{ Name = 'the launcher unloads a Direct3D module itself'; File = $host_cpp; Old = 'sos_startup_phase("launcher destroying renderer");'; New = 'FreeLibrary(GetModuleHandleW(L"d3d11.dll"));
    sos_startup_phase("launcher destroying renderer");' },
    @{ Name = 'the launcher window is destroyed before its renderer'; File = $host_cpp; Old = 'sos_startup_phase("launcher destroying renderer");'; New = 'if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    sos_startup_phase("launcher destroying renderer");' },
    @{ Name = 'sustained high render rates are no longer detected'; File = $loop; Old = 'fast_windows_ = sample.hz_x10 > kExcessiveHzX10 ? fast_windows_ + 1 : 0;'; New = 'fast_windows_ = 0;' },
    @{ Name = 'present stalls are no longer detected'; File = $loop; Old = 'if (times.present_ns < kStallPresentNs) {'; New = 'if (true) {' },
    @{ Name = 'present stall reports are not rate limited'; File = $loop; Old = 'if (!stalls_.Allow(now_ns)) {'; New = 'if (false) {' },
    @{ Name = 'the UI thread CPU share is miscalculated'; File = $loop; Old = '(end_cpu_ns - start_cpu_ns) * 1000 / elapsed_ns'; New = '(end_cpu_ns - start_cpu_ns) * 100 / elapsed_ns' },
    @{ Name = 'render Hz is divided by the minimized time too'; File = $loop; Old = 'frames_ * 10 * kNsPerSecond / visible'; New = 'frames_ * 10 * kNsPerSecond / elapsed' },
    @{ Name = 'minimized time is not accounted'; File = $loop; Old = '            minimized_ns_ += std::max<int64_t>(now_ns - minimized_since_, 0);
        }
        minimized_ = minimized;'; New = '        }
        minimized_ = minimized;' },
    @{ Name = 'the explain dialog loop bypasses the paced wait'; File = $launcher; Old = 'while (dialog.open() && host.WaitForFrame()) {'; New = 'while (dialog.open()) {' },
    @{ Name = 'frame timings are no longer reported'; File = $host_cpp; Old = 'NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});'; New = '(void)presented;' },
    @{ Name = 'the UI thread CPU is read from the whole process'; File = $host_cpp; Old = 'GetThreadTimes(GetCurrentThread(),'; New = 'GetProcessTimes(GetCurrentProcess(),' },
    @{ Name = 'the clock is no longer SDL''s monotonic counter'; File = $host_cpp; Old = 'return static_cast<int64_t>(SDL_GetTicksNS());'; New = 'return static_cast<int64_t>(SDL_GetTicks()) * 1000000;' },
    @{ Name = 'the frame wait is a millisecond sleep'; File = $host_cpp; Old = 'SDL_DelayNS(static_cast<Uint64>(ns));'; New = 'SDL_Delay(1);' },
    @{ Name = 'the minimized wait polls instead of blocking'; File = $host_cpp; Old = 'if (SDL_WaitEventTimeout(&event, static_cast<Sint32>(std::max<int64_t>(timeout_ns / kNsPerMs, 1)))) {'; New = 'if (SDL_PollEvent(&event)) {' }
)

$code = Invoke-HostMutationTest -Name 'launcher_renderer' -Root $root -Test 'tests/launcher_renderer_tests.cpp' `
    -Copy @($loop, $renderer, $host_cpp, 'launcher/host.h', $launcher, 'gpu/shim/window.cpp') -IncludeDirs @('launcher') -Mutations $mutations
if ($code -ne 0) { exit $code }

# The production host and launcher sources must still compile against SDL3, Dear ImGui and TRG.
. (Join-Path $PSScriptRoot '_common.ps1')
$tc = Resolve-Toolchain 'auto'
$prefix = Split-Path $tc.Bin -Parent
$clang = Join-Path $tc.Bin 'clang++.exe'
$include = @('-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'gpu\shim'),
             '-I', (Join-Path $root 'third_party\trg-launcher\include'), '-I', (Join-Path $root 'gpu\third_party\imgui'),
             '-I', (Join-Path $prefix 'include'))
foreach ($name in 'host', 'launcher') {
    & $clang '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fsyntax-only' @include (Get-StringDefine 'SOS_PORT_VERSION' '0.0.0') (Join-Path $root "launcher\$name.cpp")
    if ($LASTEXITCODE -ne 0) { Write-Host "  [fail] launcher\$name.cpp does not compile"; exit 1 }
    Write-Host "launcher\$name.cpp compiles with -Wall -Wextra -Werror"
}
exit 0
