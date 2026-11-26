# Headless proof of the startup and session diagnostics added after the Steam Deck report SOS-VLHSWP: flushed stage
# lines with durations and results, the diagnostic watchdog (budgets, bounded reports, never for idle phases), the system
# report (Gaming/Desktop Mode, SteamOS, Gamescope, memory, privacy), launcher input telemetry and the Gamescope gamepad
# policy, the deferred graphics card probe and its marker, the session outcome rules shared with the dashboard
# (tests/fixtures/session_outcome_cases.txt) and the Bug Report reading the new lines; then source contracts on the
# production wiring (PLAY stages, launcher teardown, loader, crash handler) and mutated copies that must fail.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$failures = 0
function Report([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name"; $script:failures++ }
}

# --- 1. Source contracts on the production wiring ------------------------------------------------------------------
function Read-Source([string]$Relative) { [IO.File]::ReadAllText((Join-Path $root $Relative)).Replace("`r`n", "`n") }
$launcher = Read-Source 'launcher\launcher.cpp'
$host_ = Read-Source 'launcher\host.cpp'
$loader = Read-Source 'src\loader.c'
$session = Read-Source 'src\runtime_session_log.c'
$logPosix = Read-Source 'src\runtime_log_posix.c'

function Get-Body([string]$Text, [string]$Start) {
    $at = $Text.IndexOf($Start, [StringComparison]::Ordinal)
    if ($at -lt 0) { return '' }
    $end = $Text.IndexOf("`n    }`n", $at, [StringComparison]::Ordinal)
    if ($end -lt 0) { $end = $Text.IndexOf("`n}`n", $at, [StringComparison]::Ordinal) }
    return $Text.Substring($at, $end - $at)
}
$requestPlay = Get-Body $launcher 'void RequestPlay() {'
Report 'PLAY logs its input source before anything else' ($requestPlay.Contains('sos_startup_event("PLAY requested input=%s", InputKindName(host_.LastInput()));'))
Report 'PLAY logs the game files recheck, the graphics preflight and the settings save as stages with results' (
    $requestPlay.Contains('sos_startup_stage_result(kPlayRecheckStage') -and $requestPlay.Contains('sos_startup_stage_result(kPlayGraphicsStage') -and
    $requestPlay.Contains('sos_startup_stage_result(kPlaySaveStage'))
Report 'every PLAY refusal is logged as an event' (
    ([regex]::Matches($requestPlay, 'sos_startup_event\("PLAY blocked: ')).Count -ge 4)
Report 'TRG''s silent PLAY refusals are logged when the answer changes' ($launcher.Contains('NotePlayAvailability(check.ok ? std::string() : check.reason);'))
Report 'the footer names Start as PLAY' ($launcher.Contains('c.hints = kLauncherHints;'))
Report 'the launcher teardown is a stage and its end is recorded' (
    $launcher.Contains('sos_startup_phase("launcher renderer teardown");') -and $launcher.Contains('sos_startup_stage_result("launcher host closed", true'))
Report 'the Vulkan probe runs after the window has drawn frames, not before it is shown' (
    -not $launcher.Contains('gpu_names_ = DcpGraphics::GpuChoices(ListGpuNames());') -and $launcher.Contains('GraphicsProbeDue(++frames_shown, ImGui::GetTime() - shown_at)'))
Report 'the launcher host logs input events, focus, the SDL video driver and a system quit' (
    $host_.Contains('NoteInput("gamepad_button"') -and $host_.Contains('"touch_click"') -and $host_.Contains('NoteInput("finger_down"') -and $host_.Contains('NoteInput("focus_lost")') -and
    $host_.Contains('video driver %s') -and $host_.Contains('quit requested by the system'))
Report 'background gamepad events are allowed under Gamescope only, before the gamepad subsystem starts' (
    $host_.IndexOf('SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");') -ge 0 -and
    $host_.IndexOf('if (background_gamepad) {') -lt $host_.IndexOf('SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");') -and
    $host_.IndexOf('SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");') -lt $host_.IndexOf('gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);'))
$allSources = (Get-ChildItem -Path (Join-Path $root 'src'), (Join-Path $root 'launcher'), (Join-Path $root 'gpu\shim') -Include *.c, *.cpp -Recurse -File |
    ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"
Report 'stage lines never wait for the log file (no runtime_log_flush as the stage flush: phases change on frame threads)' (
    -not ($allSources -match 'sos_startup_set_flush\(\s*runtime_log_flush\s*\)'))
Report 'the loader starts the watchdog and logs the system before the launcher' (
    $loader.IndexOf('sos_startup_watchdog_start();') -ge 0 -and
    $loader.IndexOf('sos_system_report_log(SOS_PORT_VERSION)') -ge 0 -and
    $loader.IndexOf('sos_system_report_log(SOS_PORT_VERSION)') -lt $loader.IndexOf('const SosLauncherResult launched = sos_launcher_run(&launcher);'))
Report 'the loader logs the launcher result and the in-process runtime handoff' (
    $loader.Contains('sos_startup_event("Launcher result: %s", sos_launcher_result_name(launched));') -and
    $loader.Contains('sos_startup_event("Runtime handoff: the game starts in this process");'))
# Log 192: the launcher result was named by array position (PLAY logged as quit).
Report 'the launcher result is named by value, never by its position in a table' (-not ($loader -match 'launcher_results\s*\['))
# Log 192: the destructor's second Close logged the whole teardown again.
$closeAt = $host_.IndexOf('void LauncherHost::Close() {', [StringComparison]::Ordinal)
$closeBody = if ($closeAt -ge 0) { $host_.Substring($closeAt, [Math]::Min(600, $host_.Length - $closeAt)) } else { '' }
Report 'a second Close (the destructor after an explicit Close) logs nothing' (
    $closeBody.IndexOf('if (!window_ && !renderer_ && !imgui_ && !video_ && !gamepad_ && textures_.empty()) {', [StringComparison]::Ordinal) -ge 0 -and
    $closeBody.IndexOf('if (!window_ && !renderer_', [StringComparison]::Ordinal) -lt $closeBody.IndexOf('sos_startup_phase("launcher host closing");', [StringComparison]::Ordinal))
# Log 192: the loading screen's present set "first present" before guest entry, so the game ran under "guest entry".
$watch = Read-Source 'gpu\shim\dcp_startup_watch.cpp'
Report 'the first game frame (not the loading screen) ends the start: phase "game running"' (
    $watch.Contains("        if (stage == Present) {`n            sos_startup_milestone(""first game frame presented"");`n            sos_startup_phase(""game running"");"))
Report 'a clean exit writes the closing record a Bug Report looks for' ($session.Contains('fputs("Session: normal exit\n", stdout);'))
Report 'the console copy of the log never blocks the log file (private non-blocking descriptor, never the shared one)' (
    $logPosix.Contains('O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY') -and $logPosix.Contains('/proc/self/fd/%d') -and
    -not ($logPosix -match 'fcntl\(echo_target, F_SETFL'))

# --- 2. Behaviour: the headless test on workspace copies, then mutations -------------------------------------------
$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_session_diag_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clang = Join-Path $tc.Bin 'clang.exe'
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $target = '--target=x86_64-w64-mingw32'
    $cFlags = @($target, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1')
    $cxxFlags = @($target, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1')

    $copyFiles = @(
        'launcher/bug_report.h', 'launcher/bug_report.cpp', 'launcher/session_outcome.h', 'launcher/graphics_probe.h',
        'launcher/launcher_input.h', 'launcher/sos_launcher.h', 'launcher/log_scrubber.h', 'launcher/log_scrubber.cpp', 'launcher/game_profile.h',
        'launcher/game_profile.cpp', 'launcher/sos_resolution.h', 'launcher/sos_resolution_sites.h', 'launcher/utf8.h',
        'launcher/localization/ui_text.h', 'launcher/localization/ui_text.cpp', 'src/startup_phase.h', 'src/startup_phase.c',
        'src/system_report.h', 'src/system_report.c', 'src/game_build.h', 'src/net/json.h', 'src/net/json.cpp',
        'tests/fixtures/session_outcome_cases.txt'
    )
    $newWorkspace = {
        param([string]$Dir)
        foreach ($rel in $copyFiles) { Copy-ToWorkspace $Dir (Join-Path $root $rel) $rel | Out-Null }
    }
    $buildIn = {
        param([string]$Dir, [string]$Exe, [string[]]$Extra)
        $inc = @('-I', (Join-Path $Dir 'launcher'), '-I', (Join-Path $Dir 'src'), '-I', (Join-Path $prefix 'include'))
        $objs = New-Object System.Collections.Generic.List[string]
        foreach ($cSrc in @('src/startup_phase.c', 'src/system_report.c')) {
            $obj = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($cSrc) + '.o')
            & $clang @cFlags @Extra @inc '-c' (Join-Path $Dir $cSrc) '-o' $obj
            if ($LASTEXITCODE -ne 0) { return }
            $objs.Add($obj)
        }
        $dirText = $Dir.Replace('\', '/')
        $cxxSources = @(
            (Join-Path $root 'tests/session_diagnostics_tests.cpp'),
            (Join-Path $Dir 'launcher/bug_report.cpp'),
            (Join-Path $Dir 'launcher/log_scrubber.cpp'),
            (Join-Path $Dir 'launcher/game_profile.cpp'),
            (Join-Path $Dir 'launcher/localization/ui_text.cpp'),
            (Join-Path $Dir 'src/net/json.cpp')
        )
        foreach ($src in $cxxSources) {
            $obj = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
            & $clangxx @cxxFlags @Extra (Get-StringDefine 'SOS_SOURCE_DIR' $dirText) @inc '-c' $src '-o' $obj
            if ($LASTEXITCODE -ne 0) { return }
            $objs.Add($obj)
        }
        & $clangxx $target '-o' $Exe @($objs) '-ldbghelp'
    }

    $base = Join-Path $temp 'base'
    & $newWorkspace $base
    $exe = Join-Path $base 'session_diagnostics_tests.exe'
    & $buildIn $base $exe @()
    if ($LASTEXITCODE -ne 0) { throw 'could not compile session_diagnostics_tests' }
    $run = Invoke-TimedProgram $exe
    Write-Host $run.Text.TrimEnd()
    if ($run.Code -ne 0) { $failures++ }

    $mutations = @(
        @{ Name = 'stage lines are no longer flushed'; File = 'src/startup_phase.c'; Old = "    if (flush) flush();`n    else fflush(stdout);"; New = '    (void)flush;' },
        @{ Name = 'setting the same phase again logs a line'; File = 'src/startup_phase.c'; Old = 'if (seen == phase || (seen && !strcmp(seen, phase))) return;'; New = 'if (0) return;' },
        @{ Name = 'the launcher has no line share of its own'; File = 'src/startup_phase.c'; Old = 'if (launcher_phase(phase) && __atomic_add_fetch(&launcher_stage_lines, 1, __ATOMIC_RELAXED) > MAX_LAUNCHER_STAGE_LINES)'; New = 'if (0)' },
        @{ Name = 'the launcher result is named by position again (PLAY logged as quit)'; File = 'launcher/sos_launcher.h'; Old = 'case SOS_LAUNCHER_PLAY: return "play";'; New = 'case SOS_LAUNCHER_PLAY: return "quit";' },
        @{ Name = 'play after the first game frame is timed by the watchdog'; File = 'src/startup_phase.c'; Old = '{"game running", 0}, '; New = '' },
        @{ Name = 'the loading screen counts as the start of play again'; File = 'launcher/session_outcome.h'; Old = 'if (line == "Runtime: first present" || line.starts_with("Stage: game running started")) first_present = true;'; New = 'if (line.starts_with("Startup: first present") || line == "Runtime: first present") first_present = true;' },
        @{ Name = 'the first-run warm-up gets a watchdog budget'; File = 'src/startup_phase.c'; Old = '{"pipeline cache warm-up", 0}'; New = '{"pipeline cache warm-up", 45}' },
        @{ Name = 'the idle launcher gets a watchdog budget'; File = 'src/startup_phase.c'; Old = '{"launcher running", 0}'; New = '{"launcher running", 30}' },
        @{ Name = 'watchdog reports are no longer bounded'; File = 'src/startup_phase.c'; Old = 'if (!budget || *reports >= 4) return 0;'; New = 'if (!budget) return 0;' },
        @{ Name = 'a failed stage becomes the last completed one'; File = 'src/startup_phase.c'; Old = '        printf("Stage: %s failed%s%s%s\n"'; New = '        __atomic_store_n(&completed_phase, stage ? stage : "", __ATOMIC_RELEASE);' + "`n" + '        printf("Stage: %s failed%s%s%s\n"' },
        @{ Name = 'a path-like environment value is logged'; File = 'src/system_report.c'; Old = "*c == '_' || *c == '-' || *c == ':' || *c == ',';"; New = "*c == '_' || *c == '-' || *c == ':' || *c == ',' || *c == '/';" },
        @{ Name = 'Gaming Mode is not told from a nested Gamescope'; File = 'src/system_report.c'; Old = 'facts.gaming_mode = facts.gamescope && ('; New = 'facts.gaming_mode = facts.gamescope || (' },
        @{ Name = 'background gamepad events allowed on every desktop'; File = 'launcher/launcher_input.h'; Old = 'return current_desktop && std::strstr(current_desktop, "gamescope") != nullptr;'; New = 'return true;' },
        @{ Name = 'input lines are no longer bounded'; File = 'launcher/launcher_input.h'; Old = 'if (++count > kLinesPerEvent) {'; New = 'if (++count > 1000000) {' },
        @{ Name = 'an unfinished probe marker is never cleared'; File = 'launcher/graphics_probe.h'; Old = "        std::filesystem::remove(marker, ec);`n        cards.state = GraphicsProbeState::kInterrupted;"; New = '        cards.state = GraphicsProbeState::kInterrupted;' },
        @{ Name = 'a log that just stops is called a crash'; File = 'launcher/session_outcome.h'; Old = '    if (crash || fatal_stop) {'; New = '    if (crash || fatal_stop || !closed_record) {' },
        @{ Name = 'a failed graphics preflight reads as a plain refusal'; File = 'launcher/session_outcome.h'; Old = 'if (blocked.starts_with("graphics preflight")) return with("graphics_preflight_failed", blocked);'; New = '' },
        @{ Name = 'the Bug Report ignores stage lines'; File = 'launcher/bug_report.cpp'; Old = 'if (started != std::string_view::npos && started > 0) {'; New = 'if (false) {' },
        @{ Name = 'the preview loses the session outcome'; File = 'launcher/bug_report.cpp'; Old = "        if (field.key == kLogTextKey) {`n            // Derived"; New = "        if (false) {`n            // Derived" },
        @{ Name = 'a failed installation reads as a launcher left without PLAY'; File = 'launcher/session_outcome.h'; Old = 'if (install.failed && !play_starting) return with("install_failed", install.Detail());'; New = '' },
        @{ Name = 'install attempts are not counted'; File = 'launcher/session_outcome.h'; Old = '            attempts = seen;'; New = '' },
        @{ Name = 'a shared case changed (the dashboard and the launcher disagree)'; File = 'tests/fixtures/session_outcome_cases.txt'; Old = "expect: graphics_preflight_failed"; New = "expect: play_blocked" }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        & $newWorkspace $work
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutExe = Join-Path $work 'session_diagnostics_tests.exe'
        $build = { & $buildIn $work $mutExe @('-w') }.GetNewClosure()
        if (Test-MutationDetected $mutation.Name $build $mutExe) { $detected++ } else { $failures++ }
    }
    Write-Host "session-diagnostics mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $failures
