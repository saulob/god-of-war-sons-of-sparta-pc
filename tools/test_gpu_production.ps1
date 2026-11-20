# Validates the CURRENT production GPU sources: CMake source registration, compile integration and link integration. The
# production target dcpgpu is built incrementally by Ninja (only what changed) and dcpgpu_link_smoke links every one of its
# objects with the loader's runtime, so undefined and duplicate symbols show although a static library hides them. Only
# these targets are built: no launcher, game executable, packaging or other tool. No window, game or audio device is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_gpu_build.ps1')

Write-Host 'Production GPU targets: dcpgpu, dcpgpu_link_smoke (current sources, incremental)'
if (-not (Invoke-GpuProductionBuild)) { Write-Host '  [fail] the current production GPU sources do not build and link'; exit 1 }

$tc = Resolve-Toolchain 'auto'
$members = @(& (Join-Path $tc.Bin 'llvm-ar.exe') t $script:GpuLibrary | ForEach-Object { [IO.Path]::GetFileName($_) })
$failures = 0
foreach ($required in 'device_lost.cpp', 'assert.cpp', 'vk_scheduler.cpp', 'vk_semaphore.cpp', 'vk_presenter.cpp', 'vk_swapchain.cpp',
        'buffer_cache.cpp', 'texture_cache.cpp', 'vk_pipeline_cache.cpp', 'vk_instance.cpp') {
    if ($members -notcontains "$required.obj") { Write-Host "  [fail] $required is not part of the production target dcpgpu (gpu/CMakeLists.txt)"; $failures++ }
}
foreach ($source in Get-ChildItem -LiteralPath (Join-Path $root 'gpu\shim\common') -Filter '*.cpp' -File) {
    if ($members -notcontains "$($source.Name).obj") { Write-Host "  [fail] gpu/shim/common/$($source.Name) is not registered in gpu/CMakeLists.txt"; $failures++ }
}
if ($failures) { exit 1 }
Write-Host "  $($members.Count) objects in dcpgpu, current; DeviceLost, Semaphore, presenter, swapchain, buffer cache and texture cache link"
exit 0
