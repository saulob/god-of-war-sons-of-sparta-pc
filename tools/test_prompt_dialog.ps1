# Headless test of the Settings confirmation dialog glyphs: the hook table, the handler on fake game objects (forced Xbox, forced
# PlayStation, Automatic with each controller kind, restoration, every missing object) and, when the boot images are installed, the
# hook on the real code of CUSA57767 1.00 and 1.01 and the static proof of the dialogs it covers. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_prompt_dialog_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$launcher = Join-Path $root 'launcher'
$src = Join-Path $root 'src'
$objects = @()
foreach ($unit in @(@{ Source = (Join-Path $launcher 'controller_prompts.c') }, @{ Source = (Join-Path $launcher 'prompt_dialog.c') }, @{ Source = (Join-Path $src 'sos_skip_intro.c') },
        @{ Source = (Join-Path $src 'redzone_analysis.c') }, @{ Source = (Join-Path $src 'isa_emulate.c') })) {
    $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($unit.Source) + '.o')
    & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $launcher '-I' $src '-c' $unit.Source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $($unit.Source)" }
    $objects += $object
}
$exe = Join-Path $temp 'test_prompt_dialog.exe'
$rootText = $root.Replace('\', '/')
& (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' "-DSOS_SOURCE_DIR=`"$rootText`"" '-I' $launcher '-I' $src `
    (Join-Path $root 'tests\prompt_dialog_tests.cpp') @objects '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the prompt dialog test' }

& $exe
$result = $LASTEXITCODE

$python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if ($python) {
    & $python.Source -I (Join-Path $root 'scripts\prompt_dialog.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
else { Write-Host '  [skip] no Python: the dialog hook and the dialogs it covers were not read from the game code' }
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
exit $result
