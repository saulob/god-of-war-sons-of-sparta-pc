# Headless proof of guest thread cancellation on the production runtime thread layer (src\runtime_thread.c): scePthreadCancel
# is deferred. The target exits at its next cancellation point or finishes naturally; the caller never blocks, the process
# never stops, and nothing is unwound or terminated from the host. Mutated copies, among them the old fatal behaviour of
# cancelling an active thread, must fail the same test. No game, launcher, GPU or window is started.
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
    $compat = Join-Path $root 'src\compat_win.c'

    $exe = Join-Path $temp 'thread_runtime_tests.exe'
    & $clang @flags $source $threadSource $compat $stubs '-lbcrypt' '-pthread' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the thread runtime test' }
    $run = Invoke-TimedProgram $exe 120
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out' } | ForEach-Object { Write-Host $_ }
    $result = $run.Code

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
        $build = { & $clang @flags '-w' $source $copy $compat $stubs '-lbcrypt' '-pthread' '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "thread runtime mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
