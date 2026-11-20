<#
.SYNOPSIS
  The one headless validation command: build parity of CUSA57767 1.00 and 1.01, the pad and title input path, the controller
  prompts and Settings dialog glyphs, Skip Intro and AvPlayer (including the decoded NV12 frame copy), the graphics imports and pixel input state, the texture cache and tiled ownership checks, the Vulkan robustness groups (graceful device lost, low VRAM fallback, sampler lifetime, NVIDIA stability), the frame rate
  model, the shutdown lifecycle, display mode and cursor policy of the host, the progress reporter and git whitespace. No game,
  launcher, window or audio device is started.
.DESCRIPTION
  Every group runs even after one fails; the exit code is non-zero when any failed. Nothing is built by this script: the
  groups compile their own small test programs in a temporary folder, as before.

  While a group runs, a progress block (percentage, bar, completed groups, elapsed time, ETA, check count so far, previous
  group) is printed at its start and refreshed on a timer: one carriage-return line with a | / - \ spinner every 500 ms in a
  terminal, a RUNNING text line every 15 s when output is redirected. A timing table and the slowest groups follow the last
  group. The check count is the number the group's own output reports; no total is invented.

  Each group is BUILD-SPECIFIC or SHARED HOST. A build-specific group must report both supported builds (the versions of
  scripts\game_profiles.py, listed by scripts\build_parity_check.py --builds) in its output and must not skip a part for one
  of them; the first group checks every build-specific table against that inventory and the prepared builds. To cover a new
  build-specific feature: register it in scripts\build_parity_check.py, then add its test as a 'build' group here.
.PARAMETER TimeoutSeconds
  Fails a group that runs longer than this.
.PARAMETER Detail
  Shows the whole output of passing groups, not only its last line.
.EXAMPLE
  .\tools\test.ps1
#>
[CmdletBinding()]
param([int]$TimeoutSeconds = 900, [switch]$Detail)

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_progress.ps1')
$root = $script:RepoRoot
$shell = (Get-Process -Id $PID).Path
$script:GroupChecks = [long]0
$script:SupportedBuilds = $null

function Result([string]$State, [string]$Reason = '') { [pscustomobject]@{ State = $State; Reason = $Reason } }

function ConvertTo-ArgumentString([string[]]$Arguments) {
    ($Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ } }) -join ' '
}

function Stop-ProcessTree($Process) {
    try { if (-not $Process.HasExited) { & taskkill.exe /PID $Process.Id /T /F 2>&1 | Out-Null } } catch { $null = $_ }
}

# Runs a program with stdout and stderr merged into Lines. While it runs, the group's progress is refreshed on a timer,
# and "@@sos-progress" lines from instrumented suites feed the check count; those lines are not part of Lines.
function Invoke-Native([string]$File, [string[]]$Arguments = @(), [string]$WorkingDirectory = $root) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = ConvertTo-ArgumentString $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $WorkingDirectory
    $info.EnvironmentVariables['SOS_TEST_PROGRESS'] = '1'
    $lines = New-Object System.Collections.Generic.List[string]
    try { $process = [System.Diagnostics.Process]::Start($info) }
    catch { return [pscustomobject]@{ Lines = @("could not start ${File}: $($_.Exception.Message)"); Code = -1 } }
    $started = $script:Clock.Elapsed.TotalSeconds
    $timedOut = $false
    try {
        $readers = @($process.StandardOutput, $process.StandardError)
        $pending = @($readers[0].ReadLineAsync(), $readers[1].ReadLineAsync())
        while ($pending[0] -or $pending[1]) {
            $active = @($pending | Where-Object { $_ })
            $index = [System.Threading.Tasks.Task]::WaitAny([System.Threading.Tasks.Task[]]$active, 500)
            if ($index -ge 0) {
                $slot = [Array]::IndexOf($pending, $active[$index])
                $line = $pending[$slot].Result
                if ($null -eq $line) { $pending[$slot] = $null }
                else {
                    $pending[$slot] = $readers[$slot].ReadLineAsync()
                    $child = ConvertFrom-ChildProgress $line
                    if ($child) { $null = Set-ProgressChecks $script:Progress $script:Clock.Elapsed.TotalSeconds $child.Checks $child.Expected $child.Estimate }
                    else {
                        $lines.Add($line)
                        $found = Get-LineCheckCount $line
                        if ($found -gt 0) {
                            $script:GroupChecks += $found
                            $null = Set-ProgressChecks $script:Progress $script:Clock.Elapsed.TotalSeconds $script:GroupChecks
                        }
                    }
                }
            }
            $null = Update-Progress $script:Progress $script:Clock.Elapsed.TotalSeconds
            if ($script:Clock.Elapsed.TotalSeconds - $started -gt $TimeoutSeconds) { $timedOut = $true; Stop-ProcessTree $process; break }
        }
        $process.WaitForExit()
        if ($timedOut) { $lines.Add("TIMEOUT: stopped after $TimeoutSeconds s"); return [pscustomobject]@{ Lines = $lines.ToArray(); Code = 124 } }
        return [pscustomobject]@{ Lines = $lines.ToArray(); Code = $process.ExitCode }
    } finally {
        Stop-ProcessTree $process
        $process.Dispose()
    }
}

