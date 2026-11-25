# Headless proof of the save backup notification on the production save implementation (src\runtime_savedata.c), including
# a virtual-time model of the save flow of CUSA57767 1.01. Mutated copies must fail the same test. A disposable user folder
# is used; no game, launcher or real save data is touched.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_save_backup_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$result = 0

function Set-SaveFolder([string]$Name) {
    $folder = Join-Path $temp $Name
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $env:SOS_TEST_SAVE_DIR = $folder
}

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang.exe'
    $flags = @('--target=x86_64-w64-mingw32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000007',
        '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'gpu\shim'))
    $test = Join-Path $root 'tests\savedata_backup_tests.c'
    $saveSource = Join-Path $root 'src\runtime_savedata.c'
    $compat = Join-Path $root 'src\compat_win.c'

    $exe = Join-Path $temp 'save_backup_tests.exe'
    & $clang @flags $test $saveSource $compat '-lbcrypt' '-pthread' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the save backup test' }
    Set-SaveFolder 'user'
    $run = Invoke-TimedProgram $exe 120
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|game model:|timed out' } | ForEach-Object { Write-Host $_ }
    $result = $run.Code

    $mutations = @(
        @{ Name = 'the notification arrives with the unmount'; Old = 'if (!events[event_head].announced || '; New = 'if (0 && ' },
        @{ Name = 'a backup stays busy forever'; Old = 'now-events[event_head].announced<BACKUP_BUSY_MS'; New = '1' },
        @{ Name = 'a poll woken early delivers the notification'; Old = 'now-events[event_head].announced<BACKUP_BUSY_MS'; New = '0' },
        @{ Name = 'a busy poll covers only the first backup'; Old = 'i<event_count;++i) {'; New = 'i<1;++i) {' },
        @{ Name = 'a reused event slot skips the busy poll'; Old = '    memset(event,0,sizeof(*event));
    event->type=type;'; New = '    event->type=type;' },
        @{ Name = 'the notification is delivered again'; Old = '    --event_count;
    pthread_mutex_unlock(&lock);
    memset(event'; New = '    pthread_mutex_unlock(&lock);
    memset(event' },
        @{ Name = 'an empty queue is reported as busy'; Old = 'if (!event_count) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }'; New = 'if (!event_count) { pthread_mutex_unlock(&lock); return ERR_EVENT_BUSY; }' },
        @{ Name = 'the notification loses the backup error'; Old = 'event_push(EVENT_UMOUNT_BACKUP,backup_error,'; New = 'event_push(EVENT_UMOUNT_BACKUP,0,' },
        @{ Name = 'the backup is not written'; Old = 'if (backup_save(host)) backup_error=ERR_INTERNAL;'; New = ';' },
        @{ Name = 'a plain unmount posts a notification'; Old = '    if (with_backup) {'; New = '    if (with_backup || 1) {' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $name = 'mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
        $work = Join-Path $temp $name
        $copy = Copy-ToWorkspace $work $saveSource 'runtime_savedata.c'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'save_backup_tests.exe'
        Set-SaveFolder (Join-Path $name 'user')
        $build = { & $clang @flags '-w' $test $copy $compat '-lbcrypt' '-pthread' '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "save backup mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
