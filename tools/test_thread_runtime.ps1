# Headless proof of the guest TLS reservation and thread cancellation on the production runtime thread layer
# (src\runtime_thread.c): the two guest TEB indices are reserved once before the launcher, later calls reuse them
# after inline TLS exhaustion, a simulated late index 78 stops safely, and scePthreadCancel is deferred. No game,
# launcher, GPU or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_thread_runtime_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
$testSource = Join-Path $root 'tests\thread_runtime_tests.c'
Copy-Item -LiteralPath $testSource -Destination $source

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang.exe'
    $flags = @('--target=x86_64-w64-mingw32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000007',
        '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'gpu\shim'))
    $stubs = Join-Path $PSScriptRoot 'startup_profile_stubs.c'
    $threadSource = Join-Path $root 'src\runtime_thread.c'
    $entrySource = Join-Path $root 'src\guest_entry.c'
    $compat = Join-Path $root 'src\compat_win.c'
    $tlsDefines = @('-DSOS_TLS_TEST')

    $threadObject = Join-Path $temp 'runtime_thread_tls.obj'
    & $clang @flags @tlsDefines '-c' $threadSource '-o' $threadObject
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the TLS shim runtime thread layer' }
    $exe = Join-Path $temp 'thread_runtime_tests.exe'
    & $clang @flags $source $threadObject $compat $stubs $entrySource '-lbcrypt' '-pthread' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the thread runtime test' }
    $run = Invoke-TimedProgram $exe 120
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out' } | ForEach-Object { Write-Host $_ }
    $result = $run.Code

    $early = Invoke-TimedProgram $exe 120 '--tls-early'
    ($early.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out|reserved guest TCB' } | ForEach-Object { Write-Host $_ }
    if ($early.Code -ne 0) { $result = 1 }
    $late = Invoke-TimedProgram $exe 120 '--tls-late'
    if ($late.Code -eq 21 -and $late.Text -match 'guest thread TEB TLS slot index 78' -and $late.Text -match '\[ok\]\s+late reservation stopped') {
        Write-Host '  [ok]   late TLS index 78 is refused safely'
    } else {
        Write-Host "  [fail] late TLS index 78 was not refused safely (exit $($late.Code))"
        Write-Host $late.Text
        $result = 1
    }

    $loader = Get-Content -LiteralPath (Join-Path $root 'src\loader.c') -Raw
    $runtime = Get-Content -LiteralPath $threadSource -Raw
    $reserve = $loader.IndexOf('runtime_thread_reserve_tls();')
    $launcher = $loader.IndexOf('sos_launcher_run(&launcher)')
    if ($reserve -ge 0 -and $launcher -gt $reserve -and ([regex]::Matches($loader, 'runtime_thread_reserve_tls\(\);').Count -eq 1)) {
        Write-Host '  [ok]   the loader reserves both TLS indices before the launcher starts'
    } else {
        Write-Host '  [fail] the loader reservation is missing or happens after the launcher starts'
        $result = 1
    }
    if ($runtime -match 'InitOnceExecuteOnce' -and $runtime.Contains('const DWORD tcb=TLS_ALLOC();') -and
        $runtime.Contains('const DWORD thread=TLS_ALLOC();') -and
        $runtime -notmatch 'CUSA57767-1\.0[01]' -and $runtime -notmatch 'tls_slot\s*=\s*\d' -and $runtime -notmatch 'thread_slot\s*=\s*\d') {
        Write-Host '  [ok]   the shared host runtime allocates both indices once, without a hardcoded slot'
    } else {
        Write-Host '  [fail] the shared host runtime does not reserve both indices once without a hardcoded slot'
        $result = 1
    }
    $redzone = Get-Content -LiteralPath (Join-Path $root 'src\runtime_redzone.c') -Raw
    $redzoneInstall = $loader.IndexOf('runtime_redzone_install(')
    if ($redzoneInstall -gt $reserve -and $redzone -notmatch 'TlsAlloc|TlsFree|TlsSetValue|TlsGetValue') {
        Write-Host '  [ok]   red-zone initialization reuses the early reservation and allocates no TLS'
    } else {
        Write-Host '  [fail] red-zone initialization is not after the reservation or allocates TLS itself'
        $result = 1
    }

    $mutations = @(
        @{ Name = 'cancelling an active thread is fatal again'; Old = '    int32_t result=0, newly=0;'; New = "    int32_t result=0, newly=0;`n    if (!t->finished) { fputs(`"STOP: scePthreadCancel of a running thread is not supported\n`",stderr); runtime_exit(21); }" },
        @{ Name = 'a cancellation request is dropped'; Old = '        __atomic_store_n(&t->cancel_pending,1,__ATOMIC_RELEASE);'; New = '        (void)0;' },
        @{ Name = 'cancelling a finished thread succeeds'; Old = 'if (t->finished) result=ERR(3);'; New = 'if (0) result=ERR(3);' },
        @{ Name = 'cancel waits for the target to finish'; Old = '    if (newly) {'; New = "    while (!__atomic_load_n(&t->finished,__ATOMIC_ACQUIRE)) sched_yield();`n    if (newly) {" },
        @{ Name = 'a disabled cancel state is ignored'; Old = 'return t && !t->cancel_state && __atomic_load_n(&t->cancel_pending,__ATOMIC_ACQUIRE);'; New = 'return t && __atomic_load_n(&t->cancel_pending,__ATOMIC_ACQUIRE);' },
        @{ Name = 'a canceled thread loses its cancellation result'; Old = '    t->result=CANCELED_RESULT;'; New = '    t->result=NULL;' },
        @{ Name = 'a joined thread can be joined twice'; Old = '    if (t->detached || t->joined) { pthread_mutex_unlock(&lock); return ERR(22); }'; New = '    if (t->detached) { pthread_mutex_unlock(&lock); return ERR(22); }' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + $detected + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $copy = Copy-ToWorkspace $work $threadSource 'runtime_thread.c'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'thread_runtime_tests.exe'
        $mutatedObject = Join-Path $work 'runtime_thread_tls.obj'
        $build = {
            & $clang @flags @tlsDefines '-w' '-c' $copy '-o' $mutatedObject
            if ($LASTEXITCODE -eq 0) { & $clang @flags '-w' $source $mutatedObject $compat $stubs $entrySource '-lbcrypt' '-pthread' '-o' $mutatedExe }
        }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "thread runtime mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
