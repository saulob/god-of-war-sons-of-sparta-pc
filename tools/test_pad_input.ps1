# Headless test of the pad the game reads, and of the controller kind it publishes for the prompts: the real src/runtime_pad.c on a fake SDL gamepad (Xbox A, B, X, Y, bumpers, Start, Back,
# stick clicks, d-pad, sticks and triggers; pressed, held and released states; any number of reads a frame at 30 to 240 Hz; Back as the
# Touchpad on every pad but the DualShock 4 and DualSense, with SOS_PAD_MAP precedence), and
# the same transcript from the baseline commit's runtime_pad.c, which must match line by line. No window, controller or audio
# device is touched.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

# The last commit before the graphics, frame rate, prompt and Skip Intro work; its pad is the reference.
$baseline = '5b47150'

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_pad_input_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clang = Join-Path $tc.Bin 'clang.exe'
$test = Join-Path $PSScriptRoot 'pad_input_tests.c'

function Build-Pad([string]$Source, [string]$Output, [string[]]$Define = @()) {
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' (Join-Path $root 'src') '-I' $PSScriptRoot `
        (Get-StringDefine 'PAD_SOURCE' $Source.Replace('\', '/')) @Define $test '-lpthread' '-o' $Output
    if ($LASTEXITCODE -ne 0) { throw "could not compile the pad test for $Source" }
}

$result = 0
$head = Join-Path $temp 'pad_head.exe'
Build-Pad (Join-Path $root 'src\runtime_pad.c') $head
$headLines = & $head
if ($LASTEXITCODE -ne 0) { $result = 1; $headLines | Where-Object { $_ -match 'FAIL' } | ForEach-Object { Write-Host $_ } }
Write-Host "  [pad] $($headLines | Select-Object -Last 1)"

$kindExe = Join-Path $temp 'pad_kind.exe'
Build-Pad (Join-Path $root 'src\runtime_pad.c') $kindExe @('-DPAD_KIND_TEST')
$kindLines = & $kindExe
if ($LASTEXITCODE -ne 0) { $result = 1; $kindLines | Where-Object { $_ -match 'FAIL' } | ForEach-Object { Write-Host $_ } }
Write-Host "  [kind] controller kind published for the prompts: $($kindLines | Select-Object -Last 1)"

$backExe = Join-Path $temp 'pad_back.exe'
Build-Pad (Join-Path $root 'src\runtime_pad.c') $backExe @('-DPAD_BACK_TEST')
$backLines = & $backExe
if ($LASTEXITCODE -ne 0) { $result = 1; $backLines | Where-Object { $_ -match 'FAIL' } | Select-Object -First 8 | ForEach-Object { Write-Host $_ } }
Write-Host "  [back] Xbox/generic Back is the Touchpad, DualShock 4/DualSense Share/Create is not; scePadRead and scePadReadState: $($backLines | Select-Object -Last 1)"
foreach ($map in 'touchpad=back', 'cross=back', 'touchpad=start') {
    $env:SOS_PAD_MAP = $map
    $mapLines = & $backExe $map
    $mapExit = $LASTEXITCODE
    Remove-Item Env:\SOS_PAD_MAP
    if ($mapExit -ne 0) { $result = 1; $mapLines | Where-Object { $_ -match 'FAIL' } | Select-Object -First 8 | ForEach-Object { Write-Host $_ } }
    Write-Host "  [map]  SOS_PAD_MAP=$map keeps its precedence: $($mapLines | Select-Object -Last 1)"
}

Push-Location $root
$null = git cat-file -e "$baseline^{commit}" 2>$null
$haveBaseline = $LASTEXITCODE -eq 0
if ($haveBaseline) {
    $reference = Join-Path $temp 'runtime_pad_baseline.c'
    [IO.File]::WriteAllText($reference, ((git show "${baseline}:src/runtime_pad.c") -join "`n") + "`n")
}
Pop-Location
if ($haveBaseline) {
    $baselineExe = Join-Path $temp 'pad_baseline.exe'
    Build-Pad $reference $baselineExe
    $baselineLines = & $baselineExe
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    $difference = Compare-Object @($headLines) @($baselineLines)
    if ($difference) { $result = 1; Write-Host '  [fail] the pad differs from the baseline commit:'; $difference | Select-Object -First 8 | ForEach-Object { Write-Host "         $($_.SideIndicator) $($_.InputObject)" } }
    else { Write-Host "  [ok]   the pad reads match commit $baseline on $($headLines.Count) lines" }
    $baselineBackExe = Join-Path $temp 'pad_baseline_back.exe'
    Build-Pad $reference $baselineBackExe @('-DPAD_BACK_TEST')
    $baselineBack = @(& $baselineBackExe | Where-Object { $_ -like 'read *' })
    $headBack = @($backLines | Where-Object { $_ -like 'read *' })
    $unexpected = @()
    $expected = 0
    if ($baselineBack.Count -ne $headBack.Count) { $unexpected += "line count $($headBack.Count) against $($baselineBack.Count)" }
    else {
        for ($i = 0; $i -lt $headBack.Count; ++$i) {
            if ($headBack[$i] -eq $baselineBack[$i]) { continue }
            if ($headBack[$i] -match '^read (DualShock4|DualSense)/\S*back\S* ' -and $baselineBack[$i] -match '^read (DualShock4|DualSense)/\S*back\S* ') { ++$expected }
            else { $unexpected += $headBack[$i] }
        }
    }
    if ($unexpected -or $expected -eq 0) { $result = 1; Write-Host "  [fail] the pad transcript differs from commit $baseline beyond DualShock 4/DualSense Back ($expected expected differences):"; $unexpected | Select-Object -First 8 | ForEach-Object { Write-Host "         $_" } }
    else { Write-Host "  [ok]   against commit $baseline only DualShock 4/DualSense Back differs: $expected of $($headBack.Count) lines" }
    Push-Location $root
    $changed = @(git diff --name-only $baseline HEAD -- src/runtime_pad.c src/runtime.c src/runtime_services.c src/runtime_sos.c src/runtime_users.c src/sos_keys.c gpu/shim/window.cpp gpu/shim/dcp_overlay.cpp)
    Pop-Location
    Write-Host ("  [info] input path files changed since {0}: {1}" -f $baseline, $(if ($changed.Count) { $changed -join ', ' } else { 'none' }))
}
else { Write-Host "  [skip] commit $baseline is not in this repository: the pad was not compared" }
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue

$python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if ($python) {
    & $python.Source -I (Join-Path $root 'scripts\map_input_check.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
else { Write-Host '  [skip] no Python: the Map action was not read from the game code' }
exit $result
