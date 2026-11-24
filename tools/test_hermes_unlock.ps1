# Headless validation of the Hermes Mode early unlock on CUSA57767 1.00 and 1.01: the Gameplay setting and its default,
# the verified site, the exact six bytes the patch changes on the real boot images, Off leaving them untouched,
# unknown-build and changed-image safety, the loader wiring, and the static proof from the IL2CPP metadata and game code.
# No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_hermes_unlock_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$result = 0
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $src = Join-Path $root 'src'
    $object = Join-Path $temp 'sos_hermes_unlock.o'
    & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-O1' '-I' $src '-c' (Join-Path $src 'sos_hermes_unlock.c') '-o' $object
    if ($LASTEXITCODE -ne 0) { throw 'could not compile sos_hermes_unlock.c' }
    $exe = Join-Path $temp 'hermes_unlock_tests.exe'
    $rootText = $root.Replace('\', '/')
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) `
        '-I' $src (Join-Path $root 'tests\hermes_unlock_tests.cpp') $object '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the Hermes Mode test' }
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $python = Get-Python
    if ($python) {
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\hermes_unlock_check.py')
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    } else {
        Write-Host '  [fail] Python 3 not found'
        $result = 1
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
