<#
.SYNOPSIS
  The one headless validation command: build parity of CUSA57767 1.00 and 1.01, the pad and title input path, the controller
  prompts and Settings dialog glyphs, True Ultrawide, Skip Intro and AvPlayer (including the decoded NV12 frame copy), the graphics imports and pixel input state, the texture cache and tiled ownership checks, the Vulkan robustness groups (graceful device lost, low VRAM fallback, sampler lifetime, NVIDIA stability), the frame rate
  model, Hermes Mode early unlock, The Pit early unlock, the shutdown lifecycle, display mode and cursor policy of the host, the updater (timeouts, size limits, redirects, retries, self-update selection) on a loopback server, the launcher tests, launcher robustness (placement, startup stages, Escape, Bug Report feedback) and the launcher renderer (60 Hz pacing, minimized wake policy, telemetry, Direct3D 11 primary on Windows with pinned modules, software fallback), the Bug Report system (WebSocket protocol, resumable crash dump upload, private admin dashboard, My reports history), the progress reporter and git whitespace. No game,
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
  of them; the first group checks every build-specific table against that inventory. To cover a new build-specific feature:
  register it in scripts\build_parity_check.py, then add its test as a 'build' group here.

  A supported build is a contract of the port, checked on every run whatever is installed here. Local game data is extra
  evidence: a real boot image (build\image, then build\research\image), the prepared game folder and the extracted package
  layers are only read, and a group that finds none of them for a build reports it as EVIDENCE lines instead of failing or
  skipping. The last block of the run sums it up for each build: the contract (PASS or FAIL), the real image (PASS, FAIL or
  DATA UNAVAILABLE) and the prepared content (AVAILABLE or NOT INSTALLED). DATA UNAVAILABLE is neither a pass nor a failure.
.PARAMETER TimeoutSeconds
  Fails a group that runs longer than this.
.PARAMETER Detail
  Shows the whole output of passing groups, not only its last line.
.PARAMETER Filter
  Runs only the groups whose name matches one of these wildcard patterns (comma-separated or repeated), for example
  -Filter session-diagnostics,posix-*. The order of the full run is kept.
.PARAMETER List
  Lists every group (name, scope, title) and exits without running anything.
.EXAMPLE
  .\tools\test.ps1
.EXAMPLE
  .\tools\test.ps1 -Filter 'session-diagnostics','posix-fault-signal','crash-*'
.EXAMPLE
  .\tools\test.ps1 -List
#>
[CmdletBinding()]
param([int]$TimeoutSeconds = 900, [switch]$Detail, [string[]]$Filter = @(), [switch]$List)

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_progress.ps1')
$root = $script:RepoRoot
$shell = (Get-Process -Id $PID).Path
$script:GroupChecks = [long]0
$script:SupportedBuilds = $null
$script:Skipped = New-Object System.Collections.Generic.List[string]
$script:DataUnavailable = New-Object System.Collections.Generic.List[string]
$script:EvidenceRecords = New-Object System.Collections.Generic.List[object]
$script:Outcomes = New-Object System.Collections.Generic.List[object]
$script:CurrentStep = $null

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

# The EVIDENCE lines of a group (what local game data it read, per build) and its DATA UNAVAILABLE notes, kept for the final summary.
function Add-GroupEvidence($Lines, [string]$Scope, [bool]$Passed) {
    $name = if ($script:CurrentStep) { $script:CurrentStep.Name } else { 'group' }
    foreach ($line in $Lines) {
        $record = ConvertFrom-EvidenceLine $line
        if ($record) {
            $script:EvidenceRecords.Add([pscustomobject]@{ Group = $name; Scope = $Scope; Passed = $Passed; Kind = $record.Kind; Build = $record.Build; State = $record.State; Detail = $record.Detail })
        } elseif ($line -match '^\s*\[data\]') {
            $script:DataUnavailable.Add(('{0}: {1}' -f $name, ($line.Trim() -replace '^\[data\]\s*', '')))
        }
    }
}

