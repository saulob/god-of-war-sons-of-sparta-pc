# Headless regression of how the release gate treats the local game data of the supported builds. Temporary synthetic folders stand
# for a machine with only 1.00, only 1.01, both, a research image alone, no prepared folder and no data at all (placeholder files, no
# game data): the image lookup of PowerShell, Python and the C++ tests must agree on each; every gate that reads the game must pass
# and name both builds when nothing is installed, a build without data must be DATA UNAVAILABLE in the run summary, never a pass,
# and a real failure must still be a failure. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$Ok, [string]$Name) {
    if ($Ok) { Write-Host "  [ok]   $Name" -ForegroundColor Green }
    else { Write-Host "  [fail] $Name" -ForegroundColor Red; $failures.Add($Name) }
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_build_evidence_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$savedRoot = $env:SOS_EVIDENCE_ROOT

$python = Get-Python
if (-not $python) { throw 'Python 3 was not found (tools\setup.ps1)' }
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"

function New-Machine([string]$Name) {
    $path = Join-Path $temp $Name
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return $path
}

function Add-Image([string]$Machine, [string]$Place, [string]$Folder, [string]$Sha, [switch]$NoBoot) {
    $directory = Join-Path $Machine "build\$Place\$Folder"
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    Set-Content -LiteralPath (Join-Path $directory 'image.json') -NoNewline -Value ('{' + "`n" + '  "eboot_sha256": "' + $Sha + '"' + "`n" + '}')
    if (-not $NoBoot) { Set-Content -LiteralPath (Join-Path $directory 'boot.bin') -NoNewline -Value 'placeholder' }
}

function Add-GameFolder([string]$Machine, [string]$Kind, [string]$Folder) {
    $directory = Join-Path $Machine "game\$Kind\$Folder"
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    Set-Content -LiteralPath (Join-Path $directory 'eboot.bin') -NoNewline -Value 'placeholder'
}

$sha100 = Get-BuildSha256 '1.00'
$sha101 = Get-BuildSha256 '1.01'
Check ($sha100 -match '^[0-9a-f]{64}$' -and $sha101 -match '^[0-9a-f]{64}$' -and $sha100 -ne $sha101) 'the two supported builds have distinct fingerprints'

$probe = Join-Path $temp 'probe.py'
Set-Content -LiteralPath $probe -Encoding ascii -Value @"
import sys
sys.path.insert(0, r'$root\scripts')
import build_evidence as e
for version in e.versions():
    boot, state = e.boot_image(version)
    print('image|%s|%s' % (e.key_of(version), state))
    print('content|%s|%s' % (e.key_of(version), e.content_state(version)))
"@

$cppSource = Join-Path $temp 'probe.cpp'
Set-Content -LiteralPath $cppSource -Encoding ascii -Value @'
#include <cstdio>
#include "build_images.h"
int main() {
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage boot = sos_test::FindBootImage("unused", key);
        std::printf("image|%s|%s\n", key, boot.found() ? boot.source : "NO DATA");
    }
    return 0;
}
'@
$cppProbe = Join-Path $temp 'probe.exe'
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-I' (Join-Path $root 'tests') $cppSource '-o' $cppProbe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the image lookup probe' }

# What each language reports for a machine: image|<build>|<state> and, from Python, content|<build>|<state>.
function Get-Lookups([string]$Machine) {
    $env:SOS_EVIDENCE_ROOT = $Machine
    $powerShell = foreach ($version in '1.00', '1.01') {
        $found = Find-BuildBootImage $version
        'image|{0}|{1}' -f (Get-BuildKey $version), $(if ($found) { $found.State } else { 'NO DATA' })
    }
    $pythonLines = @(& $python.Exe @($python.Args) -I $probe)
    $cpp = @(& $cppProbe)
    $env:SOS_EVIDENCE_ROOT = $savedRoot
    return [pscustomobject]@{ PowerShell = @($powerShell); Python = @($pythonLines | Where-Object { $_ -like 'image|*' }); Cpp = $cpp; Content = @($pythonLines | Where-Object { $_ -like 'content|*' }) }
}

