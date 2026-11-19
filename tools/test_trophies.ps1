# Headless test of the local trophies: TRP unpacking, trophy-list decryption, the Trophy Key, per-user progress, the
# sceNpTrophy* service (including the answers it keeps while no trophy list is readable) and the Trophies page's content
# model. Synthetic packs in a temporary folder; no game, launcher, window or audio device is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_trophy_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang++.exe'
$exe = Join-Path $temp 'test_trophies.exe'
$sources = @(
    'tests\trophy_tests.cpp',
    'launcher\trophy_content.cpp',
    'launcher\crypto_win.cpp',
    'gpu\shim\dcp_png.cpp'
) + @('trophy_data', 'trophy_key', 'trophy_pack', 'trophy_runtime', 'trophy_service', 'trophy_store', 'trophy_view', 'trp_archive' |
    ForEach-Object { "src\trophies\$_.cpp" })
$trophySources = (Join-Path $root 'src\trophies').Replace('\', '/')
& $clang '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' "-DSOS_TROPHY_SOURCE_DIR=`"$trophySources`"" `
    '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'gpu\shim') `
    @($sources | ForEach-Object { Join-Path $root $_ }) '-lbcrypt' '-lpng' '-lz' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the trophy test' }

$env:PATH = "$($tc.Bin);$env:PATH"
$game = Join-Path $root 'game\prepared\CUSA57767-1.00'
if (Test-Path -LiteralPath (Join-Path $game 'sce_sys\npbind.dat')) { $env:SOS_TROPHY_GAME = $game }
else { Write-Host '  [skip] no prepared CUSA57767 1.00: the real trophy pack was not read' }
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
