# Headless ISA compatibility test: SSE4a emulation, the INSERTQ rewrite (its generated code runs natively on the host) and
# feature selection against a software reference; then, for each CUSA57767 build whose game files exist, the instruction
# boundaries and the instruction-set inventory of its real executables. No game, launcher, window or audio device is
# started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_isa_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test_isa.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' `
    '-I' (Join-Path $root 'src') (Join-Path $PSScriptRoot 'isa_tests.c') (Join-Path $root 'src\isa_emulate.c') `
    '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the ISA test' }

$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE

$python = Get-Command py, python -ErrorAction SilentlyContinue | Select-Object -First 1
foreach ($version in @('1.00', '1.01')) {
    $eboot = Find-BuildGameFile $version 'eboot.bin'
    Write-GameFileEvidence $version $eboot 'the real binary scan and instruction inventory'
    if ($result -ne 0 -or -not $python -or -not $eboot) { continue }
    Write-Host "CUSA57767 $version"
    [string[]]$pythonArguments = if ($python.Name -like 'py.*') { '-3', '-I' } else { '-I' }
    $ranges = Join-Path $temp ('ranges_' + $version)
    & $python.Source @pythonArguments (Join-Path $PSScriptRoot 'export_code_ranges.py') (Split-Path -Parent $eboot) $ranges | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'could not export the code ranges' }
    & $exe --scan $ranges $version
    $result = $LASTEXITCODE
}
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
