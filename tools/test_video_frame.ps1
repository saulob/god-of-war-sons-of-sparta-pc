# Headless proof of the decoded NV12 frame copy of AvPlayer on the production CopyNV12Data: rows at the guest pitch whatever
# the decoder's row size of each plane, 1920x1080 and small non-aligned pictures, padded destination height, and mutated
# copies of the function that the same test must reject. Needs only the pinned FFmpeg headers. No window, GPU, launcher,
# game or audio device is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$ffmpeg = if ($env:DCP_FFMPEG_PREFIX) { $env:DCP_FFMPEG_PREFIX } else { Get-FFmpegPrefix }
if (-not (Test-Path -LiteralPath (Join-Path $ffmpeg 'include\libavutil\frame.h'))) {
    Write-Host "  [skip] the pinned FFmpeg headers are not built yet ($ffmpeg): run tools\build.ps1 once; the NV12 frame test was skipped"
    exit 0
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_video_frame_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $gpu = Join-Path $root 'gpu'
    $clang = Join-Path $tc.Bin 'clang++.exe'
    $include = @('-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'),
        '-I', (Join-Path $ffmpeg 'include'), '-I', (Join-Path $prefix 'include'))
    $flags = @('--target=x86_64-w64-mingw32', '-std=gnu++23', '-Wall', '-Wextra', '-O1', '-DNOMINMAX', '-Wno-deprecated-declarations', '-Wno-sign-compare',
        '-include', (Join-Path $gpu 'shim/dcp_win_compat.h'))
    $utils = Join-Path $gpu 'shadps4/core/libraries/videodec/video_utils.cpp'
    $header = Join-Path $gpu 'shadps4/core/libraries/videodec/video_utils.h'
    $test = Join-Path $root 'tests/video_frame_tests.cpp'
    $support = @((Join-Path $gpu 'shim/common/logging/log.cpp'), (Join-Path $gpu 'shim/common/assert.cpp'),
        (Join-Path $gpu 'shim/common/device_lost.cpp'))
    $link = @('-L', (Join-Path $prefix 'lib'), '-lfmt')

    $exe = Join-Path $temp 'video_frame_tests.exe'
    & $clang @flags @include $test $utils @support @link '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the video frame test' }
    & $exe
    $result = $LASTEXITCODE

    $mutations = @(
        @{ Name = 'a bulk copy ignores a decoder row wider than the guest pitch'; Old = 'const bool contiguous = src.linesize[0] == src.width && src.linesize[1] == src.width;'; New = 'const bool contiguous = true;' },
        @{ Name = 'the chroma plane uses the luma row size'; Old = 'src.data[1] + y * src.linesize[1], src.width);'; New = 'src.data[1] + y * src.linesize[0], src.width);' },
        @{ Name = 'the rows below the picture are not filled'; Old = 'if (src.height != dst_height) {'; New = 'if (false) {' },
        @{ Name = 'the guest pitch is not the destination row size'; Old = 'std::memcpy(luma_dst + y * dst_pitch, src.data[0] + y * src.linesize[0], src.width);'; New = 'std::memcpy(luma_dst + y * src.width, src.data[0] + y * src.linesize[0], src.width);' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + $detected + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $copy = Copy-ToWorkspace $work $utils 'video_utils.cpp'
        $null = Copy-ToWorkspace $work $header 'video_utils.h'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'video_frame_tests.exe'
        $build = { & $clang @flags '-w' @include $test $copy @support @link '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "video frame mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
