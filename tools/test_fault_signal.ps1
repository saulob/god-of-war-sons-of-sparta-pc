# POSIX fatal signals (Linux). The source gate runs everywhere: the loader's handler claims one report, writes it without
# stdio, drains the log thread and ends by the signal itself (never _exit or exit, which would hide the signal from the
# parent and suppress the system's core dump), installs SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT again after the
# launcher, and leaves SIGTERM/SIGINT to SDL. The behaviour test (crashing child processes: segv, bus, ill, fpe, abort,
# raised, nested, recovered, concurrent, SIGHUP) needs a POSIX host; on Windows it runs under WSL when gcc is there and
# otherwise reports as not applicable. Mutated copies of fault_signal_posix.c must fail it. No game, no window.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$failures = 0
function Report([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name"; $script:failures++ }
}

$loader = [IO.File]::ReadAllText((Join-Path $root 'src\loader.c')).Replace("`r`n", "`n")
$module = [IO.File]::ReadAllText((Join-Path $root 'src\fault_signal_posix.c')).Replace("`r`n", "`n")
$cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')).Replace("`r`n", "`n")

function Test-LoaderHandler([string]$Text) {
    $start = $Text.IndexOf('static void posix_fault_handler(int sig, siginfo_t *si, void *uc_void) {', [StringComparison]::Ordinal)
    $end = if ($start -ge 0) { $Text.IndexOf('static void install_posix_fault_handlers', $start, [StringComparison]::Ordinal) } else { -1 }
    if ($start -lt 0 -or $end -lt 0) { return $false }
    $body = $Text.Substring($start, $end - $start)
    $claim = $body.IndexOf('if (!sos_fault_begin()) sos_fault_end(sig);', [StringComparison]::Ordinal)
    $recover = $body.IndexOf('siglongjmp(*buffer, 1);', [StringComparison]::Ordinal)
    $gpu = $body.IndexOf('dcpgpu_handle_fault(uc_void, si->si_addr)', [StringComparison]::Ordinal)
    # The essential, async-signal-safe report is drained into the file before the deadline is armed, and nothing that
    # takes the loader lock or uses stdio (dladdr, backtrace, snprintf) runs before that deadline.
    $location = $body.IndexOf('sos_fault_write_location(', [StringComparison]::Ordinal)
    $firstDrain = $body.IndexOf('runtime_log_drain();', [StringComparison]::Ordinal)
    $deadline = $body.IndexOf('sos_fault_arm_deadline(sig, ', [StringComparison]::Ordinal)
    $risky = @('dladdr(', 'backtrace(', 'snprintf(') | ForEach-Object { $body.IndexOf($_, [StringComparison]::Ordinal) } | Where-Object { $_ -ge 0 }
    $riskyFirst = if ($risky) { ($risky | Measure-Object -Minimum).Minimum } else { [int]::MaxValue }
    $ordered = $deadline -gt $claim -and $location -gt $deadline -and $firstDrain -gt $location -and $riskyFirst -gt $firstDrain
    return ($ordered -and $claim -gt $recover -and $claim -gt $gpu -and $recover -ge 0 -and $gpu -ge 0 -and
        $body.Contains('runtime_log_mark_crash();') -and $body.Contains('sos_fault_write_header(2, sig, si);') -and
        $body.Contains('sos_fault_write_context(2);') -and $body.Contains("runtime_log_drain();`n    sos_fault_end(sig);`n}") -and
        -not $body.Contains('_exit(') -and -not $body.Contains('runtime_log_flush()') -and -not $body.Contains('signal(sig, SIG_DFL)'))
}
Report 'the loader handler resolves recoverable faults first, then reports once and ends by the signal' (Test-LoaderHandler $loader)
Report 'mutation detected: a handler that ends with _exit fails the source gate' (-not (Test-LoaderHandler $loader.Replace("runtime_log_drain();`n    sos_fault_end(sig);`n}", "runtime_log_drain();`n    _exit(128 + sig);`n}")))
Report 'mutation detected: a handler that calls dladdr before the deadline is armed fails the source gate' (-not (Test-LoaderHandler $loader.Replace('    sos_fault_arm_deadline(sig, 5);', "    { Dl_info early; dladdr((void *)rip, &early); }`n    sos_fault_arm_deadline(sig, 5);")))
Report 'mutation detected: a handler that never arms the deadline fails the source gate' (-not (Test-LoaderHandler $loader.Replace('    sos_fault_arm_deadline(sig, 5);', '')))
Report 'mutation detected: a handler that reports before the GPU page-tracking path fails the source gate' (-not (Test-LoaderHandler $loader.Replace('    if (sig == SIGSEGV && si) {', "    if (!sos_fault_begin()) sos_fault_end(sig);`n    if (sig == SIGSEGV && si) {")))
Report 'the handlers are installed through the module, at start and again after the launcher, with no raw sigaction left' (
    $loader.Contains('sos_fault_install(posix_fault_handler)') -and -not $loader.Contains('sigaction(SIGSEGV') -and
    ([regex]::Matches($loader, 'install_posix_fault_handlers\(\);')).Count -ge 2)
Report 'the module covers SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT and keeps SIGTERM/SIGINT with SDL' (
    $module.Contains('static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};') -and
    $module.Contains('static const int termination_signals[] = {SIGHUP};'))
Report 'no SA_RESETHAND (recoverable faults come back) and no alternate stack (GPU page tracking needs the thread stack)' (
    $module.Contains('    action.sa_flags = SA_SIGINFO;') -and -not ($module -match 'sa_flags = [^;]*(SA_RESETHAND|SA_ONSTACK)') -and
    -not $module.Contains('sigaltstack('))
Report 'the module is built into the Linux loader' ($cmake.Contains('src/runtime_log_posix.c src/runtime_session_log.c src/fault_signal_posix.c'))

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$posixHost = $false
function Convert-ToWslPath([string]$Path) {
    $converted = & wsl.exe -e wslpath -a ($Path.Replace('\', '/')) 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($converted | Select-Object -First 1).Trim()
}
if ($wsl) {
    $probe = & wsl.exe -e sh -c 'command -v gcc >/dev/null 2>&1 && echo gcc' 2>$null
    $posixHost = ($probe -join '').Trim() -eq 'gcc'
}
if (-not $posixHost) {
    Write-Host '  [n/a]  crashing child processes: this host has no POSIX environment with gcc (not applicable, not a failure)'
    exit $failures
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_fault_signal_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $sources = @('src/fault_signal.h', 'src/fault_signal_posix.c', 'src/runtime_log.h', 'src/runtime_log_posix.c',
        'src/runtime_session_log.c', 'src/startup_phase.h', 'src/startup_phase.c')
    $testWsl = Convert-ToWslPath (Join-Path $root 'tests\fault_signal_tests.c')
    function Invoke-Posix([string]$Dir, [string]$Name) {
        $dirWsl = Convert-ToWslPath $Dir
        $script = "set -e; cd '$dirWsl'; gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I src '$testWsl' src/fault_signal_posix.c src/runtime_log_posix.c src/runtime_session_log.c src/startup_phase.c -o '/tmp/$Name' || exit 97; set +e; '/tmp/$Name'; code=`$?; rm -f '/tmp/$Name'; exit `$code"
        $output = & wsl.exe -e sh -c $script 2>&1
        return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($output) }
    }
    $name = 'sos_fault_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $base = Join-Path $temp 'base'
    foreach ($relative in $sources) { Copy-ToWorkspace $base (Join-Path $root $relative) $relative | Out-Null }
    $run = Invoke-Posix $base $name
    $run.Lines | ForEach-Object { Write-Host $_ }
    if ($run.Code -ne 0) { $failures++ }
    if ($run.Code -eq 97) { throw 'the fault signal test did not compile under WSL' }

    $mutations = @(
        @{ Name = 'the process ends by an exit status instead of its signal'; Old = "    raise(sig);`n    /* Reached only"; New = "    _exit(128 + sig);`n    /* Reached only" },
        @{ Name = 'the default action is not restored before the signal is raised again'; Old = '    sigaction(sig, &standard, NULL);'; New = '' },
        @{ Name = 'SIGABRT is not handled'; Old = '{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}'; New = '{SIGSEGV, SIGBUS, SIGILL, SIGFPE}' },
        @{ Name = 'a second faulting thread writes a second report'; Old = '    if (expected == self) return 0;'; New = '    if (expected == self) return 0;' + "`n    return 1;" },
        @{ Name = 'a blocked report never ends (no deadline)'; Old = '    alarm(seconds);'; New = '    (void)seconds;' },
        @{ Name = 'the deadline ends the process by another signal'; Old = 'sos_fault_end(deadline_signal ? deadline_signal : SIGABRT);'; New = 'sos_fault_end(SIGABRT);' },
        @{ Name = 'the report lacks the last completed stage'; Old = '    write_text(fd, completed, sizeof(completed) - 1);'; New = '    write_text(fd, completed, 0);' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        foreach ($relative in $sources) { Copy-ToWorkspace $work (Join-Path $root $relative) $relative | Out-Null }
        Edit-WorkspaceText (Join-Path $work 'src/fault_signal_posix.c') $mutation.Old $mutation.New
        $mutated = Invoke-Posix $work ($name + 'm')
        if ($mutated.Code -eq 97) { Write-Host "  [fail] mutation did not compile: $($mutation.Name)"; $failures++ }
        elseif ($mutated.Code -ne 0) { Write-Host "  mutation detected: $($mutation.Name)"; $detected++ }
        else { Write-Host "  [fail] mutation was not detected: $($mutation.Name)"; $failures++ }
    }
    Write-Host "fault-signal mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $failures
