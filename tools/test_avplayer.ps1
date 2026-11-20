# Headless AvPlayer tests: the worker thread wrapper, then libSceAvPlayer through its import table with a small clip
# generated for the run. No game, launcher, window or audio device is used. The API tests need the pinned FFmpeg
# (tools\build.ps1 builds it into cache\deps\ffmpeg; DCP_FFMPEG_PREFIX overrides the folder) and are skipped without it.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_avplayer_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang++.exe'
$gpu = Join-Path $root 'gpu'
$common = @('--target=x86_64-w64-mingw32', '-std=gnu++23', '-Wall', '-Wextra')
$result = 0

function Invoke-Compile([string[]]$Arguments) {
    & $clang @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the AvPlayer test' }
}

$threadExe = Join-Path $temp 'thread_tests.exe'
Invoke-Compile ($common + @('-Werror', '-I', (Join-Path $gpu 'shadps4'), (Join-Path $PSScriptRoot 'avplayer_thread_tests.cpp'), '-pthread', '-o', $threadExe))
$env:PATH = "$($tc.Bin);$env:PATH"
Write-Host 'AvPlayer worker threads'
& $threadExe
if ($LASTEXITCODE -ne 0) { $result = 1 }

$prefix = if ($env:DCP_FFMPEG_PREFIX) { $env:DCP_FFMPEG_PREFIX } else { Get-FFmpegPrefix }
if (-not (Test-Path -LiteralPath (Join-Path $prefix 'lib\pkgconfig\libavformat.pc'))) {
    Write-Host "  [skip] the pinned FFmpeg is not built yet ($prefix): run tools\build.ps1 once; the API tests were skipped"
} else {
    $media = Join-Path $temp 'media'
    New-Item -ItemType Directory -Force -Path $media | Out-Null
    $generator = Join-Path $tc.Bin 'ffmpeg.exe'
    $env:AVPLAYER_TEST_MEDIA = $null
    if (Test-Path -LiteralPath $generator) {
        & $generator -hide_banner -loglevel error -y -f lavfi -i 'testsrc=size=128x96:rate=15' -f lavfi -i 'sine=frequency=440:sample_rate=48000' `
            -t 2 -c:v libx264 -pix_fmt yuv420p -g 15 -c:a aac -ac 1 -shortest (Join-Path $media 'tiny.mp4')
        $clip = $LASTEXITCODE -eq 0
        # Video only, exactly 30 frames with B-frames, so the decoder holds frames back until it is drained.
        & $generator -hide_banner -loglevel error -y -f lavfi -i 'testsrc=size=128x96:rate=15' -frames:v 30 -c:v libx264 `
            -pix_fmt yuv420p -g 15 -bf 2 (Join-Path $media 'video.mp4')
        if ($clip -and $LASTEXITCODE -eq 0) { $env:AVPLAYER_TEST_MEDIA = $media }
    }
    if (-not $env:AVPLAYER_TEST_MEDIA) { Write-Host '  [warn] no ffmpeg.exe to generate the test clip: only the media-free API tests run' }

    $apiExe = Join-Path $temp 'avplayer_tests.exe'
    $sources = @(Get-ChildItem -LiteralPath (Join-Path $gpu 'shadps4\core\libraries\avplayer') -Filter '*.cpp' | ForEach-Object FullName) + @(
        (Join-Path $gpu 'shadps4\core\libraries\videodec\video_utils.cpp'),
        (Join-Path $gpu 'shim\common\logging\log.cpp'), (Join-Path $gpu 'shim\common\assert.cpp'),
        (Join-Path $gpu 'shadps4\common\thread.cpp'), (Join-Path $gpu 'shadps4\common\error.cpp'), (Join-Path $gpu 'shadps4\common\string_util.cpp'))
    Invoke-Compile ($common + @('-O1', '-w', '-D_WIN32_WINNT=0x0A00', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DBOOST_ASIO_STANDALONE', '-DNDEBUG',
        '-include', (Join-Path $gpu 'shim\dcp_win_compat.h'),
        '-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'),
        '-I', (Join-Path $gpu 'third_party\magic_enum\include'), '-I', (Join-Path $gpu 'third_party\magic_enum\include\magic_enum'),
        '-I', (Join-Path $prefix 'include'), (Join-Path $PSScriptRoot 'avplayer_tests.cpp')) + $sources + @(
        '-L', (Join-Path $prefix 'lib'), '-lavformat', '-lavcodec', '-lavutil', '-lswscale', '-lswresample', '-lfmt', '-pthread', '-lntdll', '-o', $apiExe))
    $env:PATH = "$(Join-Path $prefix 'bin');$env:PATH"
    Write-Host 'libSceAvPlayer'
    & $apiExe
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source (Join-Path $root 'scripts\avplayer_check.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
} else {
    Write-Host '  [warn] python is not installed; the AvPlayer import coverage check was skipped'
}
exit $result
