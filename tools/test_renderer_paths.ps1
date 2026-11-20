# Headless proof of the renderer paths behind the Graphics and Display options: texture filtering, SMAA, FSR 1, RCAS,
# VSync, Presentation and frame pacing: pure helpers numerically, and the Vulkan code checked to call them. No window,
# GPU, launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_renderer_paths_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$prefix = Split-Path $tc.Bin -Parent
$target = '--target=x86_64-w64-mingw32'
$include = @('-I', (Join-Path $root 'gpu\shim'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $prefix 'include'))
$rootText = $root.Replace('\', '/')

$keys = Join-Path $temp 'dcp_keys.o'
& (Join-Path $tc.Bin 'clang.exe') $target '-std=c11' '-Wall' '-Wextra' '-Werror' '-O1' @include '-c' (Join-Path $root 'src\dcp_keys.c') '-o' $keys
if ($LASTEXITCODE -ne 0) { throw 'could not compile dcp_keys.c' }

$exe = Join-Path $temp 'test_renderer_paths.exe'
& (Join-Path $tc.Bin 'clang++.exe') $target '-std=c++23' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) @include `
    (Join-Path $root 'tests\renderer_paths_tests.cpp') (Join-Path $root 'gpu\shim\dcp_settings.cpp') $keys `
    '-L' (Join-Path $prefix 'lib') '-lSDL3' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the runtime settings test' }

& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
