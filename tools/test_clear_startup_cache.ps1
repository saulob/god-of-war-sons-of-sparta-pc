# Headless test of tools\clear-startup-cache.ps1 in a temporary repository layout: missing, present and nested caches,
# Status mode, deletion limited to the red-zone folder, and refusals for links and files. The real user\ folder is never
# touched. No game, launcher or GUI is started.
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script = Join-Path $PSScriptRoot 'clear-startup-cache.ps1'

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_clear_cache_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  [ok]   $what" } else { Write-Host "  [FAIL] $what"; $script:failures++ }
}

function New-Repo([string]$name) {
    $repo = Join-Path $temp $name
    $user = Join-Path $repo 'user'
    New-Item -ItemType Directory -Force -Path (Join-Path $user 'cache\CUSA57767\redzone\nested\deeper') | Out-Null
    foreach ($file in @(
        'cache\CUSA57767\redzone\0123456789abcdef0123456789abcdef.bin',
        'cache\CUSA57767\redzone\nested\deeper\inner.bin',
        'cache\CUSA57767\shader\shader.bin',
        'cache\CUSA57767\pipeline\pipeline.bin',
        'cache\CUSA57767\identity.bin',
        'cache\other\other.bin',
        'saves\profile\save.dat',
        'sons_of_sparta.ini',
        'trophies\trophy.dat',
        'logs\session.log')) {
        $path = Join-Path $user $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        Set-Content -LiteralPath $path -Value ('x' * 64) -Encoding ascii -NoNewline
    }
    $game = Join-Path $repo 'game\prepared\CUSA57767-1.00\eboot.bin'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $game) | Out-Null
    Set-Content -LiteralPath $game -Value 'eboot' -Encoding ascii
    return $repo
}

function Invoke-Clear([string]$repo, [switch]$Status) {
    $arguments = @{ RepoRoot = $repo }
    if ($Status) { $arguments.Status = $true }
    $output = (& $script @arguments *>&1 | Out-String)
    return [pscustomobject]@{ Output = $output; Code = $LASTEXITCODE }
}

$redzone = { param($repo) Join-Path $repo 'user\cache\CUSA57767\redzone' }

try {
    Write-Host 'contract'
    $loader = Get-Content -Raw -LiteralPath (Join-Path $root 'src\loader.c')
    $paths = Get-Content -Raw -LiteralPath (Join-Path $root 'launcher\paths.cpp')
    Check ($loader.Contains('"%s/cache/CUSA57767/redzone", user_dir')) 'the loader builds user\cache\CUSA57767\redzone'
    Check ($paths.Contains('exe_dir.parent_path().parent_path().parent_path()') -and $paths.Contains('root / "user"')) `
        'the launcher gives the repository build tree <repo>\user'

    Write-Host 'missing cache'
    $repo = New-Repo 'missing'
    Remove-Item -LiteralPath (& $redzone $repo) -Recurse -Force
    $r = Invoke-Clear $repo
    Check ($r.Code -eq 0) 'exit code 0 when no cache exists'
    Check ($r.Output.Contains((& $redzone $repo))) 'prints the resolved cache path'
    Check ($r.Output.Contains('No startup red-zone cache exists yet') -and $r.Output.Contains('This is not an error')) `
        'reports that no cache exists yet, and that this is not an error'
    $s = Invoke-Clear $repo -Status
    Check ($s.Code -eq 0 -and $s.Output.Contains('Exists: no')) 'Status reports the missing cache with exit code 0'

    Write-Host 'present cache, Status'
    $repo = New-Repo 'status'
    $before = @(Get-ChildItem -LiteralPath (Join-Path $repo 'user') -Recurse -Force | ForEach-Object FullName)
    $s = Invoke-Clear $repo -Status
    Check ($s.Code -eq 0 -and $s.Output.Contains('Exists: yes')) 'Status reports the cache as present'
    Check ($s.Output.Contains('Files: 2')) 'Status counts the nested files (2)'
    Check ($s.Output.Contains('Size: 128 bytes')) 'Status reports the total size (2 x 64 bytes)'
    $after = @(Get-ChildItem -LiteralPath (Join-Path $repo 'user') -Recurse -Force | ForEach-Object FullName)
    Check (($before -join '|') -eq ($after -join '|')) 'Status deletes nothing'

    Write-Host 'deletion'
    $repo = New-Repo 'delete'
    $r = Invoke-Clear $repo
    Check ($r.Code -eq 0) 'exit code 0 after deletion'
    Check ($r.Output.Contains('Deleted startup red-zone cache') -and $r.Output.Contains('Next game run will be a cold startup-cache run')) `
        'reports the deletion and the cold run that follows'
    Check (-not (Test-Path -LiteralPath (& $redzone $repo))) 'the red-zone folder is gone, nested files included'
    $survivors = @(
        'user\cache\CUSA57767\shader\shader.bin', 'user\cache\CUSA57767\pipeline\pipeline.bin',
        'user\cache\CUSA57767\identity.bin', 'user\cache\other\other.bin', 'user\cache\CUSA57767',
        'user\saves\profile\save.dat', 'user\sons_of_sparta.ini', 'user\trophies\trophy.dat', 'user\logs\session.log',
        'game\prepared\CUSA57767-1.00\eboot.bin')
    foreach ($path in $survivors) {
        Check (Test-Path -LiteralPath (Join-Path $repo $path)) "survives: $path"
    }
    $again = Invoke-Clear $repo
    Check ($again.Code -eq 0 -and $again.Output.Contains('No startup red-zone cache exists yet')) 'a second run is not an error'

    Write-Host 'refusals'
    $repo = New-Repo 'junction'
    $victim = Join-Path $temp 'victim'
    New-Item -ItemType Directory -Force -Path $victim | Out-Null
    Set-Content -LiteralPath (Join-Path $victim 'keep.bin') -Value 'keep' -Encoding ascii
    Remove-Item -LiteralPath (& $redzone $repo) -Recurse -Force
    cmd /c "mklink /J `"$(& $redzone $repo)`" `"$victim`"" | Out-Null
    $r = Invoke-Clear $repo
    Check ($r.Code -eq 1 -and $r.Output.Contains('Refused')) 'a junction at the cache path is refused'
    Check (Test-Path -LiteralPath (Join-Path $victim 'keep.bin')) 'the junction target survives'

    $repo = New-Repo 'file'
    Remove-Item -LiteralPath (& $redzone $repo) -Recurse -Force
    Set-Content -LiteralPath (& $redzone $repo) -Value 'file' -Encoding ascii
    $r = Invoke-Clear $repo
    Check ($r.Code -eq 1 -and (Test-Path -LiteralPath (& $redzone $repo) -PathType Leaf)) 'a file at the cache path is refused and kept'

    $repo = New-Repo 'outside'
    $other = Join-Path $repo 'user\cache\CUSA57767\redzone-old'
    New-Item -ItemType Directory -Force -Path $other | Out-Null
    $r = Invoke-Clear $repo
    Check ($r.Code -eq 0 -and (Test-Path -LiteralPath $other)) 'a sibling named like the cache is never touched'

    Write-Host 'invalid root'
    $r = Invoke-Clear (Join-Path $temp 'does-not-exist')
    Check ($r.Code -eq 0 -and $r.Output.Contains('No startup red-zone cache exists yet')) 'a root without a user folder is only reported'
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures -eq 0) { Write-Host 'clear-startup-cache: all checks passed' } else { Write-Host "clear-startup-cache: $failures failed" }
exit ([int]($failures -ne 0))
