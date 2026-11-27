# SOS-PLTEYS (SteamOS, Steam Deck, v0.1.0, build 1.00, log ending after "semaphore id=2").
# 1. The reporter's log on the production session rules, against the issue #5 and #6 logs (build 1.01).
# 2. Each supported build imports the stack bound exports IL2CPP reads (real image when present, else DATA UNAVAILABLE).
# 3. Under WSL: a SIGSEGV right after "semaphore id=2" through the real log pipe writes the CRASH report.
# 4. The issue #5 fix itself: the linux-main-stack test (WSL bounds, loader gates, mutations).
# Mutated copies must fail. No game, launcher, window or GPU; the repository and the game data are only read.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')
. (Join-Path $PSScriptRoot '_evidence.ps1')

$failures = 0
function Report([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name"; $script:failures++ }
}

# --- 1. the log on the production session rules ---
$outcome = 'launcher/session_outcome.h'
$fixture = 'tests/fixtures/SOS_PLTEYS_v010_steamos_log.txt'
$mutations = @(
    @{ Name = 'a game that was handed off is read as a launcher-only session'; File = $outcome; Old = 'const bool game = handoff || game_child || first_present;'; New = 'const bool game = game_child || first_present;' },
    @{ Name = 'a log that stops while starting is read as a clean exit'; File = $outcome; Old = '    if (normal_exit) return with("normal_exit", where);'; New = '    if (normal_exit || !first_present) return with("normal_exit", where);' },
    @{ Name = 'a crash report is not read as a crash'; File = $outcome; Old = 'const bool fatal_line = line.starts_with("CRASH:") || line.starts_with("STOP: ");'; New = 'const bool fatal_line = line.starts_with("STOP: ");' },
    @{ Name = 'the fixture without semaphore id=2 still classified as the issue #5 end'; File = $fixture; Old = "Runtime: semaphore id=2, attr=0, initial=0, max=256`n"; New = '' },
    @{ Name = 'the fixture with a different line in the issue #5 tail'; File = $fixture; Old = 'Runtime: dlsym handle 0 scriptingGetMem -> 0x801411d40'; New = 'Runtime: dlsym handle 0 scriptingGetMem -> 0x801411d48' }
)
$code = Invoke-HostMutationTest -Name 'sos_plteys' -Root $root -Test 'tests/sos_plteys_tests.cpp' `
    -Copy @($outcome, $fixture, 'tests/fixtures/issue_5_v010_steam_deck_tail_log.txt', 'tests/fixtures/issue_6_v010_linux_intel_arl_log.txt') `
    -IncludeDirs @('launcher') -Mutations $mutations
if ($code -ne 0) { $failures++ }

# --- 2. the stack bound imports of each build ---
$names = [IO.File]::ReadAllText((Join-Path $root 'src\import_names.inc'))
$stackApis = @('scePthreadAttrGet', 'scePthreadAttrGetstackaddr', 'scePthreadAttrGetstacksize')
$nids = @{}
foreach ($api in $stackApis) {
    $match = [regex]::Match($names, '\{"([^"]+#libkernel)","' + $api + '"\}')
    if ($match.Success) { $nids[$api] = $match.Groups[1].Value }
}
Report 'the IL2CPP stack bound exports have libkernel NIDs in the import table' ($nids.Count -eq $stackApis.Count)
foreach ($version in @('1.00', '1.01')) {
    $image = Find-BuildBootImage $version
    if (-not $image) {
        Write-Evidence 'image' $version 'NO DATA' 'no boot image; not read: its stack bound imports'
        Write-Host "  [data] CUSA57767 $version image unavailable: its imports were not read (DATA UNAVAILABLE)"
        continue
    }
    $manifest = Get-Content -LiteralPath (Join-Path (Split-Path $image.Path -Parent) 'image.json') -Raw | ConvertFrom-Json
    $missing = @($stackApis | Where-Object { $manifest.import_list -notcontains $nids[$_] })
    Write-Evidence 'image' $version $image.State 'image.json: scePthreadAttrGet/Getstackaddr/Getstacksize imports'
    Report "CUSA57767 $version imports scePthreadAttrGet, Getstackaddr and Getstacksize ($($manifest.import_list.Count) imports)" ($missing.Count -eq 0)
}

# --- 3. a fatal signal after "semaphore id=2", through the real log pipe (WSL) ---
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$gcc = $false
if ($wsl) { $gcc = ((& wsl.exe -e sh -c 'command -v gcc >/dev/null 2>&1 && echo gcc' 2>$null) -join '').Trim() -eq 'gcc' }
if (-not $gcc) {
    Write-Host '  [n/a]  fatal signal after semaphore id=2: no WSL with gcc (not applicable, not a failure)'
} else {
    function Convert-ToWslPath([string]$Path) {
        return ((& wsl.exe -e wslpath -a ($Path.Replace('\', '/'))) | Select-Object -First 1).Trim()
    }
    $parent = 'E:\ClaudeTemp\opencode'
    if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
    $parent = [IO.Path]::GetFullPath($parent)
    $temp = [IO.Path]::GetFullPath((Join-Path $parent ('sos_plteys_' + [guid]::NewGuid().ToString('N'))))
    if (-not $temp.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $temp.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "refusing scratch folder $temp" }
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        $sources = @('src/fault_signal.h', 'src/fault_signal_posix.c', 'src/runtime_log.h', 'src/runtime_log_posix.c',
            'src/runtime_session_log.c', 'src/startup_phase.h', 'src/startup_phase.c', 'tests/fault_signal_tests.c')
        function Invoke-FaultCase([string]$Dir) {
            $dirWsl = Convert-ToWslPath $Dir
            $exe = '/tmp/sos_plteys_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
            $script = "cd '$dirWsl' && gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I src tests/fault_signal_tests.c src/fault_signal_posix.c src/runtime_log_posix.c src/runtime_session_log.c src/startup_phase.c -o '$exe' || exit 97; '$exe' --only issue6; code=`$?; rm -f '$exe'; exit `$code"
            $output = & wsl.exe -e sh -c $script 2>&1
            return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($output) }
        }
        $base = Join-Path $temp 'base'
        foreach ($relative in $sources) { Copy-ToWorkspace $base (Join-Path $root $relative) $relative | Out-Null }
        $run = Invoke-FaultCase $base
        $run.Lines | ForEach-Object { Write-Host "  $_" }
        if ($run.Code -eq 97) { throw 'the fault signal test did not compile under WSL' }
        Report 'SIGSEGV after semaphore id=2: CRASH, startup phase, last stage, checkpoint and thread reach the log file' ($run.Code -eq 0)

        $work = Join-Path $temp 'mut'
        foreach ($relative in $sources) { Copy-ToWorkspace $work (Join-Path $root $relative) $relative | Out-Null }
        Edit-WorkspaceText (Join-Path $work 'src/fault_signal_posix.c') '{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}' '{SIGBUS, SIGILL, SIGFPE, SIGABRT}'
        $mutated = Invoke-FaultCase $work
        Report 'mutation detected: the Linux fatal diagnostics without SIGSEGV (v0.1.0 silence)' ($mutated.Code -ne 0 -and $mutated.Code -ne 97)
    } finally {
        if ($temp.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# --- 4. the issue #5 fix ---
& (Join-Path $PSScriptRoot 'test_linux_main_stack.ps1')
if ($LASTEXITCODE -ne 0) { $failures++ }

exit $failures
