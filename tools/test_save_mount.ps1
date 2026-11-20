# Headless proof of the save mount failure diagnostics on the production save implementation (src\runtime_savedata.c):
# sceSaveDataMount and sceSaveDataMount2 log the directory (a placeholder when the name is invalid), the error and the
# mode of every failed mount in the normal log, log nothing for a successful mount, and return the unchanged error. Mutated
# copies must fail the same test. A disposable user folder is used; no game, launcher or real save data is touched.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_save_mount_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$saveDir = Join-Path $temp 'user'
New-Item -ItemType Directory -Force -Path $saveDir | Out-Null
$env:SOS_TEST_SAVE_DIR = $saveDir
$result = 0

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang.exe'
    $flags = @('--target=x86_64-w64-mingw32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000007',
        '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'gpu\shim'))
    $test = Join-Path $root 'tests\savedata_mount_tests.c'
    $saveSource = Join-Path $root 'src\runtime_savedata.c'
    $compat = Join-Path $root 'src\compat_win.c'

    $exe = Join-Path $temp 'save_mount_tests.exe'
    & $clang @flags $test $saveSource $compat '-lbcrypt' '-pthread' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the save mount test' }
    $run = Invoke-TimedProgram $exe 120
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out' } | ForEach-Object { Write-Host $_ }
    $result = $run.Code

    $mutations = @(
        @{ Name = 'the error is left out of the failure log'; Old = 'failed (error=%#x, mode=%#x)\n",name,(unsigned)r,mode);'; New = 'failed (mode=%#x)\n",name,mode);' },
        @{ Name = 'the mode is left out of the failure log'; Old = 'failed (error=%#x, mode=%#x)\n",name,(unsigned)r,mode);'; New = 'failed (error=%#x)\n",name,(unsigned)r);' },
        @{ Name = 'the directory is echoed without validation'; Old = 'dir && valid_name(dir->data,sizeof(dir->data)) ? dir->data : "(unnamed)"'; New = 'dir ? dir->data : "(unnamed)"' },
        @{ Name = 'a successful mount is logged as a failure'; Old = '    if (r) {
        char name[33];'; New = '    if (1) {
        char name[33];' },
        @{ Name = 'the diagnostic changes the returned error'; Old = '    return r;
}
static ABI int32_t save_mount(const Mount1'; New = '    return r ? ERR_INTERNAL : r;
}
static ABI int32_t save_mount(const Mount1' },
        @{ Name = 'sceSaveDataMount2 skips the diagnostic'; Old = 'return mount_logged(m->user,NULL,m->dir,'; New = 'return mount(m->user,NULL,m->dir,' },
        @{ Name = 'sceSaveDataMount skips the diagnostic'; Old = 'return mount_logged(m->user,m->title ? m->title->data : NULL,'; New = 'return mount(m->user,m->title ? m->title->data : NULL,' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        $copy = Copy-ToWorkspace $work $saveSource 'runtime_savedata.c'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'save_mount_tests.exe'
        $build = { & $clang @flags '-w' $test $copy $compat '-lbcrypt' '-pthread' '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "save mount mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
