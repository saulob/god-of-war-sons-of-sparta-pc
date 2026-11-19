# Shows or deletes the startup red-zone cache of the repository's game build (user\cache\CUSA57767\redzone). Nothing else
# under user\ or cache\ is touched: shader and pipeline caches, saves, settings, logs, trophies and game files stay.
[CmdletBinding()]
param(
    [switch]$Status,
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The path contract: launcher/paths.cpp (DefaultsFor, the repository's build tree uses <repo>\user) and the loader's
# "%s/cache/CUSA57767/redzone" in src/loader.c. tools/test_clear_startup_cache.ps1 checks both sources still agree.
function Get-StartupCacheDirectory([string]$Root) {
    return [IO.Path]::GetFullPath((Join-Path $Root 'user\cache\CUSA57767\redzone'))
}

function Test-ReparseFree([string]$Path, [string]$Root) {
    $stop = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    for ($current = $Path; $current -and $current.Length -gt $stop.Length; $current = Split-Path -Parent $current) {
        if (-not (Test-Path -LiteralPath $current)) { continue }
        if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { return $false }
    }
    return $true
}

# Returns the reason a path is not the startup cache, or $null when it is.
function Get-RefusalReason([string]$Path, [string]$Root) {
    if ($Path -ne (Get-StartupCacheDirectory $Root)) { return 'the path is not the startup red-zone cache of this repository' }
    $parts = $Path.Split('\')
    if ($parts[-1] -ne 'redzone' -or $parts[-2] -ne 'CUSA57767' -or $parts[-3] -ne 'cache' -or $parts[-4] -ne 'user') {
        return 'the folder names do not match user\cache\CUSA57767\redzone'
    }
    if (-not (Test-ReparseFree $Path $Root)) { return 'the path or one of its parents is a link or junction' }
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return 'the cache path is a file, not a folder' }
    return $null
}

function Get-CacheContents([string]$Path) {
    $items = @(Get-ChildItem -LiteralPath $Path -Recurse -Force)
    $files = @($items | Where-Object { -not $_.PSIsContainer })
    $bytes = 0L
    foreach ($file in $files) { $bytes += $file.Length }
    return [pscustomobject]@{ Items = $items; Files = $files.Count; Bytes = $bytes }
}

$Path = Get-StartupCacheDirectory $RepoRoot
Write-Host 'Startup red-zone cache:'
Write-Host $Path

$exists = Test-Path -LiteralPath $Path -PathType Container

if ($Status) {
    if (-not $exists) {
        Write-Host 'Exists: no'
        exit 0
    }
    $contents = Get-CacheContents $Path
    Write-Host 'Exists: yes'
    Write-Host ('Files: {0}' -f $contents.Files)
    $kib = ($contents.Bytes / 1KB).ToString('0.0', [Globalization.CultureInfo]::InvariantCulture)
    Write-Host ('Size: {0} bytes ({1} KiB)' -f $contents.Bytes, $kib)
    exit 0
}

if (-not $exists) {
    if (Test-Path -LiteralPath $Path) {
        Write-Host ('Refused: ' + (Get-RefusalReason $Path $RepoRoot))
        exit 1
    }
    Write-Host ''
    Write-Host 'No startup red-zone cache exists yet'
    Write-Host 'Run the new build once and the cache will be created automatically'
    Write-Host 'This is not an error'
    exit 0
}

$reason = Get-RefusalReason $Path $RepoRoot
if ($reason) {
    Write-Host ('Refused: ' + $reason)
    exit 1
}

$contents = Get-CacheContents $Path
if (@($contents.Items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count -gt 0) {
    Write-Host 'Refused: the cache contains a link or junction'
    exit 1
}

Remove-Item -LiteralPath $Path -Recurse -Force
Write-Host ''
Write-Host 'Deleted startup red-zone cache'
Write-Host 'Next game run will be a cold startup-cache run'
exit 0
