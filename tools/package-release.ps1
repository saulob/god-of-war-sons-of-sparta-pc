<#
.SYNOPSIS
  Unified local-only release orchestrator for God of War: Sons of Sparta PC (Windows ZIP + Linux AppImage).
.DESCRIPTION
  Runs:
    1. Complete Windows headless test suite (tools\test.ps1) unless -SkipTests is set
    2. Windows Release build, verification, and ZIP packaging (tools\package.ps1); the staged folder dist\<name>\ the ZIP is
       made from stays in dist\, and both it and the ZIP are verified again with package.ps1 -VerifyOnly
    3. Ubuntu 20.04 (glibc 2.31) WSL Linux Release build, headless tests, and AppImage packaging
       (tools/package-release-linux.sh)
    4. SHA-256 verification of all dist\ artifacts and local release notes generation
  Fails closed: when the test gate or a packaging step fails, nothing that looks like a release stays in dist\ (the artifacts of
  the failed run are moved to build\package-failed\), and no package is built from a tree that failed the gate.
  A full run writes every VirusTotal entry as PENDING. -NotesOnly builds and tests nothing and never moves or deletes an
  artifact: it rewrites only the notes and the two VirusTotal manifests for the packages already in dist\, keeping each
  recorded scan URL that points at the exact SHA-256 of its file.
  Never publishes, tags, pushes, or launches the game or launcher.
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipWindows,
    [switch]$SkipLinux,
    [switch]$NotesOnly,
    [string]$WslDistro = ''
)

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_stage_zip.ps1')
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
$winAsset = "SonsOfSpartaPC-v$version-windows-x64.zip"
$linAsset = "SonsOfSpartaPC-v$version-x86_64.AppImage"
$notesOut = Join-Path $distDir "release-notes-v$version.md"
$winName = "SonsOfSpartaPC-v$version-windows-x64"
$stage = Join-Path $distDir $winName
$winSymbols = Join-Path $distDir "$winName-symbols"

# Release artifacts of this version: the packages, the staged Windows folder, their checksums, the notes and the debug symbols that go with them.
$scanList = Join-Path $distDir "virustotal-v$version.json"
$scanText = Join-Path $distDir "virustotal-v$version.txt"
$artifacts = @($notesOut, $scanList, $scanText, $stage, (Join-Path $distDir $winAsset), (Join-Path $distDir "$winAsset.sha256"), (Join-Path $distDir $linAsset),
    (Join-Path $distDir "$linAsset.sha256"), $winSymbols, (Join-Path $distDir "SonsOfSpartaPC-v$version-x86_64-symbols"))

