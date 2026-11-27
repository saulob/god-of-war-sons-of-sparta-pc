# SOS-4ECCUG (SteamOS, Steam Deck, v0.1.0, build 1.01, log ending after "semaphore id=2").
# 1. The reporter's log on the production session rules, against issue #5, SOS-PLTEYS and issue #6.
# 2. The real 1.01 image is the reporter's eboot and imports the stack bound exports IL2CPP reads.
# 3. The SOS-PLTEYS group: SIGSEGV -> CRASH under WSL and the issue #5 fix (linux-main-stack).
# Mutated copies must fail. No game, launcher, window or GPU; the repository and the game data are only read.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')
. (Join-Path $PSScriptRoot '_evidence.ps1')

$failures = 0
function Report([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name"; $script:failures++ }
}

# --- 1. the log on the production session rules ---
$outcome = 'launcher/session_outcome.h'
$fixture = 'tests/fixtures/SOS_4ECCUG_v010_steamos_log.txt'
$mutations = @(
    @{ Name = 'a game that was handed off is read as a launcher-only session'; File = $outcome; Old = 'const bool game = handoff || game_child || first_present;'; New = 'const bool game = game_child || first_present;' },
    @{ Name = 'a log that stops while starting is read as a clean exit'; File = $outcome; Old = '    if (normal_exit) return with("normal_exit", where);'; New = '    if (normal_exit || !first_present) return with("normal_exit", where);' },
    @{ Name = 'a crash report is not read as a crash'; File = $outcome; Old = 'const bool fatal_line = line.starts_with("CRASH:") || line.starts_with("STOP: ");'; New = 'const bool fatal_line = line.starts_with("STOP: ");' },
    @{ Name = 'the fixture without semaphore id=2 still classified as the issue #5 end'; File = $fixture; Old = "Runtime: semaphore id=2, attr=0, initial=0, max=256`n"; New = '' },
    @{ Name = 'the fixture no longer a build 1.01 run'; File = $fixture; Old = 'Linux x86-64, game build CUSA57767 1.01 USA'; New = 'Linux x86-64, game build CUSA57767 1.00 USA' },
    @{ Name = 'the fixture past the issue #5 fix (guest main stack line)'; File = $fixture; Old = "Runtime: Unity bootstrap entered`n"; New = "Runtime: Unity bootstrap entered`nRuntime: guest main stack 0x2000000-0x2800000`n" }
)
$code = Invoke-HostMutationTest -Name 'sos_4eccug' -Root $root -Test 'tests/sos_4eccug_tests.cpp' `
    -Copy @($outcome, $fixture, 'tests/fixtures/issue_5_v010_steam_deck_tail_log.txt', 'tests/fixtures/SOS_PLTEYS_v010_steamos_log.txt',
        'tests/fixtures/issue_6_v010_linux_intel_arl_log.txt') `
    -IncludeDirs @('launcher') -Mutations $mutations
if ($code -ne 0) { $failures++ }

# --- 2. the real 1.01 image ---
$names = [IO.File]::ReadAllText((Join-Path $root 'src\import_names.inc'))
$stackApis = @('scePthreadAttrGet', 'scePthreadAttrGetstackaddr', 'scePthreadAttrGetstacksize')
$nids = @($stackApis | ForEach-Object {
    $match = [regex]::Match($names, '\{"([^"]+#libkernel)","' + $_ + '"\}')
    if ($match.Success) { $match.Groups[1].Value }
})
Report 'the IL2CPP stack bound exports have libkernel NIDs in the import table' ($nids.Count -eq $stackApis.Count)
function Test-StackImports([string[]]$Imports) { return @($nids | Where-Object { $Imports -notcontains $_ }).Count -eq 0 }

$reported = ([regex]::Match([IO.File]::ReadAllText((Join-Path $root $fixture)), 'Eboot SHA-256: ([0-9a-f]{64})')).Groups[1].Value
$image = Find-BuildBootImage '1.01'
if (-not $image) {
    Write-Evidence 'image' '1.01' 'NO DATA' 'no boot image; not read: its stack bound imports'
    Write-Host '  [data] CUSA57767 1.01 image unavailable: its imports were not read (DATA UNAVAILABLE)'
} else {
    $manifest = Get-Content -LiteralPath (Join-Path (Split-Path $image.Path -Parent) 'image.json') -Raw | ConvertFrom-Json
    $imports = @($manifest.import_list)
    Write-Evidence 'image' '1.01' $image.State 'image.json: eboot SHA-256 and scePthreadAttrGet/Getstackaddr/Getstacksize imports'
    Report "the 1.01 image is the reporter's eboot ($reported)" ($manifest.eboot_sha256 -eq $reported -and $reported.Length -eq 64)
    Report "CUSA57767 1.01 imports scePthreadAttrGet, Getstackaddr and Getstacksize ($($imports.Count) imports)" (Test-StackImports $imports)
    Report 'mutation detected: a 1.01 import list without scePthreadAttrGetstackaddr fails' (-not (Test-StackImports @($imports | Where-Object { $_ -ne $nids[1] })))
}

# --- 3. SIGSEGV -> CRASH and the issue #5 fix, shared with SOS-PLTEYS ---
& (Join-Path $PSScriptRoot 'test_sos_plteys.ps1')
if ($LASTEXITCODE -ne 0) { $failures++ }

exit $failures
