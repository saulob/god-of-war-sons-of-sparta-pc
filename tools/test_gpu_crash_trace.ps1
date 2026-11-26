# Headless proof of the GPU crash trace (gpu/shim/common/gpu_trace.{h,cpp}) on the production sources: the lock-free ring
# keeps the newest events in order and never prints a torn one under concurrent writers, recording allocates nothing, the
# dump is bounded, printed once and names the pipelines of the unfinished draw submissions and the newest dispatches, the
# production device-lost monitor (SOS_SIMULATE_DEVICE_LOST, exit 25) and a GPU assertion (exit 23) print it before the
# process ends, and the Bug Report extracts its summary from the real output. Mutated copies must fail.
# No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$trace = 'gpu/shim/common/gpu_trace.cpp'
$monitor = 'gpu/shim/common/device_lost.cpp'
$assert = 'gpu/shim/common/assert.cpp'
$report = 'launcher/bug_report.cpp'
$sources = @($trace, $monitor, $assert, 'gpu/shim/common/logging/log.cpp', $report, 'launcher/log_scrubber.cpp',
    'launcher/game_profile.cpp', 'launcher/localization/ui_text.cpp', 'src/net/json.cpp')
$copy = $sources + @('gpu/shim/common/gpu_trace.h', 'gpu/shim/common/device_lost.h', 'launcher/bug_report.h')
$dirs = @('gpu/shim', 'gpu/shadps4', 'launcher', 'src')

$mutations = @(
    @{ Name = 'the ring does not wrap (a full ring drops new events)'; File = $trace; Old = 'const u64 seq = next_seq.fetch_add(1, std::memory_order_relaxed);'; New = 'const u64 seq = next_seq.fetch_add(1, std::memory_order_relaxed); if (seq > Capacity) { return; }' },
    @{ Name = 'the device-lost monitor does not print the trace'; File = $monitor; Old = "    if (hooks.capture) {`n        hooks.capture(root);`n    }`n"; New = '' },
    @{ Name = 'the production monitor has no capture hook'; File = $monitor; Old = '    .capture = CaptureGpuTrace,'; New = '    .capture = nullptr,' },
    @{ Name = 'a GPU assertion does not print the trace'; File = $assert; Old = '    Common::GpuTrace::Dump("GPU assertion failed");'; New = '' },
    @{ Name = 'a GPU assertion ends without draining the session log (SOS-BXG9UT)'; File = $assert; Old = "    if (auto* flush = Common::DeviceLost::Global().ExitFlush()) {`n        flush();`n    }`n"; New = '' },
    @{ Name = 'the unfinished-tick filter takes the completed tick as unfinished'; File = $trace; Old = 'tick > completed && tick <= submitted'; New = 'tick >= completed && tick <= submitted' },
    @{ Name = 'the unfinished-tick filter ignores the submitted tick'; File = $trace; Old = 'tick > completed && tick <= submitted'; New = 'tick > completed' },
    @{ Name = 'a present submission takes the draw pipelines'; File = $trace; Old = "    if (timeline != Timeline::Draw) {`n        return;`n    }`n    // The set recorded"; New = '    // The set recorded' },
    @{ Name = 'the graphics pipeline bound is removed'; File = $trace; Old = 'std::min({graphics, GraphicsPrinted, GraphicsPerSet})'; New = 'std::min(graphics, GraphicsPerSet)' },
    @{ Name = 'the dispatch window is removed'; File = $trace; Old = 'dispatches > DispatchesPerSet ? dispatches - DispatchesPerSet : 0'; New = '0' },
    @{ Name = 'the trace is printed more than once'; File = $trace; Old = 'if (dumped.exchange(true, std::memory_order_acq_rel)) {'; New = 'if (false) {' },
    @{ Name = 'the Bug Report no longer extracts the trace summary'; File = $report; Old = 'if (diag.gpu_trace_summary.empty() && line.starts_with(kGpuTraceSummaryMark)) {'; New = 'if (false) {' },
    @{ Name = 'the trace summary is not carried by a device loss'; File = $report; Old = 'if (diag.device_lost && !diag.gpu_trace_summary.empty()) {'; New = 'if (false) {' }
)
$code = Invoke-HostMutationTest -Name 'gpu_crash_trace' -Root $root -Test 'tests/gpu_crash_trace_tests.cpp' -Sources $sources `
    -Copy $copy -IncludeDirs $dirs -RootIncludeDirs $dirs -Libraries @('fmt', 'pthread') -Mutations $mutations

# The same test on Linux (WSL): the production trace writes to the real stderr there (the game's log pipe), and the
# device-lost and assertion children end through the Linux paths. Not applicable without WSL clang++ and libfmt.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$probe = if ($wsl) { (& wsl.exe -e sh -c 'command -v clang++ >/dev/null 2>&1 && test -d /usr/include/fmt && echo ready' 2>$null) -join '' } else { '' }
if ($probe.Trim() -ne 'ready') {
    Write-Host '  [n/a]  Linux run: no WSL clang++ with libfmt (not applicable, not a failure)'
} else {
    $repo = ((& wsl.exe -e wslpath -a ($root.Replace('\', '/'))) | Select-Object -First 1).Trim()
    $exe = '/tmp/sos_gpu_trace_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $linuxSources = (@('tests/gpu_crash_trace_tests.cpp') + $sources) -join ' '
    $script = "cd '$repo' && clang++ -std=gnu++23 -Wall -Wextra -O1 -Wno-missing-field-initializers -DSOS_SOURCE_DIR='`"$repo`"' -Igpu/shim -Igpu/shadps4 -Ilauncher -Isrc $linuxSources -lfmt -pthread -o '$exe' || exit 97; cd /tmp && '$exe' > '$exe.out' 2>&1; code=`$?; grep -E 'checks, [0-9]+ failures|\[fail\]' '$exe.out'; rm -f '$exe' '$exe.out'; exit `$code"
    $output = & wsl.exe -e sh -c $script 2>&1
    $output | ForEach-Object { Write-Host "  linux: $_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [fail] the Linux run exited with $LASTEXITCODE"
        $code = 1
    }
}
exit $code