# The outcome of a finished group: exit code, failure lines under exit code 0, and the build coverage of build groups.
function Get-GroupResult($Run, [string]$Scope, [string]$Label) {
    $lines = @($Run.Lines)
    foreach ($line in $lines) {
        if ($line -match '^\s*\[skip\]') { $script:Skipped.Add(('{0}: {1}' -f $Label, $line.Trim())) }
    }
    $problem = $null
    if ($Run.Code -ne 0) { $problem = "$Label exited with $($Run.Code)" }
    elseif (@($lines | Where-Object { $_ -match '^\s*(\[fail\s*\]|FAIL|FAILED)(\s|:|$)' }).Count) { $problem = "$Label exited with 0 but reported a failure" }
    elseif ($Scope -eq 'build') { $problem = Test-BuildCoverage $lines }
    Add-GroupEvidence $lines $Scope (-not $problem)
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
    @{ Name = 'build-parity'; Scope = 'build'; Title = 'Build parity: every build-specific table (identity, Skip Intro sites, Xbox prompt sites and hook, dialog glyph sites, resolution sites) covers CUSA57767 1.00 and 1.01 and nothing else, the builds agree where they must, any local eboot or boot image is the supported build it is for, the builds register, agree on image size and fingerprint and share their base layer, and the AvPlayer caller (InitEx, 6 video buffers, auto start off, file callbacks, SetAvSyncMode None) is equivalent in both'; Block = { Invoke-PythonCheck @($parity) 'build' } },
    @{ Name = 'build-contract'; Scope = 'build'; Title = 'Build contract of CUSA57767 1.00 and 1.01 from the source tables alone (registration, fingerprints, every feature in every build, patched and verified ranges inside the image and shared by no two features, unknown builds refused) and mutated tables that must fail'; Block = { Invoke-Helper 'test_build_contract.ps1' 'build' } },
    @{ Name = 'build-evidence'; Scope = 'host'; Title = 'Local build evidence: boot image lookup (build\image, then build\research\image), game folders and extracted layers, and the gates and the run summary with only 1.00, only 1.01, both, a research image alone, no prepared folder and no data at all (temporary synthetic folders, no game data)'; Block = { Invoke-Helper 'test_build_evidence.ps1' 'host' } },
    @{ Name = 'build-parity-selftest'; Scope = 'host'; Title = 'Build parity gate self-test: it fails for a build missing from a table, an unknown build, a changed patch byte, fingerprint or layout, a test that names one build, and each AvPlayer setup change (mutated copies of the data, nothing written)'; Block = { Invoke-PythonCheck @($parity, '--self-test') 'host' } },
    @{ Name = 'supported-builds'; Scope = 'host'; Title = 'Supported builds: CUSA57767 1.00 alone and the optional 1.01 update are discovered, selected, installed and removed independently, with the expected package layers and shared files'; Block = { Invoke-Helper 'test_supported_builds.ps1' 'host' } },
    @{ Name = 'graphics-imports'; Scope = 'build'; Title = 'Graphics imports (GnmDriver, VideoOut, Razor) of the eboot and every loaded module in CUSA57767 1.00 and 1.01: each has a named host function, no stub that returns success or aborts has a guest caller, and class and caller presence match scripts\graphics_imports.json'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_import_check.py')) 'build' } },
    @{ Name = 'graphics-imports-selftest'; Scope = 'host'; Title = 'Graphics import gate self-test: it fails for an import missing from the inventory or from one build, an import with no host function, a class change, an unnamed import, a new guest caller, a called stub, and a build or inventory entry that is not supported'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_import_check.py'), '--self-test') 'host' } },
    @{ Name = 'gpu-production-link'; Scope = 'host'; Title = 'Production GPU link: the CURRENT sources of the real target dcpgpu (CMake source registration incl. device_lost.cpp, compile and link integration) built incrementally once, and every object linked with the loader runtime by dcpgpu_link_smoke (undefined and duplicate symbols); later GPU groups reuse it'; Block = { $result = Invoke-Helper 'test_gpu_production.ps1' 'host'; if ($result.State -eq 'PASS') { $env:SOS_GPU_PRODUCTION_CURRENT = '1' }; $result } },
    @{ Name = 'build-graph'; Scope = 'host'; Title = 'Production build graph: touching a GPU source or header recompiles exactly its consumers, rebuilds libdcpgpu.a and relinks sons_of_sparta.exe, and an unchanged build does no work (content untouched, timestamps restored)'; Block = { Invoke-Helper 'test_build_graph.ps1' 'host' } },
    @{ Name = 'build-run-rebuild'; Scope = 'host'; Title = 'build.ps1 incremental safety: -Run reuses the current dcpgpu artifacts, an unchanged runtime identity rewrites nothing, a changed compiler, configuration, FFmpeg pin or video-core dependency pin or patch resets only the GPU artifacts, a changed dependency pin is noticed with the checkout present, the build mutex is released on failure and a failed build never launches the game'; Block = { Invoke-Helper 'test_build_run_rebuild.ps1' 'host' } },
    @{ Name = 'build-install'; Scope = 'host'; Title = 'Build/install split: build.ps1 only builds and refreshes installed boot images, the launcher owns package extraction and prepared views, and incomplete, stale and malformed installations are rejected'; Block = { Invoke-Helper 'test_build_install.ps1' 'host' } },
    @{ Name = 'install'; Scope = 'host'; Title = 'Game installer on the real file system with synthetic packages (SOS-6Y8NVY): the v0.1.0 installer fails the reported file past MAX_PATH at 43% with ERROR_PATH_NOT_FOUND and the current one installs it; paths of 259 to 700 characters with spaces, brackets and Unicode, a long source package; the production manifest declares longPathAware; file errors keep their operation, path, length and system error with no extended-length prefix; failures, a full disk, cancelling, stale or held staging and failed activations leave the installed game and its sentinels unchanged; the launcher task shows the cleanup and becomes usable again; update over a valid base and its removal; the installed game''s own path limit; mutated copies that must fail; Linux under WSL; every run stays out of the repository'; Block = { Invoke-Helper 'test_install.ps1' 'host' } },
    @{ Name = 'graphics-state-source'; Scope = 'host'; Title = 'Fragment input path in the video core: stage info cleared before it is filled, inputs filled for exactly NUM_INTERP entries from SPI_PS_INPUT_CNTL and by nothing else, equal table sizes, every consumer bounded by the count'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_state_check.py')) 'host' } },
    @{ Name = 'graphics-state-selftest'; Scope = 'host'; Title = 'Fragment input gate self-test: each invariant fails on a mutated copy of the sources (no clearing, loop bound, dropped flat bit, wrong field, second writer, table size, layout, stale equality, unbounded consumer)'; Block = { Invoke-PythonCheck @((Join-Path $root 'scripts\graphics_state_check.py'), '--self-test') 'host' } },
    @{ Name = 'graphics-state'; Scope = 'build'; Title = 'Pixel input state on the production runtime info (A -> B -> A, poisoned storage, stale entries, mutated fixtures) and the shaders embedded in the CUSA57767 1.00 and 1.01 eboots decoded by the production GCN decoder'; Block = { Invoke-Helper 'test_graphics_state.ps1' 'build' } },
    @{ Name = 'renderer-paths'; Scope = 'host'; Title = 'Graphics and Display option paths behind the production renderer state: texture filtering, SMAA, FSR 1, RCAS, VSync, presentation and frame pacing helpers'; Block = { Invoke-Helper 'test_renderer_paths.ps1' 'host' } },
    @{ Name = 'sampler-filter'; Scope = 'host'; Title = 'Texture filtering on real AmdGpu sampler descriptors: only the intended samplers change, the device limit is respected and the cache key follows the setting'; Block = { Invoke-Helper 'test_sampler_filter.ps1' 'host' } },
    @{ Name = 'pad-input'; Scope = 'build'; Title = 'Pad delivery: buttons, Back as Touchpad (not on DualShock 4/DualSense), Map action in the game code, sticks, press/hold/release, 30-240 Hz, baseline commit'; Block = { Invoke-Helper 'test_pad_input.ps1' 'build' } },
    @{ Name = 'move'; Scope = 'host'; Title = 'PlayStation Move state and handles: no controller exposed, synthetic distinct handles and shadPS4-compatible errors'; Block = { Invoke-Helper 'test_move.ps1' 'host' } },
    @{ Name = 'np-toolkit'; Scope = 'host'; Title = 'NpToolkit2 runtime exports against UnityNpToolkit2.prx imports and the generated binding table'; Block = { Invoke-Helper 'test_np_toolkit2.ps1' 'host' } },
    @{ Name = 'controller-database'; Scope = 'host'; Title = 'User gamepad database (user/gamecontrollerdb.txt) and unmapped joystick diagnostics: path resolution, load-once before classification, missing/empty/malformed/unreadable handling, hotplug, SDL_GAMECONTROLLERCONFIG priority, built-in preservation, name/GUID/VID:PID diagnostics, log-once lifetime, and mutated copies that must fail'; Block = { Invoke-Helper 'test_controller_database.ps1' 'host' } },
    @{ Name = 'title-patches'; Scope = 'build'; Title = 'Title input path and patch combinations on the real game code'; Block = { Invoke-Helper 'test_title_patches.ps1' 'build' } },
    @{ Name = 'hermes-unlock'; Scope = 'build'; Title = 'Hermes Mode early unlock (CUSA57767 1.00 and 1.01): Gameplay setting On/Off and default Off, the six-byte branch patch on both real boot images, off leaving the game bytes untouched, unknown-build and changed-image safety, save-mode identity and the cheat-code flag path'; Block = { Invoke-Helper 'test_hermes_unlock.ps1' 'build' } },
    @{ Name = 'pit-unlock'; Scope = 'build'; Title = 'The Pit early unlock (CUSA57767 1.00 and 1.01): Gameplay setting and default Off, the two-byte branch patch on both real boot images, Hermes and Exit Game independence, unknown-build, changed-image and missing-content refusal, the static proof from the game code and scenes, and mutated copies that must fail'; Block = { Invoke-Helper 'test_pit_unlock.ps1' 'build' } },
    @{ Name = 'exit-game'; Scope = 'build'; Title = 'Title menu Exit Game (CUSA57767 1.00 and 1.01 hooks, static scene/layout/navigation, all 23 text languages, host close once, unknown-build and partial-patch safety, and mutations)'; Block = { Invoke-Helper 'test_exit_game.ps1' 'build' } },
    @{ Name = 'ultrawide'; Scope = 'build'; Title = 'True Ultrawide (CUSA57767 1.00 and 1.01): render target shapes (16:9 unchanged, 21:9 and 32:9 wider, vertical resolution kept where the game accepts it), the game''s seven Screen.SetResolution(1920, 1080) requests rewritten on both boot images, Hor+ orthographic view, presenter fit without stretch, the boot splash video kept at 16:9, no room-bound mask or centred 16:9 fallback, unknown-build and partial-patch refusal, the static gate on the game code and scenes with its self-test, the render resolution suite, and mutated copies that must fail'; Block = { Invoke-Helper 'test_ultrawide.ps1' 'build' } },
    @{ Name = 'controller-prompts'; Scope = 'build'; Title = 'Controller prompts (Automatic hook, PlayStation, Xbox patch)'; Block = { Invoke-Helper 'test_controller_prompts.ps1' 'build' } },
    @{ Name = 'settings-dialog'; Scope = 'build'; Title = 'Settings dialog and campfire menu glyphs (Xbox A/B, restoration, hooks on the real code, mutations)'; Block = { Invoke-Helper 'test_prompt_dialog.ps1' 'build' } },
    @{ Name = 'image-glyphs'; Scope = 'build'; Title = 'Menu glyph Images without a controller follower (pause, vendor and Pit L2/R2 tabs, death screen, tutorials, dialogue): sprite-to-Xbox mapping against the game''s icon maps, the handlers on fake Images in every prompt style, the hooks on the real code of CUSA57767 1.00 and 1.01, coexistence with the other hooks, the scenes, and mutations'; Block = { Invoke-Helper 'test_image_glyphs.ps1' 'build' } },
    @{ Name = 'controller-icons'; Scope = 'host'; Title = 'Controller icons (Xbox left-stick glyphs and interaction icons in resources.assets and the Addressables icon bundle, prepared copies, cache, mutations; the base game''s files serve both builds)'; Block = { Invoke-Helper 'test_controller_icons.ps1' 'host' } },
    @{ Name = 'guest-file-paths'; Scope = 'host'; Title = 'Guest file paths (exact path aliases for the prepared icons and icon bundle)'; Block = { Invoke-Helper 'test_file_paths.ps1' 'host' } },
    @{ Name = 'skip-intro'; Scope = 'build'; Title = 'Skip Intro (patch flow, title stub, player), the AvPlayer API tests and the decoded NV12 frame copy (rows at the guest pitch for any decoder row size, padded height, mutated copies)'; Block = { Invoke-Helper 'test_skip_intro.ps1' 'build' } },
    @{ Name = 'image-reuse'; Scope = 'host'; Title = 'Texture cache address reuse (linear/tiled layouts, depth never aliases color), write-back before fences, boundary pages'; Block = { Invoke-Helper 'test_image_reuse.ps1' 'host' } },
    @{ Name = 'redzone'; Scope = 'host'; Title = 'Red-zone analysis: hand-made functions and malformed tables, parallel analysis against a sequential decode at several worker counts, install digest, cache rejections and cached startup reuse'; Block = { Invoke-Helper 'test_redzone.ps1' 'host' } },
    @{ Name = 'clear-startup-cache'; Scope = 'host'; Title = 'Startup cache tool: missing, present and nested caches, status mode, deletion limited to the red-zone folder, and refusals for links and unsafe paths'; Block = { Invoke-Helper 'test_clear_startup_cache.ps1' 'host' } },
    @{ Name = 'tiled-ownership'; Scope = 'host'; Title = 'Tiled CPU/GPU ownership on the real TileImage/DetileImage shaders (headless Vulkan compute, no window)'; Block = { Invoke-Helper 'test_tiled_ownership.ps1' 'host' } },
    @{ Name = 'device-lost'; Scope = 'host'; Title = 'Graceful Vulkan device lost with per-timeline GPU ticks (draw, present, flip, sparse bind keep their own submitted and completed ticks; every failure and wait names its timeline; the root report lists them): the first VK_ERROR_DEVICE_LOST (submit, sparse bind, semaphore read or wait, fence wait, acquire, present, idle, query) owns the root cause and the process end, later faults and other Vulkan errors never replace it, no submission, sparse bind, read or wait reaches a lost device or blocks, workers stop, VK_EXT_device_fault is asked once and bounded, every renderer call site is guarded, and mutated copies of the monitor and the semaphore helpers must fail'; Block = { Invoke-Helper 'test_device_lost.ps1' 'host' } },
    @{ Name = 'first-gpu-ops'; Scope = 'host'; Title = 'First guest GPU operations (SOS-BXG9UT): the first shader translation, SPIR-V module, graphics and compute pipeline (with VkResult), draw, dispatch and swapchain acquire are each logged started/finished once, concurrent first callers elect one owner, the crash reports name the operation in flight with its shaders; mutations'; Block = { Invoke-Helper 'test_first_ops.ps1' 'host' } },
    @{ Name = 'intel-uhd630'; Scope = 'host'; Title = 'Intel UHD Graphics 630 startup crash (issue #2): crash reports name the host DLL with module, base, offset, size, file version and timestamp read lock-free from memory and no path (synthetic driver image and a loaded DLL), the Bug Report scrubber keeps that line, the newest vkCmdBeginRendering with its attachments (unbound colour slots) and whether the driver was inside it, host thread names, the UHD 630 capability profile (driver interpolation with the integer fix, no pipeline binaries without maintenance5), production wiring; mutations'; Block = { Invoke-Helper 'test_intel_uhd630.ps1' 'host' } },
    @{ Name = 'interpolation-spirv'; Scope = 'host'; Title = 'Interpolation routes on the real shader recompiler (SOS-BXG9UT): Steam Deck AMD explicit, NVIDIA manual barycentrics, Intel Xe2 barycentrics and the Intel ARL fallback (no AMD explicit, no KHR barycentrics, no fp64) each give spirv-valid modules; the fallback has no barycentric/AMD capability, keeps Flat and the integer fix, and a P10/P20 V_INTERP_MOV stops naming the shader after draining the log; WSL; mutations'; Block = { Invoke-Helper 'test_interpolation_spirv.ps1' 'host' } },
    @{ Name = 'gpu-crash-trace'; Scope = 'host'; Title = 'GPU crash trace: lock-free, allocation-free ring of the newest 512 GPU events (no torn events under concurrent writers), pipelines of unfinished draw ticks and the newest 32 dispatches, bounded prefixed dump printed once by the device-lost monitor (SOS_SIMULATE_DEVICE_LOST, exit 25) and by a GPU assertion (exit 23), summary read by the Bug Report and the dashboard; mutations'; Block = { Invoke-Helper 'test_gpu_crash_trace.ps1' 'host' } },
    @{ Name = 'thread-runtime'; Scope = 'host'; Title = 'Guest thread cancellation on the real runtime thread layer: scePthreadCancel is deferred (invalid and fake handles, finished, running without and with cancellation points, disabled cancel state, detached, raced with completion, many threads finishing while cancelled, concurrent join), never blocks, never ends the process and keeps join, result and reaping valid; the startup reserves both guest TLS indices once before the launcher, later calls reuse them after inline TLS exhaustion, a simulated late index 78 stops safely, and mutated copies, among them the old fatal cancel of an active thread, must fail'; Block = { Invoke-Helper 'test_thread_runtime.ps1' 'host' } },
    @{ Name = 'guest-entry'; Scope = 'host'; Title = 'Guest entry boundary: the main entry and every guest thread are called through a SysV trampoline that clears the frame pointer (so the guest C++ unwinder stops at the host/guest boundary instead of reading host stack data), passes both arguments, uses a supplied guest stack, returns the entry result, restores the host RSP/RBP, the loader and thread layer use it, and removing the zero frame pointer must fail'; Block = { Invoke-Helper 'test_guest_entry.ps1' 'host' } },
    @{ Name = 'linux-main-stack'; Scope = 'host'; Title = 'Guest main stack bounds (issue #5): on POSIX the guest main thread runs on its own 8 MiB low stack and scePthreadAttrGet/Getstackaddr/Getstacksize report exactly that mapping (low = first byte, high = exclusive top, rsp inside, the IL2CPP-style scan from rsp to high stays inside) while the guest entry runs, the host stack before and after it; the v0.1.0 bounds reproduced; guest threads keep their own stacks; Windows keeps its TEB stack; loader wiring after the module initializers; WSL; mutations'; Block = { Invoke-Helper 'test_linux_main_stack.ps1' 'host' } },
    @{ Name = 'umtx'; Scope = 'host'; Title = 'umtx semaphores on the production source: the timed semaphore wait of CUSA57767 1.01 blocks until a post, a wake or its timeout, failures follow the POSIX errno contract, and mutated copies must fail'; Block = { Invoke-Helper 'test_umtx.ps1' 'host' } },
    @{ Name = 'pthread-sched'; Scope = 'host'; Title = 'Guest pthread scheduling: priority and policy translation, affinity, renames and errors on the production thread layer'; Block = { Invoke-Helper 'test_pthread_sched.ps1' 'host' } },
    @{ Name = 'gc-suspend'; Scope = 'host'; Title = 'IL2CPP stop-the-world handshake: SIGUSR1 acknowledgement through a kernel semaphore and event flag, guest stack capture and resume on the real thread, signal, semaphore and event-flag layers'; Block = { Invoke-Helper 'test_gc_suspend.ps1' 'host' } },
    @{ Name = 'memory-map'; Scope = 'host'; Title = 'Guest memory map: reservations, direct memory, protection, mapping and error semantics on the production runtime memory layer'; Block = { Invoke-Helper 'test_memory_map.ps1' 'host' } },
    @{ Name = 'unwind-info'; Scope = 'host'; Title = 'Unwind-table lookup behind sceKernelGetModuleInfoForUnwind and the PS4 libc stop-code names'; Block = { Invoke-Helper 'test_unwind_info.ps1' 'host' } },
    @{ Name = 'runtime-log'; Scope = 'host'; Title = 'Concurrent runtime logging: one complete line per thread including multi-printf reports, and the crash-direct writer does not corrupt it'; Block = { Invoke-Helper 'test_runtime_log.ps1' 'host' } },
    @{ Name = 'history'; Scope = 'host'; Title = 'Runtime history ring: the last 32 transitions are retained, module and host events survive wrapping, and caller text appears only for a guest caller'; Block = { Invoke-Helper 'test_history.ps1' 'host' } },
    @{ Name = 'isa'; Scope = 'host'; Title = 'ISA compatibility: SSE4a emulation, INSERTQ rewrite, feature selection against a software reference, and for CUSA57767 1.00 and 1.01 the instruction boundaries and instruction-set inventory of the real executables (no AVX2, FMA, BMI2 or non-VEX SSE4 in the code the game runs; AES only behind its CPUID check; pinned counts)'; Block = { Invoke-Helper 'test_isa.ps1' 'host' } },
    @{ Name = 'cpu-compat'; Scope = 'build'; Title = 'Processors without AVX2 (issue #1), the authoritative check of the feature with no game, launcher or setting: the production plan and install (cpu_compat_plan/cpu_compat_install, as the loader and runtime_module.c call them) on the real boot images of CUSA57767 1.00 and 1.01 and on libfmod/libfmodstudio through runtime_module.c, for 18 CPU profiles (modern AVX2, AVX without AVX2, Sandy Bridge, Ivy Bridge, AMD FX Bulldozer and Piledriver, no AVX, no LZCNT, no BMI1, no MOVBE, no F16C, no SSE4a...) and with everything forced: normal path untouched, compatibility path byte-exact against the generated tables and an independent decode, refusals with nothing written; every stub form and every runnable real site run natively against the original (registers, flags, ymm, MXCSR, memory, rsp, red zone; Windows and Linux); F16C bit-exact; the loader and module-loader wiring; red-zone view; generator; port binaries and compiler flags free of AVX2; mutations'; Block = { Invoke-Helper 'test_cpu_compat.ps1' 'build' } },
    @{ Name = 'module-read'; Scope = 'host'; Title = 'Dynamic module file reads, including Il2cppUserAssemblies.prx on the prepared build'; Block = { Invoke-Helper 'test_module_read.ps1' 'host' } },
    @{ Name = 'import-preflight'; Scope = 'host'; Title = 'Import preflight for CUSA57767 1.00 and 1.01: every prepared PRX import table and PLT/GOT call site is checked against the real NID mappings, RuntimeExport names and NID-keyed resolver tables; SaveData.prx has no called import without a resolver, VwadwBBBJ80 maps to sceSaveDataUmountWithBackup, the full inventory covers the prepared modules in both builds, and removing the export or its NID mapping must fail'; Block = { Invoke-Helper 'test_import_preflight.ps1' 'host' } },
    @{ Name = 'posix-sigpipe'; Scope = 'host'; Title = 'POSIX SIGPIPE: the loader installs SIG_IGN at the start of main on non-Windows targets only; a write to a socketpair whose peer closed kills a child by default and returns EPIPE under the startup policy, other signals untouched (runs under WSL when present, otherwise not applicable on Windows); mutated policy and loader must fail'; Block = { Invoke-Helper 'test_sigpipe.ps1' 'host' } },
    @{ Name = 'posix-fault-signal'; Scope = 'host'; Title = 'POSIX fatal signals (Linux, under WSL): SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT write one CRASH report with the startup phase and last completed stage into the session log, then the process ends by its own signal (core dump policy and wait status kept, never _exit), recoverable faults return, nested and concurrent faults report once, SIGHUP is logged, SIGTERM/SIGINT stay with SDL; mutations'; Block = { Invoke-Helper 'test_fault_signal.ps1' 'host' } },
    @{ Name = 'appimage-libs'; Scope = 'host'; Title = 'AppImage library policy (package-linux.sh is_host_lib run under WSL bash): the host glibc, Vulkan loader, GL/EGL, display server and driver stacks (Mesa RADV, NVIDIA, VA-API, VDPAU, OpenCL) are never bundled, ordinary libraries are (libselinux, libpcre2, libcap included for SteamOS); mutations'; Block = { Invoke-Helper 'test_appimage_libs.ps1' 'host' } },
    @{ Name = 'save-mount-diagnostics'; Scope = 'host'; Title = 'Save mount diagnostics on the real save implementation: sceSaveDataMount and sceSaveDataMount2 log directory (placeholder when invalid), error and mode of every failed mount, nothing for a successful one, and return the unchanged error; mutated copies must fail'; Block = { Invoke-Helper 'test_save_mount.ps1' 'host' } },
    @{ Name = 'save-backup'; Scope = 'host'; Title = 'Save backup notification on the real save implementation: save and backup are on disk before it, the first poll after a backup is busy, notifications keep their order and errors, concurrent saves lose nothing, and the CUSA57767 1.01 save model completes in every timing; mutated copies must fail'; Block = { Invoke-Helper 'test_save_backup.ps1' 'host' } },
    @{ Name = 'memory-fallback'; Scope = 'host'; Title = 'Low VRAM fallback: memory types tried preferred first, other device-local next, system memory last, only after VK_ERROR_OUT_OF_DEVICE_MEMORY, never outside memoryTypeBits or without the required properties, telemetry only for a real fallback, the sparse backing and frame image wiring, and mutated copies of the policy that must fail'; Block = { Invoke-Helper 'test_memory_fallback.ps1' 'host' } },
    @{ Name = 'buffer-arena'; Scope = 'host'; Title = 'Buffer arenas: sparse layout unchanged on sparse devices; without sparse binding/residency or a sparse-capable queue (or SOS_FORCE_NON_SPARSE_ARENAS) fully resident arenas instead of a stop: mode selection, pages within the buffer and allocation limits, 128 MiB BDA table, migration copies then publishes then repoints then retires, stale bindings followed, device-local first with the low VRAM fallback, rasterizer/HLE/detile refresh before binds, indirect binds after the argument lookup, and mutated copies that must fail'; Block = { Invoke-Helper 'test_buffer_arena.ps1' 'host' } },
    @{ Name = 'trophies'; Scope = 'host'; Title = 'Local trophies: TRP unpacking, trophy-list decryption, Trophy Key, per-user progress, the sceNpTrophy service and the Trophies page model, including the prepared trophy pack'; Block = { Invoke-Helper 'test_trophies.ps1' 'host' } },
    @{ Name = 'audio-ajm'; Scope = 'host'; Title = 'Audio HLE (Ajm): contexts, modules, instances, batches and errors through the runtime exports, other codecs, and ATRAC9 decoding from an installed FMOD bank'; Block = { Invoke-Helper 'test_ajm.ps1' 'host' } },
    @{ Name = 'sampler-lifetime'; Scope = 'host'; Title = 'Sampler lifetime: a sampler is stamped with the scheduler tick of its draw and destroyed only after the GPU completed it (pinned samplers do not hide idle ones, bounded under a lagging GPU, destroyed once), the texture cache wiring, and mutated copies of the collector that must fail'; Block = { Invoke-Helper 'test_sampler_lifetime.ps1' 'host' } },
    @{ Name = 'nvidia-stability'; Scope = 'host'; Title = 'NVIDIA stability: the fragment shader reads the emulated clip-distance attribute exactly when the hardware vertex stage writes it (vertex, geometry, tessellation, no pixel shader, A -> B -> A, no stale fragment state), the pipeline cache and SPIR-V emitter wiring, the thread-naming exception left alone by the crash guard, and mutated copies of the rule that must fail (decoded NV12 rows are in the AvPlayer group)'; Block = { Invoke-Helper 'test_nvidia_stability.ps1' 'host' } },
    @{ Name = 'gpu-compat'; Scope = 'host'; Title = 'Vulkan GPU compatibility: VkAttachmentSampleCountInfoAMD only on genuinely mixed-sample pipelines (Steam Fossilize), interpolation route and integer fix per driver (Steam Deck and NVIDIA unchanged, Intel/NVK fixed), integer nudge, provokingVertexLast queried, surface/debug-utils entry points null-checked, shader binary version bumped, sparse buffer requirement named, heap labels and once-per-heap budget warning, and mutated copies that must fail'; Block = { Invoke-Helper 'test_gpu_compat.ps1' 'host' } },
    @{ Name = 'pipeline-cache'; Scope = 'host'; Title = 'Pipeline cache recovery: sealed blobs (truncated, zero, bit-flipped, oversized and trailing-garbage data refused), a bounded IO queue (exact submitted/done/failed/dropped counts, bounded drain at exit, refused writes when full), a store per game build (CUSA57767 1.00 and 1.01 apart, legacy store adopted or rebuilt with the reason logged), game pipelines ahead of warm-up builds with one retry, and mutated copies that must fail'; Block = { Invoke-Helper 'test_pipeline_cache.ps1' 'host' } },
    @{ Name = 'frame-rate'; Scope = 'build'; Title = 'Frame rate model'; Block = { Invoke-Helper 'test_frame_model.ps1' 'build' } },
    @{ Name = 'presentation-swapchain'; Scope = 'host'; Title = 'Swapchain health on the production state machine: SUCCESS, SUBOPTIMAL and OUT_OF_DATE are distinct, a SUBOPTIMAL acquire is a usable image presented before any recreation, 121 persistently suboptimal frames recreate once, a clean SUCCESS cycle re-arms one more, OUT_OF_DATE, resize, VSync and HDR recreations are never suppressed, the loading frame follows the same rules, device lost is never a recreation and acquire and present stay behind the device-lost guard on the Present timeline with no device wait, and mutated copies must fail'; Block = { Invoke-Helper 'test_presentation_swapchain.ps1' 'host' } },
    @{ Name = 'frame-pacing'; Scope = 'host'; Title = 'Frame pacing in simulated time on the production limiter: absolute slots without drift (60 to 480 Hz, 10 s to 10 min, late wakes and jitter), a late frame or a stall of 500 ms or seconds reanchors with no catch-up burst, every Frame rate setting against 60 to 240 Hz and unknown displays, original 60 and 30 (never halved twice), 90 FPS cadence, game step untouched, real flips before redraws, the setting applied at startup only, monotonic clock and one reusable high-resolution timer, and mutated copies must fail'; Block = { Invoke-Helper 'test_frame_pacing.ps1' 'host' } },
    @{ Name = 'runtime-hot-path'; Scope = 'host'; Title = 'Recursive lock helper of the draw path on the production source: nesting locks once, any release order, shared locks, no allocation after a thread''s first lock, and mutated copies must fail'; Block = { Invoke-Helper 'test_runtime_hot_path.ps1' 'host' } },
    @{ Name = 'runtime-settings'; Scope = 'host'; Title = 'Launcher settings to runtime: every display, graphics and gameplay key read by the video core, its translation and its source wiring'; Block = { Invoke-Helper 'test_runtime_settings.ps1' 'host' } },
    @{ Name = 'equeue-lifetime'; Scope = 'host'; Title = 'Kernel equeue lifetime and concurrency on the production sources: synchronized shared_ptr registry (no contains/operator[], no raw GetEqueue or Find().get()), value-captured timer handles, lookup/timer/periodic/wait/trigger vs delete interleavings, waiter wakeup on delete with EBADF, VideoOut/vblank/GnmDriver IRQ strong ownership and stale subscription cleanup, locked zero-timeout poll, multi-waiter level-triggered notification, and mutated copies must fail'; Block = { Invoke-Helper 'test_equeue_lifetime.ps1' 'host' } },
    @{ Name = 'shutdown-lifecycle'; Scope = 'host'; Title = 'Shutdown lifecycle: every exit path ends the process with _Exit/_exit (no exit(), no main return after the video core starts), the window is never destroyed, no thread_local needs a joining or Vulkan teardown, no library unload or D3D12 path exists, and the gate fails on mutated copies of the sources'; Block = { Invoke-LifecycleArea 'shutdown' } },
    @{ Name = 'display-mode'; Scope = 'host'; Title = 'Display mode: Borderless is applied to the hidden window before it is shown and before the first surface and swapchain, Windowed is unchanged, F11 stays a runtime toggle, no synthetic startup key exists, and the gate fails on mutated copies of the sources'; Block = { Invoke-LifecycleArea 'display' } },
    @{ Name = 'cursor-policy'; Scope = 'host'; Title = 'Input-aware cursor policy: mouse shows, keyboard and controller hide, stick/trigger deadzones with hysteresis, overlay override and post-close restoration, focus safety, Windowed/Borderless parity, SDL call deduplication, window wiring, and mutated copies of the policy and wiring that must fail'; Block = { Invoke-LifecycleArea 'cursor' 'test_cursor_policy.ps1' } },
    @{ Name = 'touchscreen'; Scope = 'host'; Title = 'SteamOS/Gamescope touchscreen: STEAM_TOUCH_CLICK_MODE Passthrough claimed on focus on the game and Gamescope roots, rewritten once a second, each root restored on focus loss and every exit path, never in the launcher, SOS_TOUCH_GAMESCOPE=0 kill switch, two-finger DualShock 4 touchpad contacts on the letterboxed picture, no synthetic-mouse double input, Windows and desktop unchanged, and mutated copies that must fail'; Block = { Invoke-Helper 'test_touchscreen.ps1' 'host' } },
    @{ Name = 'launcher-renderer'; Scope = 'host'; Title = 'Launcher renderer and performance on the production headers in simulated time: Windows prefers Direct3D 11 with d3d11.dll and dxgi.dll pinned first and software always behind it (SOS_LAUNCHER_RENDERER=software forces software; an unpinnable or failing renderer is skipped, nothing is retried, the selection is logged once), the Dragon teardown gate (one named renderer creation, no module unload, teardown order, game window without SDL renderer), 60 Hz absolute-deadline pacing independent of the renderer (no drift over 10 minutes or 7 days, late frames and 2 s stalls never burst, a blocking present is not halved), minimized launcher draws nothing and wakes ten times a second with an immediate restore, telemetry (render Hz, build, submit, present, UI thread CPU, minimized share, sustained-fast and present stall detection, rate limits, bounded lines), the host wiring, host.cpp and launcher.cpp compiled with -Werror, and mutated copies that must fail'; Block = { Invoke-Helper 'test_launcher_renderer.ps1' 'host' } },
    @{ Name = 'launcher-robustness'; Scope = 'host'; Title = 'Launcher robustness on the production helpers with synthetic displays and time: placement lines (position, size, display index and name, bounds, work area, scale at 100, 150 and 225 percent, negative coordinates, missing display, stale saved placement, bounded and free of personal data), restoration that keeps maximized and fullscreen when the rectangle is off-screen and a minimized window keeping its state, startup stages and the slow-stage threshold, the show gate (hidden until the renderer exists and a first frame was presented, shown once, never in screenshot mode, no pacing delay), Escape ownership (one press, one owner, never closes the launcher, error dialog and package chooser on a headless Dear ImGui, Bug Report text kept, Escape not bindable), the Bug Report session (duplicate submit blocked, text sent before dump upload, partial success, dump retry, ticket kept, receipt never shown, async result applied after leaving the page), and mutated copies that must fail'; Block = { Invoke-Helper 'test_launcher_robustness.ps1' 'host' } },
    @{ Name = 'updater'; Scope = 'host'; Title = 'Updater hardening on the production transport and retry layer against a loopback HTTP server, never GitHub: connect, receive and total timeouts (the total spans redirects and retries), byte limits for metadata, checksum and package, Content-Length (exact, short, larger, smaller, invalid, overflowing), interrupted transfers and the .part file (destination kept on every failure, stale .part truncated, atomic promotion), redirects (limit, loop, relative and dot-segment targets, HTTPS downgrade, scheme and origin comparison, credential headers stripped across origins only), transient statuses 408, 429, 500, 502, 503 and 504 against permanent 4xx, bounded retries with Retry-After and cancellation, the structured result, log privacy, self-update decisions (newer, same, older, malformed, draft, prerelease, wrong platform, architecture or similar asset, official asset names, one version source), a failed download leaving the installed package untouched, and mutated copies that must fail'; Block = { Invoke-Helper 'test_updater.ps1' 'host' } },
    @{ Name = 'launcher'; Scope = 'host'; Title = 'Launcher headless tests: pages, settings, key bindings, game process arguments, identity, supported-build names, TRG patch set, window fit and data folders (no window, game or network)'; Block = { Invoke-Helper 'test_launcher.ps1' 'host' } },
    @{ Name = 'crash-dump'; Scope = 'host'; Title = 'Crash dump discovery and diagnostics: same-execution log/dump pairing, runtime_log session-number reservation, MDMP/size/age/type validation, optional attachment, Crash detected state, authoritative Vulkan device-lost vs generic crash classification, bounded VK_EXT_device_fault extraction, privacy scrubbing, and mutated copies that must fail'; Block = { Invoke-Helper 'test_crash_dump.ps1' 'host' } },
    @{ Name = 'crash-report'; Scope = 'host'; Title = 'Bug Report system: the Dragon incident protocol over WebSocket (incident.file/status/read/feedback/dump/dump_end), resumable chunked minidump upload with mid-transfer drop recovery, receipt ownership, status revisions and player feedback, the private Bearer-authenticated admin list/detail/log/dump/status/delete dashboard, My reports history persistence, Worker configuration, and mutated copies that must fail'; Block = { Invoke-Helper 'test_crash_report.ps1' 'host' } },
    @{ Name = 'startup-diagnostics'; Scope = 'host'; Title = 'Startup stability diagnostics: shared-host startup stage progression across configuration, boot image, renderer, Vulkan instance/device, swapchain, modules, guest entry, audio and first present, last-stage crash diagnosis, DeviceLost exit log flush, and mutated copies that must fail'; Block = { Invoke-Helper 'test_startup_diagnostics.ps1' 'host' } },
    @{ Name = 'session-diagnostics'; Scope = 'host'; Title = 'Session diagnostics (SOS-VLHSWP): flushed Stage lines with durations and results for PLAY, its input source, game files recheck, graphics preflight, settings save, launcher teardown and runtime handoff; the never-killing stage watchdog; the system report (SteamOS, Gamescope, Gaming/Desktop Mode, memory, no paths); launcher input/focus telemetry and the Gamescope gamepad policy; the deferred graphics card probe; session outcomes shared with the dashboard; mutations'; Block = { Invoke-Helper 'test_session_diagnostics.ps1' 'host' } },
    @{ Name = 'package-release'; Scope = 'host'; Title = 'Release packaging failure paths on a copy of package-release.ps1 with stand-ins for the gate and the packager (nothing is built or packaged): a failed gate builds no package, a failed packaging step or checksum leaves nothing that looks like a release in dist, and a successful run still writes the package, its checksum and the notes'; Block = { Invoke-Helper 'test_package_release.ps1' 'host' } },
    @{ Name = 'progress-reporter'; Scope = 'host'; Title = 'Test progress: percentage, bar, durations, ETA, check counts, previous group, timing summary, redirected output and a synthetic long group (synthetic time, no sleeping)'; Block = { Invoke-Helper 'test_progress.ps1' 'host' } },
    @{ Name = 'git-whitespace'; Scope = 'host'; Title = 'git diff --check (staged, unstaged and new files)'; Block = { Invoke-GitWhitespace } }
)

if ($List) {
    foreach ($step in $steps) { Write-Host ('{0,-30} {1,-6} {2}' -f $step.Name, $step.Scope, $step.Title) }
    Write-Host "$($steps.Count) groups"
    exit 0
}
$patterns = @($Filter | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($patterns.Count -gt 0) {
    $steps = @($steps | Where-Object { $name = $_.Name; @($patterns | Where-Object { $name -like $_ }).Count -gt 0 })
    if ($steps.Count -eq 0) {
        Write-Host "No group matches -Filter $($patterns -join ', '); see -List." -ForegroundColor Red
        exit 1
    }
    Write-Host "Running $($steps.Count) group(s) matching -Filter $($patterns -join ', ')" -ForegroundColor Cyan
}

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
        $script:CurrentStep = $step
        Start-ProgressGroup $script:Progress $step.Name $script:Clock.Elapsed.TotalSeconds
        Write-Step ('[{0}] {1}' -f $(if ($step.Scope -eq 'build') { 'builds' } else { 'host' }), $step.Title)
        try { $result = & $step.Block }
        catch { $result = Result 'FAIL' "the group could not run: $($_.Exception.Message)" }
        Clear-ProgressLine $script:Progress
        $script:Outcomes.Add([pscustomobject]@{ Name = $step.Name; Scope = $step.Scope; Passed = ($result.State -eq 'PASS') })
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
if ($script:Skipped.Count -gt 0) {
    Write-Host "Skipped parts: $($script:Skipped.Count)" -ForegroundColor Yellow
    $script:Skipped | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}
if ($script:DataUnavailable.Count -gt 0) {
    Write-Host "Data unavailable (not a failure): $($script:DataUnavailable.Count)" -ForegroundColor Yellow
    $script:DataUnavailable | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}
$supported = @(Get-SupportedBuilds)
if ($supported.Count -gt 0) {
    Write-Host ''
    $summary = @(Format-BuildEvidenceSummary $supported $script:EvidenceRecords.ToArray() $script:Outcomes.ToArray())
    foreach ($line in $summary) {
        $color = if ($line -eq $summary[0]) { 'Cyan' } elseif ($line -match 'FAIL') { 'Red' } elseif ($line -match 'DATA UNAVAILABLE|NOT INSTALLED') { 'Yellow' } elseif ($line -match 'PASS|AVAILABLE') { 'Green' } else { 'Cyan' }
        Write-Host $line -ForegroundColor $color
    }
}
Write-Host "Failed: $failed"
if ($failed) { exit 1 }
exit 0
