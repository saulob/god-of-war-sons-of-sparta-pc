# Stack bounds of the guest main thread (issue #5): on POSIX the guest main thread runs on its own runtime_low_map stack
# and the IL2CPP collector must read that stack's bounds through scePthreadAttrGet/Getstackaddr/Getstacksize. The POSIX
# test runs the production thread, memory, extra and guest entry sources under WSL; the Windows test keeps the TEB
# contract; source gates pin the loader wiring; mutated copies must fail. No game, launcher, GPU or window is started.
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
function Read-Source([string]$Relative) { [IO.File]::ReadAllText((Join-Path $root $Relative)).Replace("`r`n", "`n") }

$loader = Read-Source 'src\loader.c'
$thread = Read-Source 'src\runtime_thread.c'
$header = Read-Source 'src\runtime.h'

function Test-LoaderWiring([string]$Text) {
    $windows = 'sos_guest_enter((void *)(image + entry), &params, (void *)guest_exit, NULL);'
    $at = $Text.IndexOf($windows, [StringComparison]::Ordinal)
    if ($at -lt 0) { return $false }
    $posixStart = $Text.IndexOf("#else`n", $at, [StringComparison]::Ordinal)
    $posixEnd = if ($posixStart -ge 0) { $Text.IndexOf('#endif', $posixStart, [StringComparison]::Ordinal) } else { -1 }
    if ($posixEnd -lt 0) { return $false }
    $posix = $Text.Substring($posixStart, $posixEnd - $posixStart)
    $windowsBranch = $Text.Substring($at - 32, 32)
    $call = 'runtime_thread_enter_on_stack((void *)(image + entry), &params, (void *)guest_exit, stack, MAIN_STACK);'
    $attach = $Text.IndexOf('runtime_thread_attach_main();', [StringComparison]::Ordinal)
    $initialized = $Text.IndexOf('sos_startup_milestone("linked modules initialized");', [StringComparison]::Ordinal)
    $entered = $Text.IndexOf($call, [StringComparison]::Ordinal)
    return ($windowsBranch.Contains("#ifdef _WIN32`n") -and
        $posix.Contains('unsigned char *stack = runtime_low_map(MAIN_STACK, PROT_READ | PROT_WRITE);') -and
        $posix.Contains($call) -and -not $posix.Contains('sos_guest_enter(') -and
        ([regex]::Matches($Text, 'runtime_thread_enter_on_stack\(')).Count -eq 1 -and
        $attach -ge 0 -and $initialized -gt $attach -and $entered -gt $initialized)
}
function Test-PosixOnly([string]$Thread, [string]$Header) {
    return ($Thread -match "#ifndef _WIN32\n/\*[^\n]*\*/\nvoid \*runtime_thread_enter_on_stack\(" -and
        $Header -match "#ifndef _WIN32\n/\*[^\n]*\*/\nvoid \*runtime_thread_enter_on_stack\(" -and
        $Thread.Contains('*high=base;') -and $Thread.Contains('if (!pthread_getattr_np(pthread_self(),&attr))'))
}
Report 'the loader enters the guest main on its low stack through runtime_thread_enter_on_stack, after the module initializers, with the size it mapped' (Test-LoaderWiring $loader)
Report 'Windows keeps the host stack: the loader passes no stack there and the stack switch exists only on POSIX' (Test-PosixOnly $thread $header)
$v010 = 'sos_guest_enter((void *)(image + entry), &params, (void *)guest_exit, stack + MAIN_STACK - 64);'
$call = 'runtime_thread_enter_on_stack((void *)(image + entry), &params, (void *)guest_exit, stack, MAIN_STACK);'
Report 'mutation detected: the v0.1.0 loader (stack switch without bounds) fails the gate' (-not (Test-LoaderWiring $loader.Replace($call, $v010)))
Report 'mutation detected: bounds switched before the module initializers fail the gate' (-not (Test-LoaderWiring $loader.Replace(
    "    $call", '    (void)stack;').Replace('runtime_thread_attach_main();', "runtime_thread_attach_main();`n        $call")))
Report 'mutation detected: a guest stack size other than the mapped one fails the gate' (-not (Test-LoaderWiring $loader.Replace($call, $call.Replace('stack, MAIN_STACK)', 'stack, MAIN_STACK / 2)'))))
Report 'mutation detected: the Windows loader given a stack fails the gate' (-not (Test-LoaderWiring $loader.Replace(
    'sos_guest_enter((void *)(image + entry), &params, (void *)guest_exit, NULL);', 'sos_guest_enter((void *)(image + entry), &params, (void *)guest_exit, stack + MAIN_STACK - 64);')))
Report 'mutation detected: the stack switch compiled for Windows fails the gate' (-not (Test-PosixOnly ($thread -replace "#ifndef _WIN32\n(/\*[^\n]*\*/\nvoid \*runtime_thread_enter_on_stack\()", '$1') $header))

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$parent = (Resolve-Path -LiteralPath $parent).Path
$temp = Join-Path $parent ('sos_linux_main_stack_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
function Remove-Temp {
    $full = [IO.Path]::GetFullPath($temp)
    $repo = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $full -Leaf) -notlike 'sos_linux_main_stack_*' -or $full.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase) -or
        $repo.StartsWith($full.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "  [fail] refusing to remove unexpected path $full"
        $script:failures++
        return
    }
    Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
}

