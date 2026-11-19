# Headless checks of Skip Intro: the build gating and the setting-to-player wiring, then the player itself (a generated
# clip: only the first source is cut, its audio is silent, the end of the file is a natural one) through
# tools\test_avplayer.ps1. No game, launcher, window or audio device is started.
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
$exe = Join-Path $temp 'skip_intro_tests.exe'
$root_text = $root.Replace('\', '/')
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' "-DSOS_SOURCE_DIR=`"$root_text`"" `
    '-I' (Join-Path $root 'launcher') (Join-Path $root 'tests\skip_intro_tests.cpp') (Join-Path $root 'launcher\game_profile.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the Skip Intro test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue

& (Join-Path $PSScriptRoot 'test_avplayer.ps1')
if ($LASTEXITCODE -ne 0) { $result = 1 }
exit $result