function Get-SupportedBuilds {
    if ($null -eq $script:SupportedBuilds) {
        $python = Get-Python
        $script:SupportedBuilds = @()
        if ($python) {
            $listing = Invoke-Native $python.Exe (@($python.Args) + @('-I', (Join-Path $root 'scripts\build_parity_check.py'), '--builds'))
            if ($listing.Code -eq 0) { $script:SupportedBuilds = @($listing.Lines | Where-Object { $_ -match '^CUSA\d+-\d+\.\d+$' }) }
        }
    }
    return $script:SupportedBuilds
}

# A build-specific group counts only when it reported every supported build and skipped none of them.
function Test-BuildCoverage($Lines) {
    $builds = @(Get-SupportedBuilds)
    if ($builds.Count -eq 0) { return 'the supported builds could not be listed (scripts\build_parity_check.py --builds needs Python)' }
    $text = $Lines -join "`n"
    foreach ($build in $builds) {
        $title, $version = $build -split '-', 2
        if ($text -notmatch ('{0}[- ]{1}' -f [regex]::Escape($title), [regex]::Escape($version))) { return "the output never names ${title} ${version}: a supported build was not validated" }
    }
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[skip\]' -and $line -match 'CUSA\d+') { return "a part was skipped for a supported build: $($line.Trim())" }
    }
    return $null
}

# The outcome of a finished group: exit code, failure lines under exit code 0, and the build coverage of build groups.
function Get-GroupResult($Run, [string]$Scope, [string]$Label) {
    $lines = @($Run.Lines)
    $problem = $null
    if ($Run.Code -ne 0) { $problem = "$Label exited with $($Run.Code)" }
    elseif (@($lines | Where-Object { $_ -match '^\s*(\[fail\s*\]|FAIL|FAILED)(\s|:|$)' }).Count) { $problem = "$Label exited with 0 but reported a failure" }
    elseif ($Scope -eq 'build') { $problem = Test-BuildCoverage $lines }
    if ($problem) {
        $lines | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' $problem
    }
    if ($Detail) { $lines | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } }
    else { $lines | Where-Object { $_.Trim() } | Select-Object -Last 1 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray } }
    return Result 'PASS'
}

function Invoke-Helper([string]$Script, [string]$Scope) {
    $run = Invoke-Native $shell @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot $Script))
    Get-GroupResult $run $Scope $Script
}

function Invoke-PythonCheck([string[]]$Arguments, [string]$Scope) {
    $python = Get-Python
    if (-not $python) { return Result 'FAIL' 'Python 3 was not found (tools\setup.ps1)' }
    $run = Invoke-Native $python.Exe (@($python.Args) + @('-I') + $Arguments)
    Get-GroupResult $run $Scope $Arguments[0]
}

