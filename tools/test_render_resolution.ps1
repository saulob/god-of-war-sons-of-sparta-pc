# Headless checks of the render resolution and aspect ratio support: choice tables, the render target the game's screen
# manager accepts, the verified patch sites (synthetic images and the installed boot images of both builds) and the
# settings-to-loader wiring. No window, game or network.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_resolution_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$target = '--target=x86_64-w64-mingw32'
$exe = Join-Path $temp 'test_render_resolution.exe'
$root_text = $root.Replace('\', '/')
$include = @('-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'gpu\shim'),
             '-I', (Join-Path $root 'third_party\trg-launcher\include'))

$trg = @()
foreach ($name in 'settings', 'platform') {
    $object = Join-Path $temp "trg_$name.o"
    & $clangxx $target '-std=c++17' '-O1' '-D_CRT_SECURE_NO_WARNINGS' @include '-c' `
        (Join-Path $root "third_party\trg-launcher\src\$name.cpp") '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile the TRG $name" }
    $trg += $object
}

$sources = @('tests\render_resolution_tests.cpp', 'launcher\resolution.cpp', 'launcher\game_profile.cpp') |
    ForEach-Object { Join-Path $root $_ }
& $clangxx $target '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $root_text) @include @($sources) @($trg) `
    '-lshell32' '-lole32' '-lcomdlg32' '-lwinhttp' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the render resolution test' }

& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
