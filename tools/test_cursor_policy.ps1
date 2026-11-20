# Headless checks of the game cursor policy (gpu\shim\dcp_cursor.h): the five second idle hide in fullscreen, focus and
# fullscreen transitions, mouse events of another window and the single SDL call per change, with an injected clock, then
# the same tests against mutated copies of the header, which must each fail. No window, SDL, launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_cursor_policy_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$compiler = Join-Path $tc.Bin 'clang++.exe'
$test = Join-Path $root 'tests\cursor_policy_tests.cpp'
$header = Join-Path $root 'gpu\shim\dcp_cursor.h'

function Build-Test([string]$Name, [string[]]$Include, [string]$Strict = '-Werror') {
    $exe = Join-Path $temp "$Name.exe"
    $arguments = @('--target=x86_64-w64-mingw32', '-std=c++23', '-Wall', '-Wextra', $Strict, '-O1') + $Include + @($test, '-o', $exe)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $compiler @arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { $output | ForEach-Object { Write-Host $_ }; throw "could not compile the cursor policy test ($Name)" }
    return $exe
}

function Invoke-Test([string]$Exe) {
    $lines = & $Exe 2>&1
    [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($lines | ForEach-Object { "$_" }) }
}

$failed = $false
try {
    $real = Invoke-Test (Build-Test 'real' @('-I', (Join-Path $root 'gpu\shim')))
    $real.Lines | ForEach-Object { Write-Host $_ }
    if ($real.Code -ne 0) { Write-Host '  [fail] the cursor policy tests fail on the real header'; $failed = $true }

    $source = [IO.File]::ReadAllText($header)
    $mutations = @(
        @{ Old = '!fullscreen || '; New = ''; What = 'the idle timer runs while windowed (cursor hides in Windowed)' },
        @{ Old = '!gameplay || !focused || overlay ||'; New = '!gameplay || overlay ||'; Old2 = '!fullscreen || !focused || overlay'; New2 = '!fullscreen || overlay'; What = 'focus loss does not show the cursor' },
        @{ Old = "        last_motion_ms_ = now_ms;`n        MouseActivity();"; New = '        MouseActivity();'; What = 'mouse movement does not restart the timer' },
        @{ Old = 'if (event_window != game_window) {'; New = 'if (false) {'; What = 'motion of another window is accepted' },
        @{ Old = '!fullscreen || !focused || overlay'; New = '!fullscreen || overlay'; What = 'focus return does not start a fresh delay' },
        @{ Old = '!fullscreen || !focused || overlay'; New = '!fullscreen || !focused'; What = 'an open overlay does not restart the delay' },
        @{ Old = '!idle_started_ || '; New = ''; What = 'the first update does not start the delay' },
        @{ Old = 'now_ms > last_motion_ms_ && '; New = ''; What = 'a clock that steps back hides the cursor' },
        @{ Old = 'if (visible == visible_) {'; New = 'if (false) {'; What = 'SDL is called on every update' },
        @{ Old = 'kIdleHideMs = 5000;'; New = 'kIdleHideMs = 4000;'; What = 'the delay is 4 s' },
        @{ Old = 'kIdleHideMs = 5000;'; New = 'kIdleHideMs = 6000;'; What = 'the delay is 6 s' }
    )
    $index = 0
    foreach ($mutation in $mutations) {
        $index++
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $changed = $source.Replace($mutation.Old, $mutation.New)
        if ($mutation.ContainsKey('Old2')) { $changed = $changed.Replace($mutation.Old2, $mutation.New2) }
        if ($changed -eq $source) { Write-Host "  [fail] mutation did not apply: $($mutation.What)"; $failed = $true; continue }
        [IO.File]::WriteAllText((Join-Path $folder 'dcp_cursor.h'), $changed)
        $run = Invoke-Test (Build-Test "mutation$index" @('-I', $folder, '-I', (Join-Path $root 'gpu\shim')) '-Wno-error')
        if ($run.Code -ne 0 -and @($run.Lines | Where-Object { $_ -match '\[fail\]' }).Count -gt 0) { Write-Host "  [ok  ] the tests fail when $($mutation.What)" }
        else { Write-Host "  [fail] the tests do not notice: $($mutation.What)"; $failed = $true }
    }
    Write-Host "Cursor policy mutations: $($mutations.Count) copies of the header, each rejected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 }
exit 0
