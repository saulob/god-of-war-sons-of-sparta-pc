# Headless proof of the umtx semaphore operations on the production source (src\runtime_umtx.c): the timed semaphore wait of
# CUSA57767 1.01 blocks instead of spinning, posts and wakes end it, and failures follow the POSIX errno contract. Mutated
# copies must fail the same test.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_umtx_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$result = 0

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang.exe'
    $flags = @('--target=x86_64-w64-mingw32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000007',
        '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'gpu\shim'))
    $test = Join-Path $root 'tests\umtx_tests.c'
    $source = Join-Path $root 'src\runtime_umtx.c'

    $exe = Join-Path $temp 'umtx_tests.exe'
    & $clang @flags $test $source '-pthread' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the umtx test' }
    $run = Invoke-TimedProgram $exe 60
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out' } | ForEach-Object { Write-Host $_ }
    $result = $run.Code

    $mutations = @(
        @{ Name = 'the semaphore wait keeps a wrong operation number'; Old = 'UMTX_OP_SEM_WAIT = 19, UMTX_OP_SEM_WAKE = 20'; New = 'UMTX_OP_SEM_WAIT = 22, UMTX_OP_SEM_WAKE = 19' },
        @{ Name = 'the timed wait returns at once'; Old = 'return umtx_sem_wait(obj, uaddr2);'; New = 'return 0;' },
        @{ Name = 'the timed wait watches the waiters flag'; Old = '(unsigned char *)usem + 4, 0, timeout'; New = '(unsigned char *)usem, 1, timeout' },
        @{ Name = 'the waiter is not marked'; Old = 'store32(usem, 1);'; New = '(void)usem;' },
        @{ Name = 'a timeout is reported as success'; Old = '? posix_error(ETIMEDOUT) : 0'; New = '? 0 : 0' },
        @{ Name = 'the wake operation wakes nobody'; Old = 'wake_on((unsigned char *)obj + 4, 1);'; New = ';' },
        @{ Name = 'an empty trywait leaves errno alone'; Old = 'return posix_error(EAGAIN);'; New = 'return EAGAIN;' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        $copy = Copy-ToWorkspace $work $source 'runtime_umtx.c'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'umtx_tests.exe'
        $build = { & $clang @flags '-w' $test $copy '-pthread' '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "umtx mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
