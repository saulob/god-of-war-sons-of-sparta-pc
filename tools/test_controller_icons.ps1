# Headless test of the Xbox left-stick glyphs on the game's own resources.assets and resources.assets.resS (CUSA57767 1.00, when
# installed): detecting the missing LASUP, LASDOWN, LASLEFT and LASRIGHT, preparing them from the game's own PlayStation stick
# artwork into complete copies, validating the copies, partial and unknown sources, the cache and its invalidation, and the exact
# path redirection. The game's files are only read. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_icons_build_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$launcher = Join-Path $root 'launcher'
$exe = Join-Path $temp 'test_controller_icons.exe'
$rootText = $root.Replace('\', '/')
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O2' "-DSOS_SOURCE_DIR=`"$rootText`"" '-I' $launcher `
    (Join-Path $root 'tests\controller_icons_tests.cpp') (Join-Path $launcher 'controller_icons.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the controller icons test' }
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
exit $result