# One lifecycle area of the host (shutdown, display or cursor): an optional helper program, the source gate and the gate's
# self-test on mutated copies, reported as one group.
function Invoke-LifecycleArea([string]$Area, [string]$Helper = '') {
    $python = Get-Python
    if (-not $python) { return Result 'FAIL' 'Python 3 was not found (tools\setup.ps1)' }
    $gate = Join-Path $root 'scripts\window_lifecycle_check.py'
    $runs = New-Object System.Collections.Generic.List[object]
    if ($Helper) { $runs.Add((Invoke-Native $shell @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot $Helper)))) }
    $runs.Add((Invoke-Native $python.Exe (@($python.Args) + @('-I', $gate, '--area', $Area))))
    $runs.Add((Invoke-Native $python.Exe (@($python.Args) + @('-I', $gate, '--area', $Area, '--self-test'))))
    $code = 0
    foreach ($run in $runs) { if ($run.Code -ne 0) { $code = $run.Code } }
    $merged = [pscustomobject]@{ Lines = @($runs | ForEach-Object { $_.Lines }); Code = $code }
    Get-GroupResult $merged 'host' "window_lifecycle_check.py --area $Area"
}

function Invoke-GitWhitespace {
    $problems = New-Object System.Collections.Generic.List[string]
    foreach ($arguments in @('diff', '--check'), @('diff', '--cached', '--check')) {
        $run = Invoke-Native 'git' (@('-C', $root) + $arguments)
        if ($run.Code -ne 0) { $problems.Add("git $($arguments -join ' ') reported problems:"); $run.Lines | ForEach-Object { $problems.Add("  $_") } }
    }
    $untracked = Invoke-Native 'git' @('-C', $root, 'ls-files', '--others', '--exclude-standard')
    $textual = '.c', '.h', '.cpp', '.inc', '.py', '.ps1', '.sh', '.js', '.md', '.txt', '.json', '.cmake', '.toml', '.yml'
    $new = 0
    foreach ($relative in @($untracked.Lines | Where-Object { $_ })) {
        if ($textual -notcontains [IO.Path]::GetExtension($relative).ToLowerInvariant()) { continue }
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $new++
        $number = 0
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            $number++
            if ($line -match '[ \t]$') { $problems.Add("${relative}:${number}: trailing whitespace (new file)") }
            elseif ($line -match '^(<<<<<<<|>>>>>>>) ') { $problems.Add("${relative}:${number}: conflict marker (new file)") }
        }
    }
    if ($problems.Count) {
        $problems | Select-Object -First 40 | ForEach-Object { Write-Host "    $_" }
        return Result 'FAIL' 'whitespace errors or conflict markers'
    }
    Write-Host "    clean, plus $new new text files" -ForegroundColor DarkGray
    return Result 'PASS'
}

$parity = Join-Path $root 'scripts\build_parity_check.py'

