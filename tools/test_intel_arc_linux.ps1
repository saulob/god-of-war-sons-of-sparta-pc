# Issue #6 (Linux, Core Ultra 7 255H / Intel Graphics ARL, Mesa ANV on Xe KMD, v0.1.0 log ending after "semaphore id=2").
# 1. The reporter's log on the production headers: the ARL capabilities taken from its own extension list, the route they
#    give, the silent v0.1.0 end identical to the issue #5 Steam Deck log, and a session outcome that is never normal.
# 2. Source contracts of every fatal path that must reach the log file on Linux, and of the issue #5 main stack fix.
# 3. Under WSL: a SIGSEGV right after "semaphore id=2" through the real log pipe writes CRASH, phase, stage and checkpoint.
# 4. Under WSL: the real shader recompiler on the ARL route (vertex, fragment, compute) passes spirv-val.
# Mutated copies must fail. No game, launcher, window or GPU; the repository is only read.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$failures = 0
function Report([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name"; $script:failures++ }
}
function Read-Source([string]$Relative) { [IO.File]::ReadAllText((Join-Path $root $Relative)).Replace("`r`n", "`n") }

# --- 1. the log on the production headers ---
$compat = 'gpu/shadps4/video_core/renderer_vulkan/vk_gpu_compat.h'
$outcome = 'launcher/session_outcome.h'
$mutations = @(
    @{ Name = 'the ARL device is treated as barycentric'; File = $compat; Old = '    } else if (caps.fragment_shader_barycentric) {'; New = '    } else if (true) {' },
    @{ Name = 'the AMD explicit-vertex path is enabled on an Intel device'; File = $compat; Old = '    if (caps.amd_explicit_vertex_parameter) {'; New = '    if (caps.amd_explicit_vertex_parameter || caps.vendor_id == 0x8086) {' },
    @{ Name = 'the driver-interpolation route loses the integer fix'; File = $compat; Old = "    route.integer_fix =`n        route.interpolation != Interpolation::AmdExplicit && !route.manual_interpolation;"; New = '    route.integer_fix = false;' },
    @{ Name = 'a log that stops while starting is read as a normal exit'; File = $outcome; Old = '    if (normal_exit) return with("normal_exit", where);'; New = '    if (normal_exit || !first_present) return with("normal_exit", where);' },
    @{ Name = "the renderer's own first present counts as the game's first frame"; File = $outcome; Old = '        if (line == "Runtime: first present" ||'; New = '        if (line.starts_with("Startup: first present") || line == "Runtime: first present" ||' },
    @{ Name = 'a crash report is not read as a crash'; File = $outcome; Old = 'const bool fatal_line = line.starts_with("CRASH:") || line.starts_with("STOP: ");'; New = 'const bool fatal_line = line.starts_with("STOP: ");' }
)
$code = Invoke-HostMutationTest -Name 'intel_arc_linux' -Root $root -Test 'tests/intel_arc_linux_tests.cpp' `
    -Copy @($compat, $outcome, 'tests/fixtures/issue_6_v010_linux_intel_arl_log.txt', 'tests/fixtures/issue_5_v010_steam_deck_tail_log.txt') `
    -IncludeDirs @('launcher') -Mutations $mutations
if ($code -ne 0) { $failures++ }

# --- 2. fatal paths and the issue #5 fix, as source contracts ---
$loader = Read-Source 'src\loader.c'
$assert = Read-Source 'gpu\shim\common\assert.cpp'
$deviceLost = Read-Source 'gpu\shim\common\device_lost.cpp'
$dcpgpu = Read-Source 'gpu\shim\dcpgpu.cpp'
$runtimeH = Read-Source 'src\runtime.h'
$runtimeC = Read-Source 'src\runtime.c'
$module = Read-Source 'src\fault_signal_posix.c'
$spirvTest = Read-Source 'tests\interpolation_fallback_spirv_tests.cpp'

function Get-Between([string]$Text, [string]$Start, [string]$End) {
    $a = $Text.IndexOf($Start, [StringComparison]::Ordinal)
    if ($a -lt 0) { return '' }
    $b = $Text.IndexOf($End, $a + $Start.Length, [StringComparison]::Ordinal)
    if ($b -lt 0) { return '' }
    return $Text.Substring($a, $b - $a)
}
function Test-Before([string]$Text, [string[]]$Ordered) {
    $at = -1
    foreach ($item in $Ordered) {
        $next = $Text.IndexOf($item, [Math]::Max($at, 0), [StringComparison]::Ordinal)
        if ($next -lt 0 -or $next -le $at) { return $false }
        $at = $next
    }
    return $true
}
function Test-Handler([string]$Text) {
    $body = Get-Between $Text 'static void posix_fault_handler(int sig, siginfo_t *si, void *uc_void) {' 'static void install_posix_fault_handlers'
    return ($body -and -not $body.Contains('signal(sig, SIG_DFL)') -and
        (Test-Before $body @('if (!sos_fault_begin()) sos_fault_end(sig);', 'sos_fault_write_header(2, sig, si);',
            'sos_fault_write_context(2);', 'sos_fault_write_thread(2);', 'sos_fault_write_location(', 'runtime_log_drain();',
            'dcpgpu_first_frame_summary(', '"Crash: graphics progress: %s\n"', 'dcpgpu_first_ops_summary(',
            '"Crash: first GPU operations: %s\n"', 'runtime_history_dump();', 'dcpgpu_crash_trace("fatal signal");',
            'runtime_log_drain();', 'sos_fault_end(sig);')))
}
function Test-Assert([string]$Text) {
    $body = Get-Between $Text 'void assert_fail_impl() {' '[[noreturn]] void unreachable_impl()'
    return (Test-Before $body @('Common::GpuTrace::Dump("GPU assertion failed");', '"STOP: GPU library assertion failed',
        'Common::DeviceLost::Global().ExitFlush()', 'flush();', 'std::_Exit(23);'))
}
function Test-ExitFlushHook([string]$Text) {
    $body = Get-Between $Text 'Common::DeviceLost::Global().SetExitFlush([] {' '});'
    return (Test-Before $body @('sos_startup_phase_current()', 'runtime_log_mark_crash();', 'runtime_log_flush();'))
}
function Test-RuntimeExit([string]$Header, [string]$Source) {
    $exit = Get-Between $Header 'static inline __attribute__((noreturn)) void runtime_exit(int status) {' '}'
    $request = Get-Between $Source 'void runtime_log_exit_request(const char *api, int status, uintptr_t caller) {' '}'
    return ((Test-Before $exit @('runtime_log_exit_request("runtime_exit"', '_Exit(status);')) -and
        (Test-Before $request @('sos_startup_phase_current()', 'runtime_log_flush();')))
}
function Test-MainStack([string]$Text) {
    return ($Text.Contains('runtime_thread_enter_on_stack((void *)(image + entry), &params, (void *)guest_exit, stack, MAIN_STACK);') -and
        ([regex]::Matches($Text, 'sos_guest_enter\(')).Count -eq 1)
}

Report 'POSIX fault handler: CRASH header, startup phase, thread and location drained before graphics progress, first GPU operations, PS4 calls and GPU trace' (Test-Handler $loader)
Report 'mutation detected: the v0.1.0 handler (default action, no report)' (-not (Test-Handler ($loader.Replace('    if (!sos_fault_begin()) sos_fault_end(sig);', "    signal(sig, SIG_DFL);`n    raise(sig);`n    if (!sos_fault_begin()) sos_fault_end(sig);"))))
Report 'mutation detected: the report without the startup phase' (-not (Test-Handler ($loader.Replace("    sos_fault_write_context(2);`n", ''))))
Report 'mutation detected: the report without the first GPU operations' (-not (Test-Handler ($loader.Replace('"Crash: first GPU operations: %s\n"', '"Crash: %s\n"'))))
Report 'mutation detected: the essential report is not drained before the best-effort part' (-not (Test-Handler ($loader.Replace("                             guest ? (unsigned long)(rip - (uintptr_t)image) : 0);`n    runtime_log_drain();", "                             guest ? (unsigned long)(rip - (uintptr_t)image) : 0);"))))
Report 'GPU assertion: trace and STOP line, then the exit flush, then _Exit(23)' (Test-Assert $assert)
Report 'mutation detected: a GPU assertion that ends without the exit flush (v0.1.0)' (-not (Test-Assert ($assert.Replace("    if (auto* flush = Common::DeviceLost::Global().ExitFlush()) {`n        flush();`n    }`n", ''))))
Report 'the exit flush hook writes the phase, marks the crash and drains the log' (Test-ExitFlushHook $dcpgpu)
Report 'mutation detected: an exit flush hook that does not drain the log' (-not (Test-ExitFlushHook ($dcpgpu.Replace("        runtime_log_mark_crash();`n        runtime_log_flush();", '        runtime_log_mark_crash();'))))
Report 'a device loss ends through the same exit flush' ((Get-Between $deviceLost 'void EndProcess(const char* summary, int status) {' 'void StayHere()').Contains("flush();`n    }`n    std::_Exit(status);"))
Report 'runtime_exit (every STOP of the runtime) logs the request with the phase and drains the log before _Exit' (Test-RuntimeExit $runtimeH $runtimeC)
Report 'mutation detected: runtime_exit without the drained request line' (-not (Test-RuntimeExit $runtimeH ($runtimeC.Replace("           api,status,where,sos_startup_phase_current(),runtime_module_operation());`n    runtime_log_flush();", "           api,status,where,sos_startup_phase_current(),runtime_module_operation());"))))
Report 'SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT are reported' ($module.Contains('static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};'))
Report 'issue #5 fix in place: the guest main thread enters on its own stack with matching bounds' (Test-MainStack $loader)
Report 'mutation detected: the guest main entered with sos_guest_enter (host stack bounds, v0.1.0)' (-not (Test-MainStack ($loader.Replace('runtime_thread_enter_on_stack((void *)(image + entry), &params, (void *)guest_exit, stack, MAIN_STACK);', 'sos_guest_enter((void *)(image + entry), &params, (void *)guest_exit, NULL);'))))
Report 'the SPIR-V test compiles the issue #6 ARL route with exactly the log''s capabilities' ($spirvTest.Contains('{"Intel Arc 140T ARL (fallback, SOS-BXG9UT, issue #6)", {.vendor_id = 0x8086, .provoking_vertex = true}, false, 32},'))

# --- 3. a fatal signal after "semaphore id=2", through the real log pipe (WSL) ---
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$gcc = $false
if ($wsl) { $gcc = ((& wsl.exe -e sh -c 'command -v gcc >/dev/null 2>&1 && echo gcc' 2>$null) -join '').Trim() -eq 'gcc' }
if (-not $gcc) {
    Write-Host '  [n/a]  fatal signal after semaphore id=2: no WSL with gcc (not applicable, not a failure)'
} else {
    function Convert-ToWslPath([string]$Path) {
        return ((& wsl.exe -e wslpath -a ($Path.Replace('\', '/'))) | Select-Object -First 1).Trim()
    }
    $parent = 'E:\ClaudeTemp\opencode'
    if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
    $parent = [IO.Path]::GetFullPath($parent)
    $temp = [IO.Path]::GetFullPath((Join-Path $parent ('sos_intel_arc_linux_' + [guid]::NewGuid().ToString('N'))))
    if (-not $temp.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $temp.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "refusing scratch folder $temp" }
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        $sources = @('src/fault_signal.h', 'src/fault_signal_posix.c', 'src/runtime_log.h', 'src/runtime_log_posix.c',
            'src/runtime_session_log.c', 'src/startup_phase.h', 'src/startup_phase.c', 'tests/fault_signal_tests.c')
        function Invoke-Issue6([string]$Dir) {
            $dirWsl = Convert-ToWslPath $Dir
            $exe = '/tmp/sos_issue6_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
            $script = "cd '$dirWsl' && gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I src tests/fault_signal_tests.c src/fault_signal_posix.c src/runtime_log_posix.c src/runtime_session_log.c src/startup_phase.c -o '$exe' || exit 97; '$exe' --only issue6; code=`$?; rm -f '$exe'; exit `$code"
            $output = & wsl.exe -e sh -c $script 2>&1
            return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($output) }
        }
        $base = Join-Path $temp 'base'
        foreach ($relative in $sources) { Copy-ToWorkspace $base (Join-Path $root $relative) $relative | Out-Null }
        $run = Invoke-Issue6 $base
        $run.Lines | ForEach-Object { Write-Host "  $_" }
        if ($run.Code -eq 97) { throw 'the fault signal test did not compile under WSL' }
        Report 'SIGSEGV after semaphore id=2: the semaphore line, then CRASH, phase guest entry, last stage, checkpoint and thread in the file' ($run.Code -eq 0)

        $posixMutations = @(
            @{ Name = 'SIGSEGV is not handled (v0.1.0 silence)'; File = 'src/fault_signal_posix.c'; Old = '{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}'; New = '{SIGBUS, SIGILL, SIGFPE, SIGABRT}' },
            @{ Name = 'the report lacks the startup phase'; File = 'src/fault_signal_posix.c'; Old = "    write_string(fd, sos_startup_phase_current());`n    write_text(fd, `"\n`", 1);"; New = "    write_text(fd, `"\n`", 1);" },
            @{ Name = 'the report lacks the last checkpoint'; File = 'src/startup_phase.c'; Old = '    at = append(out, size, at, note[0] ? note : "none");'; New = '    at = append(out, size, at, note[0] ? "none" : "none");' }
        )
        $detected = 0
        $index = 0
        foreach ($mutation in $posixMutations) {
            $work = Join-Path $temp ('mut' + $index)
            foreach ($relative in $sources) { Copy-ToWorkspace $work (Join-Path $root $relative) $relative | Out-Null }
            Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
            $mutated = Invoke-Issue6 $work
            if ($mutated.Code -eq 97) { Write-Host "  [fail] mutation did not compile: $($mutation.Name)"; $failures++ }
            elseif ($mutated.Code -ne 0) { Write-Host "  mutation detected: $($mutation.Name)"; $detected++ }
            else { Write-Host "  [fail] mutation was not detected: $($mutation.Name)"; $failures++ }
            $index++
        }
        Write-Host "issue6 fatal signal mutations: $detected of $($posixMutations.Count) detected"
    } finally {
        if ($temp.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# --- 4. the ARL route on the real shader recompiler (WSL, spirv-val) ---
& (Join-Path $PSScriptRoot 'test_interpolation_spirv.ps1') -BaseOnly
if ($LASTEXITCODE -ne 0) { $failures++ }

exit $failures
