# Headless proof of the pixel input state path on the production runtime info types and GCN decoder, and of the shaders
# embedded in the eboot of every supported build. No window, GPU, launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_graphics_state_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $python = Get-Python
    if (-not $python) { throw 'Python 3 was not found (tools\setup.ps1)' }
    $pythonArguments = @($python.Args)
    $builds = @(& $python.Exe @pythonArguments -I (Join-Path $root 'scripts\build_parity_check.py') --builds | Where-Object { $_ -match '^CUSA\d+-\d+\.\d+$' })
    if (-not $builds.Count) { throw 'the supported builds could not be listed' }

    $shaderArguments = @()
    foreach ($build in $builds) {
        $version = ($build -split '-', 2)[1]
        $folder = Join-Path $temp ('shaders_' + $version)
        $dump = & $python.Exe @pythonArguments -I (Join-Path $root 'scripts\graphics_state_check.py') --dump $version $folder
        if ($LASTEXITCODE -ne 0) { $dump | ForEach-Object { Write-Host "    $_" }; throw "the embedded shaders of $build could not be written" }
        $shaderArguments += @($version, $folder)
    }

    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $gpu = Join-Path $root 'gpu'
    $include = @('-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'),
        '-I', (Join-Path $gpu 'third_party/half/include'), '-I', (Join-Path $gpu 'third_party/magic_enum/include'),
        '-I', (Join-Path $prefix 'include'))
    $frontend = Join-Path $gpu 'shadps4/shader_recompiler/frontend'
    $exe = Join-Path $temp 'test_graphics_state.exe'
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=gnu++23' '-Wall' '-Wextra' '-O1' '-DNOMINMAX' `
        '-Wno-deprecated-declarations' '-Wno-missing-field-initializers' '-include' (Join-Path $gpu 'shim/dcp_win_compat.h') @include `
        (Join-Path $root 'tests/interpolant_state_tests.cpp') (Join-Path $frontend 'decode.cpp') (Join-Path $frontend 'format.cpp') `
        (Join-Path $gpu 'shim/common/logging/log.cpp') (Join-Path $gpu 'shim/common/assert.cpp') (Join-Path $gpu 'shim/common/device_lost.cpp') '-L' (Join-Path $prefix 'lib') '-lfmt' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the graphics state test' }

    & $exe @shaderArguments
    $result = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