$test = Join-Path $root 'tests\linux_main_stack_tests.c'
$threadSource = Join-Path $root 'src\runtime_thread.c'
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang.exe'
    $flags = @('--target=x86_64-w64-mingw32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000007',
        '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'gpu\shim'))
    $others = @((Join-Path $root 'src\runtime_extra.c'), (Join-Path $root 'src\guest_entry.c'), (Join-Path $root 'src\compat_win.c'),
        (Join-Path $PSScriptRoot 'startup_profile_stubs.c'))
    $exe = Join-Path $temp 'linux_main_stack_windows.exe'
    & $clang @flags $test $threadSource @others '-lbcrypt' '-pthread' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the Windows stack bounds test' }
    $run = Invoke-TimedProgram $exe 120
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out' } | ForEach-Object { Write-Host $_ }
    if ($run.Code -ne 0) { $failures++ }

    $windowsMutation = Join-Path $temp 'win_mut'
    $copy = Copy-ToWorkspace $windowsMutation $threadSource 'runtime_thread.c'
    Edit-WorkspaceText $copy '    *high=base;' '    *high=base-4096;'
    $mutatedExe = Join-Path $windowsMutation 'linux_main_stack_windows.exe'
    $build = { & $clang @flags '-w' $test $copy @others '-lbcrypt' '-pthread' '-o' $mutatedExe }
    if (-not (Test-MutationDetected 'Windows main thread bounds no longer the TEB stack' $build $mutatedExe)) { $failures++ }

    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    $posixHost = $false
    if ($wsl) {
        $probe = & wsl.exe -e sh -c 'command -v gcc >/dev/null 2>&1 && echo gcc' 2>$null
        $posixHost = ($probe -join '').Trim() -eq 'gcc'
    }
    if (-not $posixHost) {
        Write-Host '  [n/a]  POSIX guest main stack: this host has no POSIX environment with gcc (not applicable, not a failure)'
        exit $failures
    }
    function Convert-ToWslPath([string]$Path) {
        $converted = & wsl.exe -e wslpath -a ($Path.Replace('\', '/')) 2>$null
        if ($LASTEXITCODE -ne 0) { throw "wslpath failed for $Path" }
        return ($converted | Select-Object -First 1).Trim()
    }
    $rootWsl = Convert-ToWslPath $root
    function Invoke-Posix([string]$ThreadFile, [string]$Quiet = '') {
        $threadWsl = Convert-ToWslPath $ThreadFile
        $name = '/tmp/sos_main_stack_' + [guid]::NewGuid().ToString('N').Substring(0, 10)
        $script = "cd '$rootWsl' || exit 98; gcc -std=gnu11 -Wall -Wextra -Werror $Quiet -pthread -I src -I launcher -I gpu/shim tests/linux_main_stack_tests.c '$threadWsl' src/runtime_memory.c src/runtime_extra.c src/guest_entry.c tools/startup_profile_stubs.c -o '$name' || exit 97; '$name'; code=`$?; rm -f '$name'; exit `$code"
        $output = & wsl.exe -e sh -c $script 2>&1
        return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($output) }
    }
    $posix = Invoke-Posix $threadSource
    $posix.Lines | Where-Object { $_ -match '^\s+(\[(ok|fail)\]|host bounds|guest rsp)|tests:' } | ForEach-Object { Write-Host $_ }
    if ($posix.Code -eq 97) { throw 'the POSIX stack bounds test did not compile under WSL' }
    if ($posix.Code -ne 0) { $failures++ }

    $mutations = @(
        @{ Name = 'the bounds update before the stack switch is removed (v0.1.0)'; Old = "    t->stack_low=(uintptr_t)stack; t->stack_high=(uintptr_t)stack+size;`n"; New = '' },
        @{ Name = 'the low bound is wrong'; Old = 't->stack_low=(uintptr_t)stack;'; New = 't->stack_low=(uintptr_t)stack+4096;' },
        @{ Name = 'the high bound is past the mapping'; Old = 't->stack_high=(uintptr_t)stack+size;'; New = 't->stack_high=(uintptr_t)stack+size+4096;' },
        @{ Name = 'the size is wrong'; Old = 't->stack_high=(uintptr_t)stack+size;'; New = 't->stack_high=(uintptr_t)stack+size/2;' },
        @{ Name = 'the host bounds are not restored after the guest entry'; Old = "    t->stack_low=host_low; t->stack_high=host_high;`n"; New = '' },
        @{ Name = 'a guest thread is given the main thread''s stack'; Old = "    GuestThread *t=runtime_thread_current();`n    *address=(void *)t->stack_low;"; New = "    GuestThread *t=threads; while (t->next) t=t->next;`n    *address=(void *)t->stack_low;" }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        $copy = Copy-ToWorkspace $work $threadSource 'runtime_thread.c'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutated = Invoke-Posix $copy '-w'
        if ($mutated.Code -eq 97) { Write-Host "  [fail] mutation did not compile: $($mutation.Name)"; $failures++ }
        elseif ($mutated.Code -ne 0 -and ($mutated.Lines -match '\[fail\]')) { Write-Host "  mutation detected: $($mutation.Name)"; $detected++ }
        else { Write-Host "  [fail] mutation was not detected: $($mutation.Name)"; $failures++ }
    }
    Write-Host "linux-main-stack mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Temp
}
exit $failures