$steps = @(
    @{ Name = 'build-parity'; Scope = 'build'; Title = 'Build parity: every build-specific table (identity, Skip Intro sites, Xbox prompt sites and hook, dialog glyph sites, resolution sites) covers CUSA57767 1.00 and 1.01 and nothing else, the builds agree where they must, the prepared eboots and boot images are the supported builds, and the AvPlayer caller (InitEx, 6 video buffers, auto start off, file callbacks, SetAvSyncMode None) is equivalent in both'; Block = { Invoke-PythonCheck @($parity) 'build' } },
    @{ Name = 'build-parity-selftest'; Scope = 'host'; Title = 'Build parity gate self-test: it fails for a build missing from a table, an unknown build, a changed patch byte, fingerprint or layout, a test that names one build, and each AvPlayer setup change (mutated copies of the data, nothing written)'; Block = { Invoke-PythonCheck @($parity, '--self-test') 'host' } },
    @{ Name = 'graphics-imports'; Scope = 'build'; Title = 'Graphics imports (GnmDriver, VideoOut, Razor) of the eboot and every loaded module in CUSA57767 1.00 and 1.01: each has a named host function, no stub that returns success or aborts has a guest caller, and class and caller presence match scripts\graphics_imports.json'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_import_check.py')) 'build' } },
    @{ Name = 'graphics-imports-selftest'; Scope = 'host'; Title = 'Graphics import gate self-test: it fails for an import missing from the inventory or from one build, an import with no host function, a class change, an unnamed import, a new guest caller, a called stub, and a build or inventory entry that is not supported'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_import_check.py'), '--self-test') 'host' } },
    @{ Name = 'gpu-production-link'; Scope = 'host'; Title = 'Production GPU link: the CURRENT sources of the real target dcpgpu (CMake source registration incl. device_lost.cpp, compile and link integration) built incrementally once, and every object linked with the loader runtime by dcpgpu_link_smoke (undefined and duplicate symbols); later GPU groups reuse it'; Block = { $result = Invoke-Helper 'test_gpu_production.ps1' 'host'; if ($result.State -eq 'PASS') { $env:SOS_GPU_PRODUCTION_CURRENT = '1' }; $result } },
    @{ Name = 'graphics-state-source'; Scope = 'host'; Title = 'Fragment input path in the video core: stage info cleared before it is filled, inputs filled for exactly NUM_INTERP entries from SPI_PS_INPUT_CNTL and by nothing else, equal table sizes, every consumer bounded by the count'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_state_check.py')) 'host' } },
    @{ Name = 'graphics-state-selftest'; Scope = 'host'; Title = 'Fragment input gate self-test: each invariant fails on a mutated copy of the sources (no clearing, loop bound, dropped flat bit, wrong field, second writer, table size, layout, stale equality, unbounded consumer)'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_state_check.py'), '--self-test') 'host' } },
    @{ Name = 'graphics-state'; Scope = 'build'; Title = 'Pixel input state on the production runtime info (A -> B -> A, poisoned storage, stale entries, mutated fixtures) and the shaders embedded in the CUSA57767 1.00 and 1.01 eboots decoded by the production GCN decoder'; Block = { Invoke-Helper 'test_graphics_state.ps1' 'build' } },
    @{ Name = 'pad-input'; Scope = 'build'; Title = 'Pad delivery: buttons, Back as Touchpad (not on DualShock 4/DualSense), Map action in the game code, sticks, press/hold/release, 30-240 Hz, baseline commit'; Block = { Invoke-Helper 'test_pad_input.ps1' 'build' } },
    @{ Name = 'title-patches'; Scope = 'build'; Title = 'Title input path and patch combinations on the real game code'; Block = { Invoke-Helper 'test_title_patches.ps1' 'build' } },
    @{ Name = 'controller-prompts'; Scope = 'build'; Title = 'Controller prompts (Automatic hook, PlayStation, Xbox patch)'; Block = { Invoke-Helper 'test_controller_prompts.ps1' 'build' } },
    @{ Name = 'settings-dialog'; Scope = 'build'; Title = 'Settings dialog glyphs (Xbox A/B in the confirmation dialogs, restoration, hook on the real code)'; Block = { Invoke-Helper 'test_prompt_dialog.ps1' 'build' } },
    @{ Name = 'controller-icons'; Scope = 'host'; Title = 'Controller icons (Xbox left-stick glyphs, prepared copies, cache; the base game''s resources.assets serves both builds)'; Block = { Invoke-Helper 'test_controller_icons.ps1' 'host' } },
    @{ Name = 'guest-file-paths'; Scope = 'host'; Title = 'Guest file paths (exact path alias for the prepared icons)'; Block = { Invoke-Helper 'test_file_paths.ps1' 'host' } },
    @{ Name = 'skip-intro'; Scope = 'build'; Title = 'Skip Intro (patch flow, title stub, player), the AvPlayer API tests and the decoded NV12 frame copy (rows at the guest pitch for any decoder row size, padded height, mutated copies)'; Block = { Invoke-Helper 'test_skip_intro.ps1' 'build' } },
    @{ Name = 'image-reuse'; Scope = 'host'; Title = 'Texture cache address reuse (linear/tiled layouts, depth never aliases color), write-back before fences, boundary pages'; Block = { Invoke-Helper 'test_image_reuse.ps1' 'host' } },
    @{ Name = 'tiled-ownership'; Scope = 'host'; Title = 'Tiled CPU/GPU ownership on the real TileImage/DetileImage shaders (headless Vulkan compute, no window)'; Block = { Invoke-Helper 'test_tiled_ownership.ps1' 'host' } },
    @{ Name = 'device-lost'; Scope = 'host'; Title = 'Graceful Vulkan device lost with per-timeline GPU ticks (draw, present, flip, sparse bind keep their own submitted and completed ticks; every failure and wait names its timeline; the root report lists them): the first VK_ERROR_DEVICE_LOST (submit, sparse bind, semaphore read or wait, fence wait, acquire, present, idle, query) owns the root cause and the process end, later faults and other Vulkan errors never replace it, no submission, sparse bind, read or wait reaches a lost device or blocks, workers stop, VK_EXT_device_fault is asked once and bounded, every renderer call site is guarded, and mutated copies of the monitor and the semaphore helpers must fail'; Block = { Invoke-Helper 'test_device_lost.ps1' 'host' } },
    @{ Name = 'thread-runtime'; Scope = 'host'; Title = 'Guest thread cancellation on the real runtime thread layer: scePthreadCancel is deferred (invalid and fake handles, finished, running without and with cancellation points, disabled cancel state, detached, raced with completion, many threads finishing while cancelled, concurrent join), never blocks, never ends the process and keeps join, result and reaping valid; mutated copies, among them the old fatal cancel of an active thread, must fail'; Block = { Invoke-Helper 'test_thread_runtime.ps1' 'host' } },
    @{ Name = 'posix-sigpipe'; Scope = 'host'; Title = 'POSIX SIGPIPE: the loader installs SIG_IGN at the start of main on non-Windows targets only; a write to a socketpair whose peer closed kills a child by default and returns EPIPE under the startup policy, other signals untouched (runs under WSL when present, otherwise not applicable on Windows); mutated policy and loader must fail'; Block = { Invoke-Helper 'test_sigpipe.ps1' 'host' } },
    @{ Name = 'save-mount-diagnostics'; Scope = 'host'; Title = 'Save mount diagnostics on the real save implementation: sceSaveDataMount and sceSaveDataMount2 log directory (placeholder when invalid), error and mode of every failed mount, nothing for a successful one, and return the unchanged error; mutated copies must fail'; Block = { Invoke-Helper 'test_save_mount.ps1' 'host' } },
    @{ Name = 'memory-fallback'; Scope = 'host'; Title = 'Low VRAM fallback: memory types tried preferred first, other device-local next, system memory last, only after VK_ERROR_OUT_OF_DEVICE_MEMORY, never outside memoryTypeBits or without the required properties, telemetry only for a real fallback, the sparse backing and frame image wiring, and mutated copies of the policy that must fail'; Block = { Invoke-Helper 'test_memory_fallback.ps1' 'host' } },
    @{ Name = 'sampler-lifetime'; Scope = 'host'; Title = 'Sampler lifetime: a sampler is stamped with the scheduler tick of its draw and destroyed only after the GPU completed it (pinned samplers do not hide idle ones, bounded under a lagging GPU, destroyed once), the texture cache wiring, and mutated copies of the collector that must fail'; Block = { Invoke-Helper 'test_sampler_lifetime.ps1' 'host' } },
    @{ Name = 'nvidia-stability'; Scope = 'host'; Title = 'NVIDIA stability: the fragment shader reads the emulated clip-distance attribute exactly when the hardware vertex stage writes it (vertex, geometry, tessellation, no pixel shader, A -> B -> A, no stale fragment state), the pipeline cache and SPIR-V emitter wiring, the thread-naming exception left alone by the crash guard, and mutated copies of the rule that must fail (decoded NV12 rows are in the AvPlayer group)'; Block = { Invoke-Helper 'test_nvidia_stability.ps1' 'host' } },
    @{ Name = 'frame-rate'; Scope = 'build'; Title = 'Frame rate model'; Block = { Invoke-Helper 'test_frame_model.ps1' 'build' } },
    @{ Name = 'shutdown-lifecycle'; Scope = 'host'; Title = 'Shutdown lifecycle: every exit path ends the process with _Exit/_exit (no exit(), no main return after the video core starts), the window is never destroyed, no thread_local needs a joining or Vulkan teardown, no library unload or D3D12 path exists, and the gate fails on mutated copies of the sources'; Block = { Invoke-LifecycleArea 'shutdown' } },
    @{ Name = 'display-mode'; Scope = 'host'; Title = 'Display mode: Borderless is applied to the hidden window before it is shown and before the first surface and swapchain, Windowed is unchanged, F11 stays a runtime toggle, no synthetic startup key exists, and the gate fails on mutated copies of the sources'; Block = { Invoke-LifecycleArea 'display' } },
    @{ Name = 'cursor-policy'; Scope = 'host'; Title = 'Cursor policy: 5 s idle hide in fullscreen only, motion of the game window, focus and fullscreen transitions, one SDL call per change (injected clock), the window wiring, and mutated copies of the policy and the wiring that must fail'; Block = { Invoke-LifecycleArea 'cursor' 'test_cursor_policy.ps1' } },
    @{ Name = 'progress-reporter'; Scope = 'host'; Title = 'Test progress: percentage, bar, durations, ETA, check counts, previous group, timing summary, redirected output and a synthetic long group (synthetic time, no sleeping)'; Block = { Invoke-Helper 'test_progress.ps1' 'host' } },
    @{ Name = 'git-whitespace'; Scope = 'host'; Title = 'git diff --check (staged, unstaged and new files)'; Block = { Invoke-GitWhitespace } }
)

$environment = [ordered]@{ SDL_VIDEODRIVER = 'dummy'; SDL_AUDIODRIVER = 'dummy'; SOS_GPU_PRODUCTION_CURRENT = $null }
$savedEnvironment = @{}
foreach ($name in $environment.Keys) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }

$script:Clock = [System.Diagnostics.Stopwatch]::StartNew()
$interactive = -not [Console]::IsOutputRedirected -and $Host.Name -eq 'ConsoleHost'
$script:Progress = New-ProgressReporter @($steps | ForEach-Object { $_.Name }) 0 $interactive {
    param([string]$Text, [bool]$NoNewline)
    if ($NoNewline) { Write-Host $Text -NoNewline } else { Write-Host $Text }
}

$failed = 0
$finishedRun = $false
try {
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    foreach ($step in $steps) {
        $script:GroupChecks = [long]0
        Start-ProgressGroup $script:Progress $step.Name $script:Clock.Elapsed.TotalSeconds
        Write-Step ('[{0}] {1}' -f $(if ($step.Scope -eq 'build') { 'builds' } else { 'host' }), $step.Title)
        try { $result = & $step.Block }
        catch { $result = Result 'FAIL' "the group could not run: $($_.Exception.Message)" }
        Clear-ProgressLine $script:Progress
        switch ($result.State) {
            'PASS' { Write-Host '    PASS' -ForegroundColor Green }
            'FAIL' { $failed++; Write-Host "    FAIL: $($result.Reason)" -ForegroundColor Red }
            default { Write-Host "    SKIPPED: $($result.Reason)" -ForegroundColor Yellow }
        }
        $final = if ($script:GroupChecks -gt 0) { $script:GroupChecks } else { $null }
        Complete-ProgressGroup $script:Progress $(if ($result.State -in 'PASS', 'FAIL') { $result.State } else { 'SKIPPED' }) $script:Clock.Elapsed.TotalSeconds $final
    }
    $finishedRun = $true
} finally {
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    Clear-ProgressLine $script:Progress
    Write-Host ''
    Format-TimingSummary $script:Progress $script:Clock.Elapsed.TotalSeconds | ForEach-Object { Write-Host $_ }
    if (-not $finishedRun) { Write-Host 'Run interrupted before the last group finished.' -ForegroundColor Yellow }
}
Write-Host ''
Write-Host "Failed: $failed"
if ($failed) { exit 1 }
exit 0
