# SIGPIPE policy of the POSIX loader. The source gate runs everywhere: the loader installs the policy at the start of main
# on non-Windows targets only. The behaviour test (a write to a socketpair whose peer closed: killed by default, EPIPE with
# the startup policy, unrelated signals untouched) needs a POSIX host; on Windows it runs under WSL when a gcc is there
# and otherwise reports as not applicable. A copy of the policy without SIG_IGN must fail it. No network, no game.
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

$header = Join-Path $root 'src\sos_posix_signals.h'
$loader = [IO.File]::ReadAllText((Join-Path $root 'src\loader.c')).Replace("`r`n", "`n")
$policy = [IO.File]::ReadAllText($header).Replace("`r`n", "`n")

function Test-LoaderInstallsPolicy([string]$Text) {
    $mainAt = $Text.IndexOf('int main(int argc, char **argv) {', [StringComparison]::Ordinal)
    $callAt = $Text.IndexOf("#ifndef _WIN32`n    sos_ignore_sigpipe();`n#endif", [StringComparison]::Ordinal)
    $firstUse = $Text.IndexOf('runtime_memory_reserve();', [StringComparison]::Ordinal)
    return ($mainAt -ge 0 -and $callAt -gt $mainAt -and $callAt -lt $firstUse)
}
Report 'the loader installs the SIGPIPE policy at the start of main, on POSIX targets only' (Test-LoaderInstallsPolicy $loader)
Report 'mutation detected: a loader without the startup call fails the source gate' (-not (Test-LoaderInstallsPolicy $loader.Replace("#ifndef _WIN32`n    sos_ignore_sigpipe();`n#endif`n", '')))
Report 'mutation detected: a call made on Windows too fails the source gate' (-not (Test-LoaderInstallsPolicy $loader.Replace("#ifndef _WIN32`n    sos_ignore_sigpipe();`n#endif", '    sos_ignore_sigpipe();')))
Report 'the loader includes the policy header' ($loader.Contains('#include "sos_posix_signals.h"'))
Report 'the policy ignores SIGPIPE and nothing else, and is not compiled on Windows' (
    $policy.Contains('#ifndef _WIN32') -and $policy.Contains('sigaction(SIGPIPE, &ignore, NULL)') -and
    $policy.Contains('ignore.sa_handler = SIG_IGN;') -and ([regex]::Matches($policy, 'sigaction\(').Count -eq 1))
$winSources = @('compat_win.c', 'compat_win.h') | ForEach-Object { [IO.File]::ReadAllText((Join-Path $root "src\$_")) }
Report 'no Windows source path uses the POSIX signal API for it' (-not (($winSources -join "`n") -match 'SIGPIPE|sos_ignore_sigpipe'))

function Test-PosixGpuFaults([string]$Text) {
    $enabled = [regex]::Match($Text, 'dcpgpu_init\(&gpu\) == 0\) \{\n\s*gpu_enabled = 1;').Success
    $handlerAt = $Text.IndexOf('static void posix_fault_handler(', [StringComparison]::Ordinal)
    $handler = if ($handlerAt -ge 0) { $Text.Substring($handlerAt, $Text.IndexOf('static void install_posix_fault_handlers', $handlerAt) - $handlerAt) } else { '' }
    $unblockAt = $handler.IndexOf('pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);', [StringComparison]::Ordinal)
    $jumpAt = $handler.IndexOf('siglongjmp(*buffer, 1);', [StringComparison]::Ordinal)
    return ($enabled -and $unblockAt -ge 0 -and $unblockAt -lt $jumpAt)
}
Report 'GPU page faults are dispatched on every platform and a recovered fault unblocks its signal' (Test-PosixGpuFaults $loader)
Report 'mutation detected: GPU faults dispatched on Windows only fail the source gate' (-not (Test-PosixGpuFaults $loader.Replace("            gpu_enabled = 1;`n", "#ifdef _WIN32`n            gpu_enabled = 1;`n#endif`n")))
Report 'mutation detected: a recovered fault that stays blocked fails the source gate' (-not (Test-PosixGpuFaults $loader.Replace('pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);', '')))

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
    Write-Host '  [n/a]  POSIX socketpair behaviour: this host has no POSIX environment with gcc (not applicable, not a failure)'
    exit $failures
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_sigpipe_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $testWsl = Convert-ToWslPath (Join-Path $root 'tests\sigpipe_tests.c')
    $srcWsl = Convert-ToWslPath (Join-Path $root 'src')
    $tempWsl = Convert-ToWslPath $temp
    function Invoke-Posix([string]$Include, [string]$Name) {
        $script = "set -e; gcc -std=gnu11 -Wall -Wextra -Werror -I '$Include' '$testWsl' -o '/tmp/$Name'; '/tmp/$Name'; code=`$?; rm -f '/tmp/$Name'; exit `$code"
        $output = & wsl.exe -e sh -c $script 2>&1
        return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($output) }
    }
    $name = 'sos_sigpipe_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $run = Invoke-Posix $srcWsl $name
    $run.Lines | ForEach-Object { Write-Host $_ }
    if ($run.Code -ne 0) { $failures++ }

    $work = Join-Path $temp 'mutated'
    $copy = Copy-ToWorkspace $work $header 'sos_posix_signals.h'
    Edit-WorkspaceText $copy '    ignore.sa_handler = SIG_IGN;' '    ignore.sa_handler = SIG_DFL;'
    $mutated = Invoke-Posix (Convert-ToWslPath $work) ($name + 'm')
    if ($mutated.Code -ne 0) { Write-Host '  mutation detected: the startup policy no longer ignores SIGPIPE' }
    else { Write-Host '  [fail] mutation was not detected: the startup policy no longer ignores SIGPIPE'; $failures++ }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $failures
