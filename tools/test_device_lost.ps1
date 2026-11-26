# Headless proof of the graceful Vulkan device-lost path: the production monitor, call and wait templates and timeline
# semaphore helpers against a fake driver, the call-site inventory of the renderer, and mutated copies of the monitor
# and the semaphore helpers that the same test must reject. No window, GPU, launcher or game is started and no device is
# lost.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_device_lost_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $gpu = Join-Path $root 'gpu'
    $rootText = $root.Replace('\', '/')
    $clang = Join-Path $tc.Bin 'clang++.exe'
    $include = @('-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'),
        '-I', (Join-Path $gpu 'third_party/half/include'), '-I', (Join-Path $gpu 'third_party/magic_enum/include'),
        '-I', (Join-Path $prefix 'include'))
    $flags = @('--target=x86_64-w64-mingw32', '-std=gnu++23', '-Wall', '-Wextra', '-O1', '-DNOMINMAX', '-Wno-deprecated-declarations',
        '-Wno-missing-field-initializers', '-include', (Join-Path $gpu 'shim/dcp_win_compat.h'), "-DSOS_SOURCE_DIR=$rootText", '-pthread')

    $monitorHeader = Join-Path $gpu 'shim/common/device_lost.h'
    $monitorSource = Join-Path $gpu 'shim/common/device_lost.cpp'
    $traceSource = Join-Path $gpu 'shim/common/gpu_trace.cpp' # the monitor's capture hook prints the GPU trace
    $semaphoreHeader = Join-Path $gpu 'shadps4/video_core/renderer_vulkan/vk_semaphore.h'
    $test = Join-Path $root 'tests/device_lost_tests.cpp'

    $exe = Join-Path $temp 'device_lost_tests.exe'
    & $clang @flags @include $test $monitorSource $traceSource '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the device lost test' }
    & $exe
    $result = $LASTEXITCODE

    # Each mutation breaks one rule of the owner or of the post-loss behaviour; the same test must fail on it.
    $mutations = @(
        @{ Name = 'a second fault replaces the first'; File = 'monitor'; Old = 'if (!phase.compare_exchange_strong(expected, Phase::Lost, std::memory_order_acq_rel)) {'; New = 'if (false) {' },
        @{ Name = 'a submission after the loss still reaches Vulkan'; File = 'header'; Old = "    if (monitor.IsLost()) {`n        monitor.Observe(OperationName(operation));`n        return DeviceLostResult;`n    }`n    const s32 result = monitor.Simulate(operation) ? DeviceLostResult : static_cast<s32>(call());"; New = "    const s32 result = monitor.Simulate(operation) ? DeviceLostResult : static_cast<s32>(call());" },
        @{ Name = 'a fence wait after the loss keeps waiting'; File = 'header'; Old = "        if (monitor.IsLost()) {`n            monitor.Observe(OperationName(operation));`n            return DeviceLostResult;`n        }`n        const bool simulated"; New = "        const bool simulated" },
        @{ Name = 'the fault is asked more than once'; File = 'monitor'; Old = 'if (fault_claimed.exchange(true, std::memory_order_acq_rel)) {'; New = 'if (false) {' },
        @{ Name = 'the shutdown is requested more than once'; File = 'monitor'; Old = 'if (shutdown_issued.exchange(true, std::memory_order_acq_rel)) {'; New = 'if (false) {' },
        @{ Name = 'consequence lines are not bounded'; File = 'monitor'; Old = 'if (seen > MaxSecondaryLines) {'; New = 'if (false) {' },
        @{ Name = 'another Vulkan error counts as a device loss'; File = 'header'; Old = 'if (result == DeviceLostResult) {'; New = 'if (result != Success) {' },
        @{ Name = 'a timeout ends a wait'; File = 'header'; Old = 'if (result != Timeout) {'; New = 'if (true) {' },
        @{ Name = 'the worker stop predicate ignores the loss'; File = 'header'; Old = 'return stop.stop_requested() || monitor.IsLost();'; New = 'return stop.stop_requested();' },
        @{ Name = 'a semaphore read after the loss reaches the driver'; File = 'semaphore'; Old = "CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue, `"semread`", known, ticks.timeline,`n                             [&] { return read_counter(counter); });"; New = "static_cast<s32>(read_counter(counter));" },
        @{ Name = 'a semaphore wait after the loss blocks'; File = 'semaphore'; Old = 'WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", tick, ticks.timeline, wait_once);'; New = 'static_cast<s32>(wait_once());' }
        ,
        @{ Name = 'all timelines share one submitted tick'; File = 'header'; Old = 'submitted_ticks[u32(timeline)].store(tick, std::memory_order_relaxed);'; New = 'submitted_ticks[0].store(tick, std::memory_order_relaxed);' },
        @{ Name = 'all timelines share one completed tick'; File = 'header'; Old = 'completed_ticks[u32(timeline)].store(tick, std::memory_order_relaxed);'; New = 'completed_ticks[0].store(tick, std::memory_order_relaxed);' },
        @{ Name = 'a present completion is recorded as draw'; File = 'semaphore'; Old = 'monitor.NoteCompleted(ticks.timeline, counter);'; New = 'monitor.NoteCompleted(Common::DeviceLost::Timeline::Draw, counter);' },
        @{ Name = 'a sparse bind wait is recorded as draw'; File = 'semaphore'; Old = '"semwait", tick, ticks.timeline, wait_once);'; New = '"semwait", tick, Common::DeviceLost::Timeline::Draw, wait_once);' },
        @{ Name = 'the last submitted tick of the root comes from the draw timeline'; File = 'monitor'; Old = 'root.timelines[u32(timeline)].submitted;'; New = 'root.timelines[u32(Timeline::Draw)].submitted;' },
        @{ Name = 'the root report omits the timeline of its tick'; File = 'monitor'; Old = 'text.Add(", %s tick %llu", TimelineName(root.timeline), (unsigned long long)root.tick);'; New = 'text.Add(", tick %llu", (unsigned long long)root.tick);' },
        @{ Name = 'the root cause forgets its timeline'; File = 'monitor'; Old = 'root.timeline = timeline;'; New = 'root.timeline = Timeline::Draw;' },
        @{ Name = 'the per-timeline state is not part of the report'; File = 'monitor'; Old = '    Print(timeline_line);'; New = '' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + $detected + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $header = Copy-ToWorkspace $work $monitorHeader 'common/device_lost.h'
        $source = Copy-ToWorkspace $work $monitorSource 'common/device_lost.cpp'
        $semaphore = Copy-ToWorkspace $work $semaphoreHeader 'video_core/renderer_vulkan/vk_semaphore.h'
        $target = @{ header = $header; monitor = $source; semaphore = $semaphore }[$mutation.File]
        Edit-WorkspaceText $target $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'device_lost_tests.exe'
        $build = { & $clang @flags '-w' '-I' $work @include $test $source $traceSource '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "device lost mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
