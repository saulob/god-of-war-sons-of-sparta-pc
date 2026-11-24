<#
.SYNOPSIS
  Unified local-only release orchestrator for God of War: Sons of Sparta PC (Windows ZIP + Linux AppImage).
.DESCRIPTION
  Runs:
    1. Complete Windows headless test suite (tools\test.ps1) unless -SkipTests is set
    2. Windows Release build, verification, and ZIP packaging (tools\package.ps1)
    3. Ubuntu 20.04 (glibc 2.31) WSL Linux Release build, headless tests, and AppImage packaging
       (tools/package-release-linux.sh)
    4. SHA-256 verification of all dist\ artifacts and local release notes generation
  Never publishes, tags, pushes, or launches the game or launcher.
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipWindows,
    [switch]$SkipLinux,
    [string]$WslDistro = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot
$shell = (Get-Process -Id $PID).Path

function Get-ProjectVersion {
    $text = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    if ($text -match 'project\s*\(\s*sons_of_sparta_pc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { return $Matches[1] }
    Stop-Tool 'Could not read project VERSION from CMakeLists.txt'
}

function Find-FocalWslDistro {
    $raw = (& wsl.exe -l -q 2>$null | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
    $preferred = @($raw | Where-Object { $_ -match 'Focal' }) + @($raw | Where-Object { $_ -notmatch 'Focal' })
    foreach ($d in $preferred) {
        $probe = (& wsl.exe -d $d -- bash -lc 'ldd --version | head -n1; command -v clang cmake ninja pkg-config patchelf rsync >/dev/null && pkg-config --exists sdl3 libavcodec && echo READY' 2>$null | Out-String)
        if ($probe -match '2\.31' -and $probe -match 'READY') { return $d }
    }
    return $null
}

$version = Get-ProjectVersion
$distDir = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$failures = New-Object System.Collections.Generic.List[string]

if (-not $SkipTests) {
    Write-Step 'Step 1: Windows headless test gate (tools\test.ps1)'
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test.ps1')
    if ($LASTEXITCODE -ne 0) { $failures.Add("Windows tools\test.ps1 failed (exit $LASTEXITCODE)") }
}

if (-not $SkipWindows) {
    Write-Step "Step 2: Windows Release build & ZIP packaging (v$version)"
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1')
    if ($LASTEXITCODE -ne 0) { $failures.Add("Windows tools\package.ps1 failed (exit $LASTEXITCODE)") }
}

if (-not $SkipLinux) {
    Write-Step "Step 3: Linux x86_64 Release build, headless tests & AppImage packaging (v$version)"
    $distro = if ($WslDistro) { $WslDistro } else { Find-FocalWslDistro }
    if (-not $distro) {
        $failures.Add('No Ubuntu 20.04 (glibc 2.31) WSL distro with clang/cmake/ninja/sdl3/ffmpeg found')
    } else {
        Write-Host "  [wsl]  Using distro $distro"
        $wslRoot = (& wsl.exe -d $distro -- wslpath -a ($root.Replace('\', '/'))).Trim()
        & wsl.exe -d $distro -- bash -lc "bash '$wslRoot/tools/package-release-linux.sh'"
        if ($LASTEXITCODE -ne 0) { $failures.Add("Linux package-release-linux.sh failed on $distro (exit $LASTEXITCODE)") }
    }
}

$winAsset = "SonsOfSpartaPC-v$version-windows-x64.zip"
$linAsset = "SonsOfSpartaPC-v$version-x86_64.AppImage"
$winSha = 'MISSING'
$linSha = 'MISSING'

foreach ($pair in @(@($winAsset, 'win'), @($linAsset, 'lin'))) {
    $file = Join-Path $distDir $pair[0]
    $shaFile = "$file.sha256"
    if ((Test-Path -LiteralPath $file) -and (Test-Path -LiteralPath $shaFile)) {
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        $recorded = ((Get-Content -LiteralPath $shaFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
        if ($actual -ne $recorded) { $failures.Add("SHA-256 mismatch for $($pair[0])") }
        elseif ($pair[1] -eq 'win') { $winSha = $actual } else { $linSha = $actual }
    } else {
        if (($pair[1] -eq 'win' -and -not $SkipWindows) -or ($pair[1] -eq 'lin' -and -not $SkipLinux)) {
            $failures.Add("Missing artifact or checksum: $($pair[0])")
        }
    }
}

$notesTemplate = Join-Path $root 'packaging\release-notes.md'
if (Test-Path -LiteralPath $notesTemplate) {
    $notes = (Get-Content -LiteralPath $notesTemplate -Raw -Encoding UTF8) `
        -replace '@VERSION@', $version `
        -replace '@WINDOWS_ASSET@', $winAsset `
        -replace '@WINDOWS_SHA256@', $winSha `
        -replace '@LINUX_ASSET@', $linAsset `
        -replace '@LINUX_SHA256@', $linSha
    $notesOut = Join-Path $distDir "release-notes-v$version.md"
    [IO.File]::WriteAllText($notesOut, $notes, (New-Object Text.UTF8Encoding($false)))
    Write-Ok "Wrote $notesOut"
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host 'RELEASE NOT PREPARED:' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host "LOCAL RELEASE v$version PREPARED IN $distDir" -ForegroundColor Green
Write-Host "  Windows: $winAsset ($winSha)"
Write-Host "  Linux:   $linAsset ($linSha)"
exit 0
