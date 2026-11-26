# Headless proof for issue #2 (Intel UHD Graphics 630 startup crash in a host DLL under vkCmdBeginRendering): crash
# reports name the DLL (module, base, offset, size, file version, timestamp, no path), the Bug Report scrubber keeps that
# line, the newest vkCmdBeginRendering is reported with its attachments and whether the driver was inside it, the UHD 630
# capability profile, and the production wiring. Mutated copies must fail. No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_intel_uhd630_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clang = Join-Path $tc.Bin 'clang.exe'
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $target = '--target=x86_64-w64-mingw32'
    $cFlags = @($target, '-std=c11', '-Wall', '-Wextra', '-O1')
    $cxxFlags = @($target, '-std=c++20', '-Wall', '-Wextra', '-O1', '-Wno-missing-field-initializers')

    $copyFiles = @(
        'CMakeLists.txt',
        'launcher/log_scrubber.h',
        'launcher/log_scrubber.cpp',
        'launcher/utf8.h',
        'src/runtime_log.h',
        'src/runtime_log.c',
        'src/runtime_session_log.c',
        'src/crash_guard.h',
        'src/crash_guard.c',
        'src/host_module.h',
        'src/host_module.c',
        'src/startup_phase.h',
        'src/startup_phase.c',
        'src/loader.c',
        'gpu/shim/dcp_rendering_trace.h',
        'gpu/shadps4/common/thread.cpp',
        'gpu/shadps4/video_core/renderer_vulkan/vk_gpu_compat.h',
        'gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp',
        'gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp'
    )

    $newWorkspace = {
        param([string]$Dir)
        foreach ($rel in $copyFiles) {
            Copy-ToWorkspace $Dir (Join-Path $root $rel) $rel | Out-Null
        }
    }

    # A mutated workspace reuses the baseline object of every C file it did not change.
    $buildIn = {
        param([string]$Dir, [string]$Exe, [string[]]$Extra, [string]$Reuse = '')
        $inc = @('-I', (Join-Path $Dir 'launcher'), '-I', (Join-Path $Dir 'src'), '-I', (Join-Path $Dir 'gpu/shim'),
            '-I', (Join-Path $Dir 'gpu/shadps4'), '-I', (Join-Path $prefix 'include'))
        $objs = New-Object System.Collections.Generic.List[string]
        foreach ($cSrc in @('src/runtime_log.c', 'src/runtime_session_log.c', 'src/crash_guard.c', 'src/host_module.c', 'src/startup_phase.c')) {
            $name = [IO.Path]::GetFileNameWithoutExtension($cSrc) + '.o'
            $obj = Join-Path $Dir $name
            $header = [IO.Path]::ChangeExtension($cSrc, '.h')
            $unchanged = $Reuse -and ([IO.File]::ReadAllText((Join-Path $Dir $cSrc)) -eq [IO.File]::ReadAllText((Join-Path $Reuse $cSrc))) -and
                (-not (Test-Path -LiteralPath (Join-Path $Dir $header)) -or
                 [IO.File]::ReadAllText((Join-Path $Dir $header)) -eq [IO.File]::ReadAllText((Join-Path $Reuse $header)))
            if ($unchanged) {
                Copy-Item -LiteralPath (Join-Path $Reuse $name) -Destination $obj -Force
            } else {
                & $clang @cFlags @Extra @inc '-c' (Join-Path $Dir $cSrc) '-o' $obj
                if ($LASTEXITCODE -ne 0) { return }
            }
            $objs.Add($obj)
        }
        $rootText = $Dir.Replace('\', '/')
        foreach ($src in @((Join-Path $root 'tests/intel_uhd630_tests.cpp'), (Join-Path $Dir 'launcher/log_scrubber.cpp'))) {
            $obj = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
            & $clangxx @cxxFlags @Extra @inc "-DSOS_SOURCE_DIR=$rootText" '-c' $src '-o' $obj
            if ($LASTEXITCODE -ne 0) { return }
            $objs.Add($obj)
        }
        & $clangxx $target '-o' $Exe @($objs) '-ldbghelp'
    }

    $base = Join-Path $temp 'base'
    & $newWorkspace $base
    $exe = Join-Path $base 'intel_uhd630_tests.exe'
    & $buildIn $base $exe @()
    if ($LASTEXITCODE -ne 0) { throw 'could not compile intel_uhd630_tests' }
    $run = Invoke-TimedProgram $exe
    Write-Host $run.Text.TrimEnd()
    $result = $run.Code

    $hostModule = 'src/host_module.c'
    $guard = 'src/crash_guard.c'
    $trace = 'gpu/shim/dcp_rendering_trace.h'
    $mutations = @(
        @{ Name = 'a path in the export name reaches the report'; File = $hostModule; Old = "            used = 0;`n            continue;"; New = '' },
        @{ Name = 'the first resource type is taken as RT_VERSION'; File = $hostModule; Old = 'if (want_id && (entry.Name & 0x80000000u ? 1 : entry.Name != id)) continue;'; New = '' },
        @{ Name = 'the file version halves are swapped'; File = $hostModule; Old = 'version[0] = (uint16_t)(words[i + 2] >> 16);'; New = 'version[0] = (uint16_t)(words[i + 3] >> 16);' },
        @{ Name = 'a 32-bit optional header is accepted'; File = $hostModule; Old = 'nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||'; New = '' },
        @{ Name = 'reads leave the image bounds'; File = $hostModule; Old = 'if (rva >= image->image_size || size > image->image_size - rva) return 0;'; New = '' },
        @{ Name = 'the report drops the module base'; File = $hostModule; Old = '    put_str(&out, " module_base=");' + "`n" + '    put_hex(&out, module->base);'; New = '' },
        @{ Name = 'crash reports go back to module+'; File = $guard; Old = '    if (sos_host_module_at(address, &module)) {'; New = '    if (0) {' },
        @{ Name = 'the CRASH report omits the host module'; File = $guard; Old = "    describe(&text, (uintptr_t)c->Rip);`n    put_host_module(&text, (uintptr_t)c->Rip);"; New = '    describe(&text, (uintptr_t)c->Rip);' },
        @{ Name = 'the scrubber turns file_version into <ipv4>'; File = 'launcher/log_scrubber.cpp'; Old = 'StartsFolded(word.substr(word.size() - kVersionSuffix.size()), kVersionSuffix))'; New = 'false)' },
        @{ Name = 'the scrubber exempts every snake_case key'; File = 'launcher/log_scrubber.cpp'; Old = 'StartsFolded(word.substr(word.size() - kVersionSuffix.size()), kVersionSuffix))'; New = 'word.find(''_'') != std::string_view::npos)' },
        @{ Name = 'the unbound-attachment line repeats'; File = $trace; Old = '!logged_unbound.exchange(true, std::memory_order_relaxed)'; New = 'true' },
        @{ Name = 'a pass inside the driver reads as returned'; File = $trace; Old = '        in_driver.store(true, std::memory_order_release);'; New = '' },
        @{ Name = 'the unbound mask is not kept'; File = $trace; Old = 'std::uint64_t(s.unbound_colors & 0xff) << 4 |'; New = '' },
        @{ Name = 'the recorder never leaves the driver'; File = 'gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp'; Old = "    cmdbuf.beginRendering(info);`n    trace.End();"; New = '    cmdbuf.beginRendering(info);' },
        @{ Name = 'pipeline binaries ignore the maintenance5 extension'; File = 'gpu/shadps4/video_core/renderer_vulkan/vk_gpu_compat.h'; Old = 'return maintenance5_extension && maintenance5_feature && pipeline_binaries_feature;'; New = 'return maintenance5_feature && pipeline_binaries_feature;' },
        @{ Name = 'the Windows crash report omits the rendering pass'; File = 'src/loader.c'; Old = 'fprintf(stderr, "  rendering: %s\n", rendering);'; New = '' },
        @{ Name = 'host threads stay unnamed in crash reports'; File = 'gpu/shadps4/common/thread.cpp'; Old = "    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());`n    sos_thread_label_set(name);"; New = '    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());' }
    )

    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        & $newWorkspace $work
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutExe = Join-Path $work 'intel_uhd630_tests.exe'
        $build = { & $buildIn $work $mutExe @('-w') $base }.GetNewClosure()
        if (Test-MutationDetected $mutation.Name $build $mutExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "intel-uhd630 mutations: $detected of $($mutations.Count) detected"
    exit $result
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
