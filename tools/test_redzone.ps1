# Headless red-zone test: hand-made functions and malformed function tables; then, when a prepared CUSA57767 1.00 boot
# image exists, the parallel analysis against a sequential full-decode reference at several worker counts, the install
# digest at 1, 2, 4 and 32 workers, every cache rejection, and (-Bench) the phase timings. No game, launcher, window or
# audio device is started.
[CmdletBinding()]
param([switch]$Bench)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_redzone_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$exe = Join-Path $temp 'test_redzone.exe'
$src = Join-Path $root 'src'
& (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O2' '-D_WIN32_WINNT=0x0A00' `
    '-I' $src (Get-StringDefine 'REDZONE_SOURCE' 'runtime_redzone.c') (Join-Path $PSScriptRoot 'redzone_tests.c') `
    (Join-Path $src 'isa_emulate.c') (Join-Path $src 'redzone_analysis.c') (Join-Path $src 'redzone_cache.c') `
    '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the red-zone test' }

$env:PATH = "$($tc.Bin);$env:PATH"
& $exe --synthetic
$result = $LASTEXITCODE

$found = Find-BuildBootImage '1.00'
Write-Evidence 'image' '1.00' $(if ($found) { $found.State } else { 'NO DATA' }) $(if ($found) { 'boot.bin of the red-zone analysis' } else { 'no boot image in build\image or build\research\image; not read: the real image analysis' })
if ($result -eq 0 -and $found) {
    $boot = $found.Path
    & $exe --verify $boot 349685 | Where-Object { $_ -notmatch '^Red zone: \d+ tables' }
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    $cache = Join-Path $temp 'cache'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    & $exe --cache $boot $cache | Where-Object { $_ -notmatch '^Red zone: \d+ tables' }
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    if ($Bench) {
        $benchCache = Join-Path $temp 'bench'
        New-Item -ItemType Directory -Force -Path $benchCache | Out-Null
        & $exe --bench $boot $benchCache | Where-Object { $_ -notmatch '^Red zone: \d+ tables' }
    }
}
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
exit $result
