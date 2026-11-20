# Headless checks of Skip Intro: the setting, the build gating, the patch set on the real boot images of CUSA57767 1.00 and
# 1.01 (when installed), the wait for the save system, the loader wiring, the IL2CPP proof of the patched coroutine
# (scripts/skip_intro_check.py), then the player itself (every source plays completely) through tools/test_avplayer.ps1.
# No game, launcher, window or audio device is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_skip_intro_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$src = Join-Path $root 'src'
$object = Join-Path $temp 'sos_skip_intro.o'
& (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-O1' '-I' $src '-c' (Join-Path $src 'sos_skip_intro.c') '-o' $object
if ($LASTEXITCODE -ne 0) { throw 'could not compile sos_skip_intro.c' }
$exe = Join-Path $temp 'skip_intro_tests.exe'
$root_text = $root.Replace('\', '/')
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $root_text) `
    '-I' (Join-Path $root 'launcher') '-I' $src (Join-Path $root 'tests\skip_intro_tests.cpp') (Join-Path $root 'launcher\game_profile.cpp') $object '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the Skip Intro test' }
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue

$python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if ($python) {
    & $python.Source -I (Join-Path $root 'scripts\skip_intro_check.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
else { Write-Host '  [skip] no Python: the IL2CPP proof was not run' }

& (Join-Path $PSScriptRoot 'test_avplayer.ps1')
if ($LASTEXITCODE -ne 0) { $result = 1 }
exit $result
