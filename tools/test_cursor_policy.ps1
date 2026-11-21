# Headless checks of the input-aware game cursor policy (gpu\shim\dcp_cursor.h): mouse/keyboard/controller transitions,
# stick and trigger deadzones with hysteresis, overlay visibility override and post-close restoration, focus changes,
# windowed/borderless parity, and SDL call deduplication, plus mutated copies of the header that must each fail.
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

    $source = ([IO.File]::ReadAllText($header)).Replace("`r`n", "`n")
    $mutations = @(
        @{ Old = "        if (mouse_controls_) {`n            device_ = Device::Mouse;`n        }"; New = "        (void)mouse_controls_;"; What = 'mouse activity fails to show cursor' },
        @{ Old = 'if (event_window != game_window || (dx == 0.0f && dy == 0.0f)) {'; New = 'if (event_window != game_window) {'; What = 'synthetic zero-delta mouse motion is accepted' },
        @{ Old = 'if (event_window != game_window || (dx == 0.0f && dy == 0.0f)) {'; New = 'if (dx == 0.0f && dy == 0.0f) {'; What = 'motion of another window is accepted' },
        @{ Old = 'void KeyboardActivity() { device_ = Device::Keyboard; }'; New = 'void KeyboardActivity() {}'; What = 'keyboard input fails to hide cursor' },
        @{ Old = 'void ControllerActivity() { device_ = Device::Controller; }'; New = 'void ControllerActivity() {}'; What = 'controller input fails to hide cursor' },
        @{ Old = '!gameplay || !focused || overlay ||'; New = '!gameplay || !focused ||'; What = 'overlay no longer forces cursor visible' },
        @{ Old = "        if (!gameplay) {`n            device_ = Device::Mouse;`n        }"; New = "        if (!gameplay || overlay) {`n            device_ = Device::Mouse;`n        }"; What = 'overlay close forgets controller/keyboard input state' },
        @{ Old = 'inline constexpr int kStickDeadzone = 7849;'; New = 'inline constexpr int kStickDeadzone = 1;'; What = 'stick deadzone is removed' },
        @{ Old = 'inline constexpr int kStickReleaseDeadzone = 5500;'; New = 'inline constexpr int kStickReleaseDeadzone = 7849;'; What = 'stick hysteresis is removed' },
        @{ Old = 'inline constexpr int kTriggerThreshold = 30;'; New = 'inline constexpr int kTriggerThreshold = 1;'; What = 'trigger deadzone is removed' },
        @{ Old = 'inline constexpr int kTriggerReleaseThreshold = 15;'; New = 'inline constexpr int kTriggerReleaseThreshold = 30;'; What = 'trigger hysteresis is removed' },
        @{ Old = 'if (visible == visible_) {'; New = 'if (false) {'; What = 'cursor API is called repeatedly without state transition' },
        @{ Old = '!gameplay || !focused || overlay ||'; New = '!gameplay || overlay ||'; What = 'focus loss fails to show cursor' },
        @{ Old = '!gameplay || !focused || overlay ||'; New = '!focused || overlay ||'; What = 'non-gameplay (launcher) policy regressed' }
    )
    $index = 0
    foreach ($mutation in $mutations) {
        $index++
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $at = $source.IndexOf($mutation.Old, [StringComparison]::Ordinal)
        if ($at -lt 0) { Write-Host "  [fail] mutation did not apply: $($mutation.What)"; $failed = $true; continue }
        $changed = $source.Substring(0, $at) + $mutation.New + $source.Substring($at + $mutation.Old.Length)
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
