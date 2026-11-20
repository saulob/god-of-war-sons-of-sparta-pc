# Headless proof of the texture cache address-reuse and write-back rules on the real ImageInfo and aliasing code and the
# cache/fence wiring. No window, GPU, launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_image_reuse_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$prefix = Split-Path $tc.Bin -Parent
$gpu = Join-Path $root 'gpu'
$rootText = $root.Replace('\', '/')
$include = @('-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'),
    '-I', (Join-Path $gpu 'third_party/half/include'), '-I', (Join-Path $gpu 'third_party/magic_enum/include'),
    '-I', (Join-Path $prefix 'include'))

$exe = Join-Path $temp 'test_image_reuse.exe'
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=gnu++23' '-Wall' '-Wextra' '-O1' '-DNOMINMAX' `
    '-Wno-deprecated-declarations' '-Wno-missing-field-initializers' '-include' (Join-Path $gpu 'shim/dcp_win_compat.h') @include `
    (Join-Path $root 'tests/image_reuse_tests.cpp') (Join-Path $gpu 'shim/common/logging/log.cpp') `
    (Join-Path $gpu 'shim/common/assert.cpp') (Join-Path $gpu 'shim/common/device_lost.cpp') (Join-Path $gpu 'shadps4/video_core/texture_cache/aliasing.cpp') `
    (Join-Path $gpu 'shadps4/video_core/texture_cache/host_compatibility.cpp') "-DSOS_SOURCE_DIR=$rootText" `
    '-L' (Join-Path $prefix 'lib') '-lfmt' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the image reuse test' }

& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
