# Headless test of the patch combinations a launch can produce (Controller prompts Automatic, PlayStation or Xbox, Skip Intro off or on,
# 60 and 90 FPS) on the real boot images of CUSA57767 1.00 and 1.01, and the static proof of the title's input path
# (scripts/title_input_check.py). No game, launcher, window or audio device is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_title_patches_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clang = Join-Path $tc.Bin 'clang.exe'
$launcher = Join-Path $root 'launcher'
$src = Join-Path $root 'src'
$objects = @()
foreach ($unit in @(@{ Source = (Join-Path $launcher 'controller_prompts.c'); Include = $launcher }, @{ Source = (Join-Path $src 'sos_skip_intro.c'); Include = $src })) {
    $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($unit.Source) + '.o')
    & $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-O1' '-I' $unit.Include '-I' $src '-c' $unit.Source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $($unit.Source)" }
    $objects += $object
}
$exe = Join-Path $temp 'title_patch_matrix_tests.exe'
$rootText = $root.Replace('\', '/')
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) `
    '-I' $launcher '-I' $src '-I' (Join-Path $root 'gpu\shim') (Join-Path $root 'tests\title_patch_matrix_tests.cpp') (Join-Path $launcher 'game_profile.cpp') @objects '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the title patch matrix test' }
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue

$python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if ($python) {
    & $python.Source -I (Join-Path $root 'scripts\title_input_check.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
else { Write-Host '  [skip] no Python: the title input path was not read from the game code' }
exit $result
