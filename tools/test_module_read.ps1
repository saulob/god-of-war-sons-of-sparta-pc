# Headless test of the dynamic module loader's file read; with a prepared CUSA57767 1.00 it also times reading
# Il2cppUserAssemblies.prx, the module whose exports libfmod's imports resolve against. No game, launcher, window or
# audio device is started.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$temp = Join-Path ([IO.Path]::GetTempPath()) ('sos_module_read_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$tc = Resolve-Toolchain 'auto'
$exe = Join-Path $temp 'test.exe'
& (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O2' `
    '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') `
    '-I' (Join-Path $root 'gpu\shim') (Join-Path $PSScriptRoot 'module_read_tests.c') `
    (Join-Path $PSScriptRoot 'startup_profile_stubs.c') (Join-Path $root 'src\runtime_stop.c') '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the module read test' }
$env:PATH = "$($tc.Bin);$env:PATH"
$module = Find-BuildGameFile '1.00' 'Media\Modules\Il2cppUserAssemblies.prx'
Write-GameFileEvidence '1.00' $module 'the real game module'
if ($module) { & $exe $module } else { & $exe }
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
exit $result
