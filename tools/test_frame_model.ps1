# Headless check, on the real prepared game files, of what the Frame rate setting rests on: the flip rate the executable
# derives from the sync interval and the fixed physics step. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $python) { Write-Host '  [skip] no Python: the frame model was not read'; exit 0 }
& $python.Source -I (Join-Path $root 'scripts/frame_model_check.py')
exit $LASTEXITCODE
