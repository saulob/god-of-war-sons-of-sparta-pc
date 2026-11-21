# Headless proof of shared-host startup diagnostic stages, last-stage crash diagnosis, bounded milestone/checkpoint
# caps, DeviceLost exit flush wiring, and mutations that remove critical startup stages.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_startup_diag_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clang = Join-Path $tc.Bin 'clang.exe'
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $target = '--target=x86_64-w64-mingw32'

    $copyFiles = @(
        'src/startup_phase.h',
        'src/startup_phase.c',
        'src/loader.c',
        'src/runtime_audio.c',
        'src/game_build.h',
        'gpu/shim/common/device_lost.h',
        'gpu/shim/common/device_lost.cpp',
        'gpu/shadps4/common/types.h',
        'gpu/shim/dcp_win_compat.h',
        'gpu/shim/dcpgpu.cpp',
        'gpu/shim/dcp_startup_watch.cpp',
        'gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp',
        'gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp',
        'launcher/bug_report.h',
        'launcher/bug_report.cpp',
        'launcher/log_scrubber.h',
        'launcher/log_scrubber.cpp',
        'launcher/game_profile.h',
        'launcher/game_profile.cpp',
        'launcher/sos_resolution.h',
        'launcher/utf8.h',
        'launcher/localization/ui_text.h',
        'launcher/localization/ui_text.cpp'
    )

    $newWorkspace = {
        param([string]$Dir)
        foreach ($rel in $copyFiles) {
            Copy-ToWorkspace $Dir (Join-Path $root $rel) $rel | Out-Null
        }
    }

    $buildIn = {
        param([string]$Dir, [string]$Exe, [string[]]$Extra)
        $inc = @('-I', (Join-Path $Dir 'src'), '-I', (Join-Path $Dir 'launcher'), '-I', (Join-Path $Dir 'gpu/shim'),
                 '-I', (Join-Path $Dir 'gpu/shadps4'), '-I', (Join-Path $prefix 'include'))
        $rootText = $Dir.Replace('\', '/')
        $phaseObj = Join-Path $Dir 'startup_phase.o'
        & $clang $target '-std=c11' '-Wall' '-Wextra' '-O1' @Extra @inc '-c' (Join-Path $Dir 'src/startup_phase.c') '-o' $phaseObj
        if ($LASTEXITCODE -ne 0) { return }
        $objs = New-Object System.Collections.Generic.List[string]
        $objs.Add($phaseObj)
        $cxxSources = @(
            (Join-Path $root 'tests/startup_diagnostics_tests.cpp'),
            (Join-Path $Dir 'gpu/shim/common/device_lost.cpp'),
            (Join-Path $Dir 'launcher/bug_report.cpp'),
            (Join-Path $Dir 'launcher/log_scrubber.cpp'),
            (Join-Path $Dir 'launcher/game_profile.cpp'),
            (Join-Path $Dir 'launcher/localization/ui_text.cpp')
        )
        foreach ($src in $cxxSources) {
            $obj = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
            & $clangxx $target '-std=gnu++23' '-Wall' '-Wextra' '-O1' '-DNOMINMAX' '-Wno-deprecated-declarations' `
                '-Wno-missing-field-initializers' '-include' (Join-Path $Dir 'gpu/shim/dcp_win_compat.h') `
                "-DSOS_SOURCE_DIR=$rootText" @Extra @inc '-c' $src '-o' $obj
            if ($LASTEXITCODE -ne 0) { return }
            $objs.Add($obj)
        }
        & $clangxx $target '-o' $Exe @($objs) '-pthread'
    }

    $base = Join-Path $temp 'base'
    & $newWorkspace $base
    $exe = Join-Path $base 'startup_diagnostics_tests.exe'
    & $buildIn $base $exe @()
    if ($LASTEXITCODE -ne 0) { throw 'could not compile startup_diagnostics_tests' }
    $run = Invoke-TimedProgram $exe
    Write-Host $run.Text.TrimEnd()
    $result = $run.Code

    $mutations = @(
        @{ Name = 'Vulkan device creation startup stage missing'; File = 'gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp'; Old = '    sos_startup_phase("Vulkan device creation");'; New = '' },
        @{ Name = 'swapchain creation startup stage missing'; File = 'gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp'; Old = '      surface{(sos_startup_phase("swapchain creation"), CreateSurface(instance.GetInstance(), window))},'; New = '      surface{CreateSurface(instance.GetInstance(), window)},' },
        @{ Name = 'boot image load startup stage missing'; File = 'src/loader.c'; Old = '    sos_startup_phase("boot image load");'; New = '' },
        @{ Name = 'crash context omits current startup phase'; File = 'src/startup_phase.c'; Old = '    at = append(out, size, at, sos_startup_phase_current());'; New = '    at = append(out, size, at, "unknown");' }
    )

    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        & $newWorkspace $work
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutExe = Join-Path $work 'startup_diagnostics_tests.exe'
        $build = { & $buildIn $work $mutExe @('-w') }.GetNewClosure()
        if (Test-MutationDetected $mutation.Name $build $mutExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "startup-diagnostics mutations: $detected of $($mutations.Count) detected"
    exit $result
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