function Test-Machine([string]$Name, [string]$Machine, [string]$Expect100, [string]$Expect101, [string[]]$Content = @()) {
    $lookups = Get-Lookups $Machine
    $expected = @("image|CUSA57767-1.00|$Expect100", "image|CUSA57767-1.01|$Expect101")
    Check (($lookups.PowerShell -join ';') -eq ($expected -join ';')) "${Name}: PowerShell finds $Expect100 / $Expect101"
    Check (($lookups.Python -join ';') -eq ($expected -join ';')) "${Name}: Python finds $Expect100 / $Expect101"
    Check (($lookups.Cpp -join ';') -eq ($expected -join ';')) "${Name}: the C++ tests find $Expect100 / $Expect101"
    if ($Content.Count) { Check (($lookups.Content -join ';') -eq (($Content | ForEach-Object { $_ }) -join ';')) "${Name}: game content is $($Content -join ', ')" }
}

try {
    Write-Step 'The image lookup on machines with different data'
    $none = New-Machine 'none'
    Test-Machine 'nothing installed' $none 'NO DATA' 'NO DATA' @('content|CUSA57767-1.00|NOT INSTALLED', 'content|CUSA57767-1.01|NOT INSTALLED')

    $only100 = New-Machine 'only100'
    Add-Image $only100 'image' 'CUSA57767-1.00' $sha100
    Add-GameFolder $only100 'prepared' 'CUSA57767-1.00'
    Test-Machine 'only 1.00' $only100 'REAL IMAGE' 'NO DATA' @('content|CUSA57767-1.00|AVAILABLE', 'content|CUSA57767-1.01|NOT INSTALLED')

    $only101 = New-Machine 'only101'
    Add-Image $only101 'image' 'CUSA57767-1.01' $sha101
    Add-GameFolder $only101 'prepared' 'CUSA57767-1.01'
    Test-Machine 'only 1.01' $only101 'NO DATA' 'REAL IMAGE' @('content|CUSA57767-1.00|NOT INSTALLED', 'content|CUSA57767-1.01|AVAILABLE')

    $legacy = New-Machine 'legacy'
    Add-Image $legacy 'image' 'v1.00' $sha100
    Add-Image $legacy 'image' 'v1.01' $sha101
    Test-Machine 'launcher folders of the older names' $legacy 'REAL IMAGE' 'REAL IMAGE'

    $both = New-Machine 'both'
    Add-Image $both 'image' 'CUSA57767-1.00' $sha100
    Add-Image $both 'image' 'CUSA57767-1.01' $sha101
    Add-GameFolder $both 'prepared' 'CUSA57767-1.00'
    Add-GameFolder $both 'prepared' 'CUSA57767-1.01'
    Test-Machine 'both installed' $both 'REAL IMAGE' 'REAL IMAGE' @('content|CUSA57767-1.00|AVAILABLE', 'content|CUSA57767-1.01|AVAILABLE')

    $research = New-Machine 'research'
    Add-Image $research 'research\image' 'CUSA57767-1.00' $sha100
    Add-Image $research 'research\image' 'CUSA57767-1.01' $sha101
    Test-Machine 'research images alone, no prepared folder' $research 'RESEARCH IMAGE' 'RESEARCH IMAGE' @('content|CUSA57767-1.00|NOT INSTALLED', 'content|CUSA57767-1.01|NOT INSTALLED')

    $mixed = New-Machine 'mixed'
    Add-Image $mixed 'image' 'CUSA57767-1.00' $sha100
    Add-Image $mixed 'research\image' 'CUSA57767-1.01' $sha101
    Test-Machine 'a real image and a research image' $mixed 'REAL IMAGE' 'RESEARCH IMAGE'

    $shadow = New-Machine 'shadow'
    Add-Image $shadow 'image' 'CUSA57767-1.01' $sha101
    Add-Image $shadow 'research\image' 'CUSA57767-1.01' $sha101
    Test-Machine 'the launcher image comes before the research copy' $shadow 'NO DATA' 'REAL IMAGE'

    $wrong = New-Machine 'wrong'
    Add-Image $wrong 'image' 'CUSA57767-1.01' $sha100
    Add-Image $wrong 'research\image' 'CUSA57767-1.00' $sha101
    Test-Machine 'an image of the other build is no evidence' $wrong 'NO DATA' 'NO DATA'

    $broken = New-Machine 'broken'
    Add-Image $broken 'image' 'CUSA57767-1.00' $sha100 -NoBoot
    Add-Image $broken 'image' 'CUSA57767-1.01' 'not a fingerprint'
    Test-Machine 'an image without boot.bin or with another fingerprint is no evidence' $broken 'NO DATA' 'NO DATA'

    $layers = New-Machine 'layers'
    Add-GameFolder $layers 'extracted' 'v1.00'
    Add-GameFolder $layers 'extracted' 'CUSA57767-1.01'
    Test-Machine 'extracted package layers only' $layers 'NO DATA' 'NO DATA' @('content|CUSA57767-1.00|EXTRACTED LAYERS', 'content|CUSA57767-1.01|EXTRACTED LAYERS')

    Write-Step 'Every gate that reads the game passes with no data at all and names both builds'
    $env:SOS_EVIDENCE_ROOT = $none
    $gates = 'build_parity_check', 'pit_unlock_check', 'image_glyphs', 'hermes_unlock_check', 'skip_intro_check', 'title_input_check', 'prompt_sites',
             'prompt_dialog', 'map_input_check', 'ultrawide_check', 'graphics_import_check', 'frame_model_check', 'avplayer_check', 'np_toolkit2_check',
             'prompt_assets_check', 'exit_game_sites'
    foreach ($gate in $gates) {
        $output = @(& $python.Exe @($python.Args) -I (Join-Path $root "scripts\$gate.py") 2>&1)
        $code = $LASTEXITCODE
        $text = $output -join "`n"
        $named = ($text -match 'CUSA57767[- ]1\.00') -and ($text -match 'CUSA57767[- ]1\.01')
        $lies = @($output | Where-Object { $_ -match '^EVIDENCE\|image\|[^|]*\|(REAL|RESEARCH) IMAGE\|' })
        Check ($code -eq 0 -and $named -and $lies.Count -eq 0) "${gate}: passes, names 1.00 and 1.01 and claims no image it did not read"
        Check (@($output | Where-Object { $_ -match '^\s*\[skip\]' }).Count -eq 0) "${gate}: reports no data as EVIDENCE, not as a skip"
    }
    $env:SOS_EVIDENCE_ROOT = $savedRoot

    Write-Step 'The C++ gates with no data run the contract of both builds and report the images as NO DATA'
    $env:SOS_EVIDENCE_ROOT = $none
    foreach ($helper in 'test_pit_unlock.ps1', 'test_image_glyphs.ps1', 'test_hermes_unlock.ps1') {
        # The Pit's mutations must be caught without a real image; the glyph ones always run apart from any image.
        if ($helper -eq 'test_pit_unlock.ps1') { Remove-Item Env:\SOS_TEST_NO_MUTATIONS -ErrorAction SilentlyContinue } else { $env:SOS_TEST_NO_MUTATIONS = '1' }
        $text = (& powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $helper) 2>&1 | Out-String)
        $code = $LASTEXITCODE
        $contract = [regex]::Matches($text, 'EVIDENCE\|contract\|CUSA57767-1\.0[01]\|SYNTHETIC CONTRACT').Count
        $both = ($text -match 'EVIDENCE\|image\|CUSA57767-1\.00\|NO DATA') -and ($text -match 'EVIDENCE\|image\|CUSA57767-1\.01\|NO DATA')
        Check ($code -eq 0 -and $both -and $text -notmatch '\|(REAL|RESEARCH) IMAGE\|') "${helper}: passes and reports both images as NO DATA"
        if ($helper -ne 'test_hermes_unlock.ps1') { Check ($contract -eq 2) "${helper}: the contract of 1.00 and 1.01 ran on synthetic data" }
        if ($helper -eq 'test_pit_unlock.ps1') { Check ($text -match 'Pit unlock mutations: (\d+) of (\d+) rejected' -and $Matches[1] -eq $Matches[2] -and [int]$Matches[1] -ge 14) "${helper}: every deliberately broken site is still rejected without an image" }
    }
    Remove-Item Env:\SOS_TEST_NO_MUTATIONS -ErrorAction SilentlyContinue
    $env:SOS_EVIDENCE_ROOT = $savedRoot

    Write-Step 'The run summary keeps the three states apart'
    $builds = 'CUSA57767-1.00', 'CUSA57767-1.01'
    $groups = @(
        [pscustomobject]@{ Name = 'pit-unlock'; Scope = 'build'; Passed = $true },
        [pscustomobject]@{ Name = 'image-glyphs'; Scope = 'build'; Passed = $true },
        [pscustomobject]@{ Name = 'launcher'; Scope = 'host'; Passed = $true })
    function New-Record([string]$Group, [string]$Kind, [string]$Build, [string]$State, [bool]$Passed = $true) {
        [pscustomobject]@{ Group = $Group; Scope = 'build'; Passed = $Passed; Kind = $Kind; Build = $Build; State = $State; Detail = '' }
    }
    $records = @(
        (New-Record 'pit-unlock' 'image' $builds[0] 'REAL IMAGE'), (New-Record 'image-glyphs' 'image' $builds[0] 'REAL IMAGE'),
        (New-Record 'pit-unlock' 'image' $builds[1] 'NO DATA'), (New-Record 'image-glyphs' 'image' $builds[1] 'NO DATA'),
        (New-Record 'pit-unlock' 'content' $builds[0] 'AVAILABLE'), (New-Record 'pit-unlock' 'content' $builds[1] 'NOT INSTALLED'))
    $summary = @(Format-BuildEvidenceSummary $builds $records $groups)
    $block100 = ($summary | Select-String -SimpleMatch -Pattern 'CUSA57767-1.00' -Context 0, 3 | ForEach-Object { $_.Context.PostContext }) -join "`n"
    $block101 = ($summary | Select-String -SimpleMatch -Pattern 'CUSA57767-1.01' -Context 0, 3 | ForEach-Object { $_.Context.PostContext }) -join "`n"
    Check ($block100 -match 'contract:\s+PASS' -and $block100 -match 'real image:\s+PASS' -and $block100 -match 'prepared content:\s+AVAILABLE') '1.00 with its image and folder: contract PASS, real image PASS, content AVAILABLE'
    Check ($block101 -match 'contract:\s+PASS' -and $block101 -match 'real image:\s+DATA UNAVAILABLE' -and $block101 -notmatch 'real image:\s+PASS' -and $block101 -notmatch 'real image:\s+FAIL' -and $block101 -match 'prepared content:\s+NOT INSTALLED') '1.01 without data: contract PASS, real image DATA UNAVAILABLE (neither PASS nor FAIL), content NOT INSTALLED'

    $mixedRecords = @((New-Record 'pit-unlock' 'image' $builds[1] 'RESEARCH IMAGE'), (New-Record 'image-glyphs' 'image' $builds[1] 'NO DATA'))
    $mixedBlock = ((Format-BuildEvidenceSummary @($builds[1]) $mixedRecords $groups) -join "`n")
    Check ($mixedBlock -match 'PASS in 1 groups \(RESEARCH IMAGE\); DATA UNAVAILABLE in 1 groups \(image-glyphs\)') 'a research image in one group and none in another is told apart'

    $failedGroups = @([pscustomobject]@{ Name = 'pit-unlock'; Scope = 'build'; Passed = $false }, [pscustomobject]@{ Name = 'image-glyphs'; Scope = 'build'; Passed = $true })
    $failedRecords = @((New-Record 'pit-unlock' 'image' $builds[0] 'REAL IMAGE' $false), (New-Record 'image-glyphs' 'image' $builds[0] 'REAL IMAGE'), (New-Record 'pit-unlock' 'image' $builds[1] 'NO DATA' $false))
    $failedText = ((Format-BuildEvidenceSummary $builds $failedRecords $failedGroups) -join "`n")
    Check ($failedText -match 'contract:\s+FAIL \(pit-unlock\)' -and $failedText -match 'real image:\s+FAIL \(pit-unlock\)') 'a failed group makes the contract and its real image FAIL'
    Check ($failedText -match 'real image:\s+DATA UNAVAILABLE') 'a failed group without a real image does not turn DATA UNAVAILABLE into a pass'

    Check ($null -eq (ConvertFrom-EvidenceLine '  [skip] CUSA57767 1.01: no boot image') -and $null -eq (ConvertFrom-EvidenceLine 'EVIDENCE|image|x')) 'only complete EVIDENCE lines are evidence'
    $line = ConvertFrom-EvidenceLine 'EVIDENCE|image|CUSA57767-1.01|NO DATA|not read: a | in the detail'
    Check ($line.Build -eq 'CUSA57767-1.01' -and $line.State -eq 'NO DATA' -and $line.Detail -eq 'not read: a | in the detail') 'an EVIDENCE line keeps its detail whole'
} finally {
    $env:SOS_EVIDENCE_ROOT = $savedRoot
    if ($null -eq $savedRoot) { Remove-Item Env:\SOS_EVIDENCE_ROOT -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) check(s) failed." -ForegroundColor Red
    exit 1
}
Write-Host 'All build evidence checks passed.'
exit 0
