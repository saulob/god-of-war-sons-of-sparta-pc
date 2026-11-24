# Headless proof of crash dump discovery, same-execution pairing, MDMP/size/age validation, optional attachment,
# session-number reservation in runtime_log.c, and Vulkan device-lost vs generic crash diagnostics, plus mutations.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_crash_dump_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clang = Join-Path $tc.Bin 'clang.exe'
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $target = '--target=x86_64-w64-mingw32'
    $cFlags = @($target, '-std=c11', '-Wall', '-Wextra', '-O1')
    $cxxFlags = @($target, '-std=c++20', '-Wall', '-Wextra', '-O1')

    $copyFiles = @(
        'launcher/bug_report.h',
        'launcher/bug_report.cpp',
        'launcher/log_scrubber.h',
        'launcher/log_scrubber.cpp',
        'launcher/game_profile.h',
        'launcher/game_profile.cpp',
        'launcher/sos_resolution.h',
        'launcher/sos_resolution_sites.h',
        'launcher/utf8.h',
        'launcher/localization/ui_text.h',
        'launcher/localization/ui_text.cpp',
        'src/runtime_log.h',
        'src/runtime_log.c',
        'src/runtime_session_log.c',
        'src/crash_guard.h',
        'src/crash_guard.c',
        'src/startup_phase.h',
        'src/startup_phase.c',
        'src/game_build.h'
    )

    $newWorkspace = {
        param([string]$Dir)
        foreach ($rel in $copyFiles) {
            Copy-ToWorkspace $Dir (Join-Path $root $rel) $rel | Out-Null
        }
    }

    $buildIn = {
        param([string]$Dir, [string]$Exe, [string[]]$Extra)
        $inc = @('-I', (Join-Path $Dir 'launcher'), '-I', (Join-Path $Dir 'src'), '-I', (Join-Path $prefix 'include'))
        $objs = New-Object System.Collections.Generic.List[string]
        foreach ($cSrc in @('src/runtime_log.c', 'src/runtime_session_log.c', 'src/crash_guard.c', 'src/startup_phase.c')) {
            $obj = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($cSrc) + '.o')
            & $clang @cFlags @Extra @inc '-c' (Join-Path $Dir $cSrc) '-o' $obj
            if ($LASTEXITCODE -ne 0) { return }
            $objs.Add($obj)
        }
        $cxxSources = @(
            (Join-Path $root 'tests/crash_dump_tests.cpp'),
            (Join-Path $Dir 'launcher/bug_report.cpp'),
            (Join-Path $Dir 'launcher/log_scrubber.cpp'),
            (Join-Path $Dir 'launcher/game_profile.cpp'),
            (Join-Path $Dir 'launcher/localization/ui_text.cpp')
        )
        foreach ($src in $cxxSources) {
            $obj = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
            & $clangxx @cxxFlags @Extra @inc '-c' $src '-o' $obj
            if ($LASTEXITCODE -ne 0) { return }
            $objs.Add($obj)
        }
        & $clangxx $target '-o' $Exe @($objs) '-ldbghelp'
    }

    $base = Join-Path $temp 'base'
    & $newWorkspace $base
    $exe = Join-Path $base 'crash_dump_tests.exe'
    & $buildIn $base $exe @()
    if ($LASTEXITCODE -ne 0) { throw 'could not compile crash_dump_tests' }
    $run = Invoke-TimedProgram $exe
    Write-Host $run.Text.TrimEnd()
    $result = $run.Code

    $mutations = @(
        @{ Name = 'unrelated dump name accepted as same execution'; File = 'launcher/bug_report.cpp'; Old = 'if (expected.empty() || Utf8(dump.filename()) != expected) return DumpProblem::kNotThisLog;'; New = 'if (expected.empty()) return DumpProblem::kNotThisLog;' },
        @{ Name = 'MDMP magic validation removed'; File = 'launcher/bug_report.cpp'; Old = 'if (size < kFewestDumpBytes || !StartsLikeMinidump(dump)) return DumpProblem::kNotADump;'; New = 'if (size < kFewestDumpBytes) return DumpProblem::kNotADump;' },
        @{ Name = 'dump max size limit removed'; File = 'launcher/bug_report.cpp'; Old = 'if (size > kMostDumpBytes) return DumpProblem::kTooLarge;'; New = 'if (false) return DumpProblem::kTooLarge;' },
        @{ Name = 'dump age pairing window removed'; File = 'launcher/bug_report.cpp'; Old = 'if (std::abs(std::difftime(dump_time, log.written)) > kDumpPairingSeconds) return DumpProblem::kNotThisLog;'; New = 'if (false) return DumpProblem::kNotThisLog;' },
        @{ Name = 'dump attachment becomes mandatory'; File = 'launcher/bug_report.cpp'; Old = '    const bool dump_valid = log.crash && CheckCrashDump(log, log.crash->path, now) == DumpProblem::kNone;'; New = '    const bool dump_valid = log.crash && CheckCrashDump(log, log.crash->path, now) == DumpProblem::kNone; if (!dump_valid) return std::nullopt;' },
        @{ Name = 'generic crash mislabeled as device lost'; File = 'launcher/bug_report.cpp'; Old = '    } else if (saw_crash_line) {'; New = '    } else if (saw_crash_line) { diag.device_lost = true; diag.device_lost_reason = "fabricated";' },
        @{ Name = 'later device-lost line replaces authoritative root cause'; File = 'launcher/bug_report.cpp'; Old = '        if (line.starts_with("GPU device lost: first detected in ")) {' + "`n" + '            if (!diag.device_lost) {'; New = '        if (line.starts_with("GPU device lost: first detected in ")) {' + "`n" + '            if (true) {' },
        @{ Name = 'runtime_log ignores .dmp when choosing next session number'; File = 'src/runtime_log.c'; Old = '    if (!strcmp(rest, ".log") || !strcmp(rest, ".dmp")) return number;'; New = '    if (!strcmp(rest, ".log")) return number;' }
    )

    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        & $newWorkspace $work
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutExe = Join-Path $work 'crash_dump_tests.exe'
        $build = { & $buildIn $work $mutExe @('-w') }.GetNewClosure()
        if (Test-MutationDetected $mutation.Name $build $mutExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "crash-dump mutations: $detected of $($mutations.Count) detected"
    exit $result
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
