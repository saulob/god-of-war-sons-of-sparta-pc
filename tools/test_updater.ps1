# Headless updater tests: versions, release JSON, SHA-256, ZIP safety, staging, jobs and rollback; the update controller
# against a scripted loopback HTTP server (never GitHub); the trg-updater helper on a temporary install folder. The tests
# are compiled directly with clang++ and start only console helper processes: no game, launcher, window or network.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$launcher = Join-Path $root 'launcher'
$updater = Join-Path $launcher 'updater'
$tests = Join-Path $root 'tests'
$trg = Join-Path $root 'third_party\trg-launcher'
if ($IsLinux -or $IsMacOS) {
    Write-Host '  [skip] the updater tests are built for Windows'
    exit 0
}
if (-not (Test-Path -LiteralPath (Join-Path $trg 'src\download.cpp'))) {
    Write-Host '  [skip] TRG Launcher is not fetched yet: run tools\build.ps1 once'
    exit 0
}

$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw),
    'project\(sons_of_sparta_pc VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'CMakeLists.txt has no project version' }

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_updater_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clang = Join-Path $tc.Bin 'clang.exe'
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$target = '--target=x86_64-w64-mingw32'

function Slash([string]$Path) { return $Path.Replace('\', '/') }

$exeUpdater = Join-Path $temp 'trg-updater.exe'
$exeMarker = Join-Path $temp 'update_restart_marker.exe'
$config = Join-Path $temp 'test_config.h'
@(
    "#define SOS_SOURCE_DIR `"$(Slash $root)`"",
    "#define SOS_UPDATER_SRC_DIR `"$(Slash $launcher)`"",
    "#define SOS_PORT_VERSION `"$version`"",
    "#define SOS_UPDATER_EXE `"$(Slash $exeUpdater)`"",
    "#define SOS_MARKER_EXE `"$(Slash $exeMarker)`""
) | Set-Content -LiteralPath $config -Encoding ASCII

function Compile-Object([string]$Source, [string]$Standard = 'c++20') {
    $object = Join-Path $temp (([IO.Path]::GetFileNameWithoutExtension($Source)) + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.o')
    & $clangxx $target "-std=$Standard" '-Wall' '-Wextra' '-D_CRT_SECURE_NO_WARNINGS' '-include' $config `
        '-I' $launcher '-I' (Join-Path $trg 'include') '-c' $Source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $Source" }
    return $object
}

function Link-Test([string]$Output, [string[]]$Objects, [string[]]$Arguments = @()) {
    & $clangxx $target $Objects $Arguments '-pthread' '-o' $Output
    if ($LASTEXITCODE -ne 0) { throw "could not link $Output" }
}

$failures = New-Object System.Collections.Generic.List[string]
function Run-Test([string]$Name, [string]$Exe, [string[]]$Arguments = @()) {
    Write-Host "==> $Name"
    & $Exe @Arguments
    if ($LASTEXITCODE -eq 0) { Write-Host "  [ok]   $Name" -ForegroundColor Green }
    else { Write-Host "  [fail] $Name (exit $LASTEXITCODE)" -ForegroundColor Red; $failures.Add($Name) }
}

try {
    $miniz = Join-Path $temp 'miniz.o'
    & $clang $target '-std=gnu11' '-w' '-c' (Join-Path $updater 'third_party\miniz\miniz.c') '-o' $miniz
    if ($LASTEXITCODE -ne 0) { throw 'could not compile miniz' }

    $core = 'github_release', 'install_transaction', 'json', 'sha256', 'update_job', 'update_paths', 'update_platform', 'update_zip', 'version' |
        ForEach-Object { Compile-Object (Join-Path $updater "$_.cpp") }
    $controller = Compile-Object (Join-Path $updater 'update_controller.cpp')
    $updateConfig = Compile-Object (Join-Path $launcher 'update_config.cpp')
    $trgCore = 'settings', 'platform', 'download' | ForEach-Object { Compile-Object (Join-Path $trg "src\$_.cpp") 'c++17' }

    $exeTests = Join-Path $temp 'sos-update-tests.exe'
    Link-Test $exeTests (@(Compile-Object (Join-Path $tests 'update_tests.cpp')) + $core + $miniz) @('-lshell32')
    Run-Test 'updater (no network)' $exeTests

    $exeNetwork = Join-Path $temp 'sos-update-network-tests.exe'
    Link-Test $exeNetwork (@(Compile-Object (Join-Path $tests 'update_network_tests.cpp')) + $updateConfig + $controller + $core + $trgCore + $miniz) `
        @('-lshell32', '-lws2_32', '-lwinhttp', '-lole32', '-lcomdlg32')
    Run-Test 'updater on a loopback server' $exeNetwork

    Link-Test $exeUpdater (@(Compile-Object (Join-Path $updater 'helper_main.cpp')) + $core[1] + $core[3] + $core[4] + $core[5] + $core[6]) `
        @('-static', '-mwindows', '-lshell32')
    Link-Test $exeMarker @(Compile-Object (Join-Path $tests 'update_restart_marker.cpp')) @('-static', '-mwindows', '-lshell32')
    $exeHelper = Join-Path $temp 'sos-update-helper-tests.exe'
    Link-Test $exeHelper @(Compile-Object (Join-Path $tests 'update_helper_tests.cpp')) @('-lshell32')
    Run-Test 'update helper' $exeHelper
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) failed: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'Updater tests passed.' -ForegroundColor Green
