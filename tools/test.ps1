# One command for the headless checks of the controller and title input path: the pad the game reads (against the baseline
# commit) and the controller kind it publishes, the title's input path and the patch combinations on the real 1.00 and 1.01 code,
# the controller prompts (Automatic, PlayStation, Xbox, the hook), the Settings dialog glyphs, the Xbox left-stick glyphs on the game's resources.assets and
# their cache, the exact path alias, Skip Intro, the frame rate model, and git diff --check. No game, launcher, window or audio
# device is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shell = (Get-Process -Id $PID).Path

$steps = @(
    @{ Name = 'Pad delivery: buttons, Back as Touchpad (not on DualShock 4/DualSense), Map action in the game code, sticks, press/hold/release, 30-240 Hz, baseline commit'; Script = 'test_pad_input.ps1' },
    @{ Name = 'Title input path and patch combinations on the real game code'; Script = 'test_title_patches.ps1' },
    @{ Name = 'Controller prompts (Automatic hook, PlayStation, Xbox patch)'; Script = 'test_controller_prompts.ps1' },
    @{ Name = 'Settings dialog glyphs (Xbox A/B in the confirmation dialogs, restoration, hook on the real code)'; Script = 'test_prompt_dialog.ps1' },
    @{ Name = 'Controller icons (Xbox left-stick glyphs, prepared copies, cache)'; Script = 'test_controller_icons.ps1' },
    @{ Name = 'Guest file paths (exact path alias for the prepared icons)'; Script = 'test_file_paths.ps1' },
    @{ Name = 'Skip Intro (patch flow, title stub, player)'; Script = 'test_skip_intro.ps1' },
    @{ Name = 'Frame rate model'; Script = 'test_frame_model.ps1' }
)

$results = [ordered]@{}
foreach ($step in $steps) {
    Write-Host "==> $($step.Name)" -ForegroundColor Cyan
    & $shell -NoProfile -File (Join-Path $PSScriptRoot $step.Script)
    $results[$step.Name] = $LASTEXITCODE -eq 0
}

Write-Host '==> Game builds' -ForegroundColor Cyan
foreach ($build in @(@{ Key = 'CUSA57767-1.00'; Folders = @('CUSA57767-1.00', 'v1.00') }, @{ Key = 'CUSA57767-1.01'; Folders = @('CUSA57767-1.01', 'v1.01') })) {
    $image = $build.Folders | ForEach-Object { Join-Path $root "build\image\$_\boot.bin" } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($image) { Write-Host "  [ok]   $($build.Key): boot image read by the checks above" }
    else { Write-Host "  [warn] $($build.Key): no boot image, its real game code was not checked" -ForegroundColor Yellow }
}

Write-Host '==> git diff --check' -ForegroundColor Cyan
Push-Location $root
git diff --check
$whitespace = $LASTEXITCODE -eq 0
git diff --cached --check
$whitespace = $whitespace -and $LASTEXITCODE -eq 0
Pop-Location
$results['git diff --check'] = $whitespace

Write-Host ''
$failed = 0
foreach ($name in $results.Keys) {
    if ($results[$name]) { Write-Host "  [ok]   $name" -ForegroundColor Green }
    else { Write-Host "  [FAIL] $name" -ForegroundColor Red; ++$failed }
}
if ($failed) { Write-Host "$failed of $($results.Count) checks failed" -ForegroundColor Red; exit 1 }
Write-Host "all $($results.Count) checks passed" -ForegroundColor Green
exit 0
