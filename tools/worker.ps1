# Validates or deploys the standalone bug report Worker (server\bug-report) with Wrangler. Wrangler's own configuration
# decides where it goes. Saulo runs this deliberately; nothing here logs in, installs or publishes by itself.
#   .\tools\worker.ps1 -DryRun    npx wrangler deploy --dry-run (bundles, uploads nothing)
#   .\tools\worker.ps1            npx wrangler deploy
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("worker: $message")
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workerDir = Join-Path $repoRoot 'server\bug-report'

if (-not (Test-Path -LiteralPath $workerDir -PathType Container)) { Fail "missing folder $workerDir" }
foreach ($name in 'wrangler.toml', 'package.json') {
    if (-not (Test-Path -LiteralPath (Join-Path $workerDir $name) -PathType Leaf)) {
        Fail "missing $workerDir\$name"
    }
}
$configText = Get-Content -Raw -LiteralPath (Join-Path $workerDir 'wrangler.toml')
if ($configText -notmatch '(?m)^\s*main\s*=\s*"([^"]+)"') { Fail 'wrangler.toml has no main entry point' }
$entry = Join-Path $workerDir $Matches[1]
if (-not (Test-Path -LiteralPath $entry -PathType Leaf)) { Fail "missing $entry, the entry point named by wrangler.toml" }
if ($configText -notmatch '(?m)^\s*name\s*=\s*"[^"]+"') { Fail 'wrangler.toml has no Worker name' }
if ($configText -notmatch 'new_sqlite_classes') { Fail 'wrangler.toml has no SQLite Durable Object migration' }
if (-not (Get-Command node -ErrorAction SilentlyContinue)) { Fail 'node not found; install Node.js (https://nodejs.org) and open a new terminal' }
if (-not (Get-Command npx -ErrorAction SilentlyContinue)) { Fail 'npx not found; install Node.js (https://nodejs.org) and open a new terminal' }

$wrangler = @('--no-install', 'wrangler', 'deploy')
if ($DryRun) { $wrangler += '--dry-run' }

Push-Location -LiteralPath $workerDir
try {
    $probe = & npx '--no-install' 'wrangler' '--version' 2>&1
    if ($LASTEXITCODE -ne 0) {
        Fail 'Wrangler is not installed; run "npm install -g wrangler" first. This script never installs it.'
    }
    & npx @wrangler
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -ne 0) {
    [Console]::Error.WriteLine("worker: wrangler exited with code $code (install Wrangler first; this script never installs it)")
    exit $code
}
Write-Host ($(if ($DryRun) { 'Worker bundle checked (dry run, nothing uploaded).' } else { 'Worker deployed.' }))
Write-Host 'The dashboard secret is configured separately and never stored in this repository:'
Write-Host '  npx wrangler secret put SOS_DASHBOARD_TOKEN   (run inside server\bug-report)'
