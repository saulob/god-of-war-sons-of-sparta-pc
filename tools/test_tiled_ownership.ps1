# Headless Vulkan proof of tiled CPU/GPU ownership on the production TileImage, DetileImage and MergeGuestPages shaders
# and code. A compute device only: no window, swapchain, launcher, game or audio. It compiles the focused test and the
# sources whose layout it depends on, and links the rest from the product's static library, so the product must have
# been built once (build\cmake\clang64).
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

# The library must be the current production one: test.ps1 builds it once (gpu-production-link); a run on its own builds it here.
. (Join-Path $PSScriptRoot '_gpu_build.ps1')
if ($env:SOS_GPU_PRODUCTION_CURRENT -ne '1' -and -not (Invoke-GpuProductionBuild @('dcpgpu'))) { exit 1 }
$build = $script:GpuBuildDir
$library = Join-Path $build 'gpu\libdcpgpu.a'
$shaders = Join-Path $build 'gpu\include\video_core\host_shaders\tiling_micro_32_comp.h'
if (-not (Test-Path -LiteralPath $library) -or -not (Test-Path -LiteralPath $shaders)) {
    Write-Host "  [FAIL] $build has no product library or generated tiling shaders: build the product once first" -ForegroundColor Red
    exit 1
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_tiled_ownership_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$prefix = Split-Path $tc.Bin -Parent
$gpu = Join-Path $root 'gpu'
$video = Join-Path $gpu 'shadps4\video_core'
$clang = Join-Path $tc.Bin 'clang++.exe'
$target = '--target=x86_64-w64-mingw32'

$generated = Join-Path $temp 'include/video_core/host_shaders'
New-Item -ItemType Directory -Force -Path $generated | Out-Null
$shaderJobs = foreach ($bits in 8, 16, 32, 64, 96, 128) {
    foreach ($macro in 0, 1) {
        $kind = if ($macro) { 'macro' } else { 'micro' }
        $name = "tiling_${kind}_${bits}_comp"
        $header = Join-Path $generated "$name.h"
        $arguments = @('--target-env', 'vulkan1.3', '-o', $header, '--vn', $name.ToUpper(), "-DBITS_PER_PIXEL=$bits", "-DIS_MACRO_TILED=$macro", (Join-Path $video 'host_shaders/tiling.comp'))
        $process = Start-Process -FilePath (Join-Path $tc.Bin 'glslangValidator.exe') -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput ($header + '.log')
        $null = $process.Handle # Windows PowerShell 5.1 reports a null ExitCode unless the handle was taken while the process ran
        [pscustomobject]@{ Header = $header; Process = $process }
    }
}
foreach ($job in $shaderJobs) {
    $job.Process.WaitForExit()
    if ($job.Process.ExitCode -ne 0) { Get-Content -LiteralPath ($job.Header + '.log') | Select-Object -First 20; Remove-Item -LiteralPath $temp -Recurse -Force; throw "could not compile $($job.Header)" }
    $text = (Get-Content -LiteralPath $job.Header -Raw).Replace('#pragma once', "#pragma once`n#include <cstdint>")
    Set-Content -LiteralPath $job.Header -Value $text -NoNewline
}

$include = @('-I', (Join-Path $temp 'include'), '-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $build 'gpu\include'),
    '-I', (Join-Path $gpu 'third_party/gcn/include'), '-I', (Join-Path $gpu 'third_party/half/include'),
    '-I', (Join-Path $gpu 'third_party/magic_enum/include'), '-I', (Join-Path $gpu 'third_party/magic_enum/include/magic_enum'),
    '-I', (Join-Path $gpu 'third_party/imgui'), '-I', (Join-Path $gpu 'third_party/xbyak'), '-I', (Join-Path $gpu 'third_party/sirit/include'),
    '-I', (Join-Path $prefix 'include'), '-I', (Join-Path $prefix 'include/vma'))
$flags = @($target, '-std=gnu++23', '-O1', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DBOOST_ASIO_STANDALONE', '-DNDEBUG',
    '-Wno-deprecated-declarations', '-Wno-missing-field-initializers', '-include', (Join-Path $gpu 'shim/dcp_win_compat.h')) + $include

$keys = Join-Path $temp 'dcp_keys.o'
& (Join-Path $tc.Bin 'clang.exe') $target '-std=c11' '-O1' '-I' (Join-Path $gpu 'shim') '-I' (Join-Path $root 'src') '-I' (Join-Path $prefix 'include') '-c' (Join-Path $root 'src\dcp_keys.c') '-o' $keys
if ($LASTEXITCODE -ne 0) { throw 'could not compile dcp_keys.c' }

$sources = @(
    (Join-Path $root 'tests\tiled_ownership_gpu_tests.cpp'),
    (Join-Path $video 'texture_cache\image.cpp'), (Join-Path $video 'texture_cache\image_info.cpp'),
    (Join-Path $video 'texture_cache\image_view.cpp'), (Join-Path $video 'texture_cache\blit_helper.cpp'),
    (Join-Path $video 'texture_cache\tile_manager.cpp'), (Join-Path $video 'renderer_vulkan\vk_runtime.cpp'),
    (Join-Path $video 'renderer_vulkan\vk_scene_resolution.cpp'))
$timer = [Diagnostics.Stopwatch]::StartNew()
$jobs = foreach ($source in $sources) {
    $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    $log = $object + '.log'
    $arguments = $flags + @('-c', $source, '-o', $object)
    $process = Start-Process -FilePath $clang -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardError $log
    $null = $process.Handle
    [pscustomobject]@{ Source = $source; Object = $object; Log = $log; Process = $process }
}
$failed = $false
foreach ($job in $jobs) {
    $job.Process.WaitForExit()
    if ($job.Process.ExitCode -ne 0) {
        $failed = $true
        Write-Host "  [FAIL] could not compile $($job.Source)" -ForegroundColor Red
        Get-Content -LiteralPath $job.Log | Select-Object -First 30
    }
}
if ($failed) { Remove-Item -LiteralPath $temp -Recurse -Force; exit 1 }

$exe = Join-Path $temp 'test_tiled_ownership.exe'
& $clang $target '-o' $exe @($jobs.Object) $keys $library (Join-Path $build 'libsos_startup_phase.a') '-L' (Join-Path $prefix 'lib') `
    '-lSDL3' '-lvulkan-1' '-lfmt' '-lxxhash' '-lpng' '-lz' (Join-Path $build 'gpu\third_party\miniz\libminiz.a') '-lZydis' '-lZycore' `
    (Join-Path $build 'gpu\third_party\sirit\src\libsirit.a') '-lntdll' '-lsynchronization' '-lwinmm' '-lbcrypt' '-lws2_32' '-pthread'
if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $temp -Recurse -Force; throw 'could not link the tiled ownership test' }
Write-Host ("  built in {0:N1} s" -f $timer.Elapsed.TotalSeconds)

$timer.Restart()
$lines = & $exe 2>&1 | Where-Object { $_ -notmatch '^GPU \[|^GPU: ' }
$result = $LASTEXITCODE
$lines | ForEach-Object { Write-Host $_ }
Write-Host ("  ran in {0:N1} s" -f $timer.Elapsed.TotalSeconds)
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