# A failed run takes its artifacts out of dist\, so nothing in it can be mistaken for a release.
function Move-ArtifactsOutOfDist {
    $existing = @($artifacts | Where-Object { Test-Path -LiteralPath $_ })
    if ($existing.Count -eq 0) { return }
    $folder = Join-Path $root ('build\package-failed\v{0}-{1}' -f $version, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    foreach ($path in $existing) { Move-Item -LiteralPath $path -Destination $folder -Force }
    Write-Host "  The artifacts of this failed run are not releases; they were moved out of dist to $folder" -ForegroundColor Yellow
}

if ($NotesOnly) { $SkipTests = $true; $SkipWindows = $true; $SkipLinux = $true }
$recordedScans = @{}
if ($NotesOnly -and (Test-Path -LiteralPath $scanList -PathType Leaf)) {
    try {
        foreach ($entry in @(Get-Content -LiteralPath $scanList -Raw | ConvertFrom-Json | ForEach-Object { $_ })) {
            if ($entry.path) { $recordedScans[[string]$entry.path] = $entry }
        }
    } catch { Stop-Tool "Cannot read ${scanList}: $($_.Exception.Message)" }
}

function Get-ScanCell([string]$RelativePath, [string]$Sha256) {
    $entry = $recordedScans[$RelativePath]
    if (-not $entry -or [string]$entry.sha256 -ne $Sha256) { return 'PENDING' }
    $url = [string]$entry.virustotal
    if ($url -cmatch ('^https://www\.virustotal\.com/gui/file/' + $Sha256 + '(\?nocache=1)?$')) { return $url }
    return 'PENDING'
}

# A failed run must not leave an older build behind under this version's name.
$stale = if ($NotesOnly) { @() } else { @($notesOut, $scanList, $scanText) }
if (-not $SkipWindows) { $stale += (Join-Path $distDir $winAsset), (Join-Path $distDir "$winAsset.sha256") }
if (-not $SkipLinux) { $stale += (Join-Path $distDir $linAsset), (Join-Path $distDir "$linAsset.sha256") }
foreach ($file in $stale) { if (Test-Path -LiteralPath $file -PathType Leaf) { Remove-Item -LiteralPath $file -Force } }
if (-not $SkipWindows) {
    try { foreach ($folder in $stage, $winSymbols) { Remove-ReleaseCandidate $distDir $folder $winName } }
    catch { Stop-Tool $_.Exception.Message }
}

if (-not $SkipTests) {
    Write-Step 'Step 1: Windows headless test gate (tools\test.ps1)'
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test.ps1')
    if ($LASTEXITCODE -ne 0) { $failures.Add("Windows tools\test.ps1 failed (exit $LASTEXITCODE)") }
}

$packaging = $failures.Count -eq 0
if (-not $packaging) { Write-Host '  No package is built: the test gate failed.' -ForegroundColor Yellow }

if (-not $SkipWindows -and $packaging) {
    Write-Step "Step 2: Windows Release build & ZIP packaging (v$version)"
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1')
    if ($LASTEXITCODE -ne 0) { $failures.Add("Windows tools\package.ps1 failed (exit $LASTEXITCODE)") }
    else {
        foreach ($program in 'sons_of_sparta.exe', 'trg-updater.exe') {
            if (-not (Test-Path -LiteralPath (Join-Path $stage $program) -PathType Leaf)) { $failures.Add("The staged Windows folder has no $program ($stage)") }
        }
        foreach ($target in $stage, (Join-Path $distDir $winAsset)) {
            & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1') -VerifyOnly -Target $target
            if ($LASTEXITCODE -ne 0) { $failures.Add("tools\package.ps1 -VerifyOnly failed for $target (exit $LASTEXITCODE)") }
        }
    }
}

if (-not $SkipLinux -and $packaging) {
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

$winSha = 'MISSING'
$linSha = 'MISSING'

foreach ($pair in @(@($winAsset, 'win'), @($linAsset, 'lin'))) {
    if (-not $packaging) { break }
    $file = Join-Path $distDir $pair[0]
    $shaFile = "$file.sha256"
    if ((Test-Path -LiteralPath $file) -and (Test-Path -LiteralPath $shaFile)) {
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        $recorded = ((Get-Content -LiteralPath $shaFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
        if ($actual -ne $recorded) { $failures.Add("SHA-256 mismatch for $($pair[0])") }
        elseif ($pair[1] -eq 'win') { $winSha = $actual } else { $linSha = $actual }
    } else {
        if ($NotesOnly -or ($pair[1] -eq 'win' -and -not $SkipWindows) -or ($pair[1] -eq 'lin' -and -not $SkipLinux)) {
            $failures.Add("Missing artifact or checksum: $($pair[0])")
        }
    }
}

if ($NotesOnly -and $failures.Count -eq 0) {
    $differences = @(Compare-ReleaseStageWithZip $stage (Join-Path $distDir $winAsset) $winName)
    if ($differences.Count -gt 0) { $failures.Add("The staged Windows folder no longer matches $winAsset ($($differences[0]))") }
}

$notesTemplate = Join-Path $root 'packaging\release-notes.md'
if ($failures.Count -eq 0 -and $packaging -and (Test-Path -LiteralPath $notesTemplate)) {
    $programSha = @{}
    foreach ($program in 'sons_of_sparta.exe', 'trg-updater.exe') {
        $programPath = Join-Path $stage $program
        $programSha[$program] = if (Test-Path -LiteralPath $programPath -PathType Leaf) { (Get-FileHash -LiteralPath $programPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { 'MISSING' }
    }
    $scanFiles = @(
        @{ Key = 'WINDOWS'; Path = (Join-Path $distDir $winAsset); Sha256 = $winSha },
        @{ Key = 'WINDOWS_EXE'; Path = (Join-Path $stage 'sons_of_sparta.exe'); Sha256 = $programSha['sons_of_sparta.exe'] },
        @{ Key = 'UPDATER_EXE'; Path = (Join-Path $stage 'trg-updater.exe'); Sha256 = $programSha['trg-updater.exe'] },
        @{ Key = 'LINUX'; Path = (Join-Path $distDir $linAsset); Sha256 = $linSha })
    foreach ($file in $scanFiles) { $file.Scan = Get-ScanCell $file.Path.Substring($root.Length + 1) $file.Sha256 }
    $caveat = 'VirusTotal is an additional verification signal and not an absolute guarantee of safety'
    $status = if (@($scanFiles | Where-Object { $_.Scan -eq 'PENDING' }).Count -eq 0) {
        "The exact final v$version release artifacts and executables were checked with VirusTotal before publication`n`n$caveat"
    } else {
        "VirusTotal: PENDING`n`nNot every file below has been checked with VirusTotal yet. Do not publish these notes until each row links its scan`n`n$caveat"
    }
    $notes = (Get-Content -LiteralPath $notesTemplate -Raw -Encoding UTF8) `
        -replace '@VERSION@', $version `
        -replace '@WINDOWS_ASSET@', $winAsset `
        -replace '@WINDOWS_SHA256@', $winSha `
        -replace '@WINDOWS_EXE_SHA256@', $programSha['sons_of_sparta.exe'] `
        -replace '@UPDATER_EXE_SHA256@', $programSha['trg-updater.exe'] `
        -replace '@LINUX_ASSET@', $linAsset `
        -replace '@LINUX_SHA256@', $linSha `
        -replace '@VIRUSTOTAL_STATUS@', $status
    foreach ($file in $scanFiles) {
        $cell = if ($file.Scan -eq 'PENDING') { 'PENDING' } else { "[VirusTotal]($($file.Scan))" }
        $notes = $notes.Replace("@$($file.Key)_VIRUSTOTAL@", $cell)
    }
    if ($notes -match '@[A-Z_0-9]+@') { $failures.Add("Release notes: unreplaced placeholder $($Matches[0])") }
    elseif ((-not $SkipWindows -or $NotesOnly) -and $programSha.Values -contains 'MISSING') { $failures.Add("Release notes: a staged Windows program is missing from $stage") }
    else {
        [IO.File]::WriteAllText($notesOut, $notes, (New-Object Text.UTF8Encoding($false)))
        Write-Ok "Wrote $notesOut"
        $entries = @($scanFiles | ForEach-Object { [ordered]@{ path = $_.Path.Substring($root.Length + 1); sha256 = $_.Sha256; virustotal = $_.Scan } })
        $utf8 = New-Object Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($scanList, ((ConvertTo-Json -InputObject $entries -Depth 3) + "`n"), $utf8)
        [IO.File]::WriteAllText($scanText, ((@($entries | ForEach-Object { "$($_.virustotal)  $($_.sha256)  $($_.path)" }) -join "`n") + "`n"), $utf8)
        Write-Ok "Wrote $scanList and $scanText"
    }
}

Write-Host ''
if ($failures.Count -gt 0) {
    if ($NotesOnly) { Write-Host '  Nothing in dist was changed.' -ForegroundColor Yellow } else { Move-ArtifactsOutOfDist }
    Write-Host 'RELEASE NOT PREPARED:' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host "LOCAL RELEASE v$version PREPARED IN $distDir" -ForegroundColor Green
Write-Host "  Windows: $winAsset ($winSha)"
Write-Host "  Linux:   $linAsset ($linSha)"
exit 0
