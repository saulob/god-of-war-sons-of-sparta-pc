# Headless proof of the guest entry boundary (src\guest_entry.c): a guest entry called from the host sees a zero
# frame pointer, its prologue saves zero, both SysV arguments arrive, a supplied guest stack is used, the entry's
# result is returned and the host RSP/RBP are restored. The loader and guest threads must use it, and a mutation
# that removes the zero frame pointer must fail. No game, launcher, GPU or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_guest_entry_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$test = Join-Path $root 'tests\guest_entry_tests.c'
$entry = Join-Path $root 'src\guest_entry.c'

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang.exe'
    $flags = @('--target=x86_64-w64-mingw32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fno-omit-frame-pointer',
        '-I', (Join-Path $root 'src'))

    $exe = Join-Path $temp 'guest_entry_tests.exe'
    & $clang @flags $test $entry '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the guest entry test' }
    $run = Invoke-TimedProgram $exe 120
    ($run.Text -split "`r?`n") | Where-Object { $_ -match '^\s+\[(ok|fail)\]|tests:|timed out' } | ForEach-Object { Write-Host $_ }
    $result = $run.Code

    $loader = Get-Content -LiteralPath (Join-Path $root 'src\loader.c') -Raw
    $thread = Get-Content -LiteralPath (Join-Path $root 'src\runtime_thread.c') -Raw
    $source = Get-Content -LiteralPath $entry -Raw
    $loaderCalls = ([regex]::Matches($loader, 'sos_guest_enter\(')).Count
    if ($loaderCalls -eq 1 -and $loader.Contains('sos_guest_enter((void *)(image + entry), &params, (void *)guest_exit, NULL);') -and
        $loader.Contains('runtime_thread_enter_on_stack((void *)(image + entry), &params, (void *)guest_exit, stack, MAIN_STACK);') -and
        $thread.Contains('void *result=sos_guest_enter(entry,arg0,arg1,(unsigned char *)stack+size-64);') -and
        -not $loader.Contains('((Entry)(image + entry))(&params, guest_exit);')) {
        Write-Host '  [ok]   the loader enters the guest through the zero-frame trampoline on both targets'
    } else {
        Write-Host '  [fail] the loader still calls the guest entry directly or does not use the trampoline on both targets'
        $result = 1
    }
    if ($thread.Contains('sos_guest_enter((void *)t->entry,t->argument,NULL,NULL)') -and
        -not $thread.Contains('t->entry(t->argument)')) {
        Write-Host '  [ok]   guest threads enter through the same zero-frame trampoline'
    } else {
        Write-Host '  [fail] guest threads still call their entry directly'
        $result = 1
    }
    if ($source.Contains('xorl %ebp, %ebp') -and $source.Contains('sos_guest_enter')) {
        Write-Host '  [ok]   the trampoline clears the guest frame pointer'
    } else {
        Write-Host '  [fail] the trampoline does not clear the guest frame pointer'
        $result = 1
    }

    $line = '"xorl %ebp, %ebp\n"'
    if (-not $source.Contains($line)) { throw 'the zero-frame line is not in the expected form' }
    $mutatedSource = $source -replace ('(?m)^\s*' + [regex]::Escape($line) + '.*\r?\n'), ''
    $mutatedEntry = Join-Path $temp 'guest_entry_mutated.c'
    Set-Content -LiteralPath $mutatedEntry -Value $mutatedSource -NoNewline
    $mutatedExe = Join-Path $temp 'guest_entry_tests_mutated.exe'
    & $clang @flags '-w' $test $mutatedEntry '-o' $mutatedExe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the mutated guest entry trampoline' }
    $mutated = Invoke-TimedProgram $mutatedExe 120
    if ($mutated.Code -ne 0 -and $mutated.Text -match '\[fail\]' -and $mutated.Text -match 'zero frame pointer') {
        Write-Host '  [ok]   removing the zero frame pointer is detected'
    } else {
        Write-Host "  [fail] removing the zero frame pointer was not detected (exit $($mutated.Code))"
        $result = 1
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
