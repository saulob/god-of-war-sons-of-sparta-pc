# Headless Ajm test: contexts, modules, instances, batches and errors through the runtime's exports, other codecs
# answering with results, and, when an installed FMOD bank holds an ATRAC9 stream, real ATRAC9 decoding by LibAtrac9
# (third_party\LibAtrac9, fetched by tools\build.ps1). No game, launcher, window or audio device is started and the
# decoded PCM is only compared numerically.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$atrac9 = Join-Path $root 'third_party\LibAtrac9\C\src'
if (-not (Test-Path -LiteralPath (Join-Path $atrac9 'libatrac9.h'))) {
    Write-Host '  [skip] LibAtrac9 is not fetched yet: run tools\build.ps1 once'
    exit 0
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_ajm_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test_ajm.exe'
$objects = @()
foreach ($source in Get-ChildItem -LiteralPath $atrac9 -Filter '*.c') {
    $object = Join-Path $temp ($source.BaseName + '.o')
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-w' '-c' $source.FullName '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $($source.Name)" }
    $objects += $object
}
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' $atrac9 `
    (Join-Path $PSScriptRoot 'ajm_tests.c') $objects '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the Ajm test' }

$env:AJM_TEST_AT9 = $null
$python = Get-Command py, python -ErrorAction SilentlyContinue | Select-Object -First 1
[string[]]$pythonArguments = if ($python -and $python.Name -like 'py.*') { '-3' } else { @() }
$fixture = Join-Path $temp 'at9.fixture'
if ($python -and (Test-Path -LiteralPath (Join-Path $root 'game\extracted'))) {
    & $python.Source @pythonArguments (Join-Path $root 'scripts\at9_fixture.py') $fixture 2>$null
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $fixture)) { $env:AJM_TEST_AT9 = $fixture }
}
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
