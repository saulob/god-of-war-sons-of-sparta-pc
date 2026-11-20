# Headless test of the Controller prompts: the setting and its three styles (Automatic, PlayStation, Xbox), the verified sites of
# CUSA57767 1.00 and 1.01, the Xbox patch and the Automatic hook on a synthetic image and, when the boot images are installed, on
# the real code and the game's own data. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_prompts_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$launcher = Join-Path $root 'launcher'
$src = Join-Path $root 'src'
$objects = @()
foreach ($unit in @(@{ Source = (Join-Path $launcher 'controller_prompts.c'); Include = $launcher }, @{ Source = (Join-Path $src 'sos_skip_intro.c'); Include = $src })) {
    $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($unit.Source) + '.o')
    & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-O1' '-I' $launcher '-I' $src '-c' $unit.Source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $($unit.Source)" }
    $objects += $object
}
$exe = Join-Path $temp 'test_controller_prompts.exe'
$rootText = $root.Replace('\', '/')
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' "-DSOS_SOURCE_DIR=`"$rootText`"" '-I' $launcher '-I' $src `
    (Join-Path $root 'tests\controller_prompts_tests.cpp') @objects '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the controller prompts test' }

& $exe
$result = $LASTEXITCODE

$python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if ($python) {
    & $python.Source -I (Join-Path $root 'scripts\prompt_assets_check.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    & $python.Source -I (Join-Path $root 'scripts\prompt_sites.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
else { Write-Host '  [skip] no Python: the prompt assets and the game code were not read' }
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
