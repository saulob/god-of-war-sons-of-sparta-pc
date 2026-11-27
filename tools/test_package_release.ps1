# Headless check of how tools\package-release.ps1 fails: a copy of the real script runs in a temporary project with stand-ins for the test
# gate and the packager, so nothing is built or packaged. A failed gate must build no package, a failed packaging step must not leave
# anything that looks like a release in dist\, and a successful run must still write the packages, their checksums and the notes.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_stage_zip.ps1')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$Ok, [string]$Name) {
    if ($Ok) { Write-Host "  [ok]   $Name" -ForegroundColor Green }
    else { Write-Host "  [fail] $Name" -ForegroundColor Red; $failures.Add($Name) }
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_package_release_' + [guid]::NewGuid().ToString('N'))
$shell = (Get-Process -Id $PID).Path
$version = '9.8.7'
$stem = "SonsOfSpartaPC-v$version-windows-x64"
$zip = "$stem.zip"

function New-Project([string]$Name) {
    $project = Join-Path $temp $Name
    $tools = Join-Path $project 'tools'
    New-Item -ItemType Directory -Force -Path $tools, (Join-Path $project 'packaging') | Out-Null
    foreach ($file in '_common.ps1', '_evidence.ps1', '_stage_zip.ps1', 'package-release.ps1') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $tools }
    Set-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Value "project(sons_of_sparta_pc VERSION $version LANGUAGES C CXX)"
    Set-Content -LiteralPath (Join-Path $project 'packaging\release-notes.md') -Value "v@VERSION@ @WINDOWS_ASSET@ @WINDOWS_SHA256@ @WINDOWS_EXE_SHA256@ @UPDATER_EXE_SHA256@ @LINUX_ASSET@ @LINUX_SHA256@`n@VIRUSTOTAL_STATUS@`n@WINDOWS_VIRUSTOTAL@ @WINDOWS_EXE_VIRUSTOTAL@ @UPDATER_EXE_VIRUSTOTAL@ @LINUX_VIRUSTOTAL@"
    Set-Content -LiteralPath (Join-Path $tools 'test.ps1') -Value 'Add-Content -LiteralPath (Join-Path $PSScriptRoot ''..\ran.txt'') -Value ''tests''; exit [int]$env:SOS_STUB_TESTS'
    Set-Content -LiteralPath (Join-Path $tools 'package.ps1') -Value @'
param([switch]$VerifyOnly, [string]$Target = '')
. (Join-Path $PSScriptRoot '_stage_zip.ps1')
$ran = Join-Path $PSScriptRoot '..\ran.txt'
if ($VerifyOnly) {
    Add-Content -LiteralPath $ran -Value ('verify:' + (Split-Path -Leaf $Target))
    if ($env:SOS_STUB_VERIFY -eq '1') { exit 1 }
    exit 0
}
Add-Content -LiteralPath $ran -Value 'package'
$dist = Join-Path $PSScriptRoot '..\dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$name = 'SonsOfSpartaPC-v9.8.7-windows-x64'
$stage = Join-Path $dist $name
if (Test-Path -LiteralPath $stage) { Add-Content -LiteralPath $ran -Value 'stale-stage-present' }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $dist "$name-symbols") | Out-Null
Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Value 'readme'
if ($env:SOS_STUB_STAGE -ne '0') {
    Set-Content -LiteralPath (Join-Path $stage 'sons_of_sparta.exe') -Value 'game program'
    Set-Content -LiteralPath (Join-Path $stage 'trg-updater.exe') -Value 'updater program'
}
Set-Content -LiteralPath (Join-Path $dist "$name-symbols\sons_of_sparta.debug") -Value 'symbols'
[void](New-ReleaseZip $stage (Join-Path $dist "$name.zip") $name)
if ($env:SOS_STUB_PACKAGE -eq '1') { exit 1 }
$hash = (Get-FileHash -LiteralPath (Join-Path $dist "$name.zip") -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $dist "$name.zip.sha256") -Value "$hash  $name.zip"
exit 0
'@
    return $project
}

function Invoke-Release([string]$Project, [int]$TestsExit, [int]$PackageExit, [string[]]$Arguments, [switch]$WithoutPrograms, [switch]$FailVerify) {
    $env:SOS_STUB_TESTS = "$TestsExit"
    $env:SOS_STUB_PACKAGE = "$PackageExit"
    if ($WithoutPrograms) { $env:SOS_STUB_STAGE = '0' }
    if ($FailVerify) { $env:SOS_STUB_VERIFY = '1' }
    $output = (& $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $Project 'tools\package-release.ps1') @Arguments 2>&1 | Out-String)
    $code = $LASTEXITCODE
    Remove-Item Env:\SOS_STUB_TESTS, Env:\SOS_STUB_PACKAGE, Env:\SOS_STUB_STAGE, Env:\SOS_STUB_VERIFY -ErrorAction SilentlyContinue
    $ran = if (Test-Path -LiteralPath (Join-Path $Project 'ran.txt')) { @(Get-Content -LiteralPath (Join-Path $Project 'ran.txt')) } else { @() }
    return [pscustomobject]@{ Code = $code; Output = $output; Ran = $ran }
}

function Get-DistFiles([string]$Project) {
    $dist = Join-Path $Project 'dist'
    if (-not (Test-Path -LiteralPath $dist)) { return @() }
    return @(Get-ChildItem -LiteralPath $dist -Force | ForEach-Object { $_.Name })
}

try {
    Write-Step 'A failed test gate builds no package and leaves nothing in dist'
    $project = New-Project 'gate-fails'
    New-Item -ItemType Directory -Force -Path (Join-Path $project 'dist') | Out-Null
    foreach ($stale in $zip, "$zip.sha256", "release-notes-v$version.md") { Set-Content -LiteralPath (Join-Path $project "dist\$stale") -Value 'older run' }
    New-Item -ItemType Directory -Force -Path (Join-Path $project "dist\$stem"), (Join-Path $project "dist\$stem-symbols"), (Join-Path $project 'dist\steam-artwork') | Out-Null
    Set-Content -LiteralPath (Join-Path $project "dist\$stem\sons_of_sparta.exe") -Value 'older game program'
    Set-Content -LiteralPath (Join-Path $project 'dist\steam-artwork\capsule.png') -Value 'artwork'
    $run = Invoke-Release $project 1 0 @('-SkipLinux')
    Check ($run.Code -ne 0) 'the exit code is not zero'
    Check ($run.Ran -contains 'tests' -and $run.Ran -notcontains 'package') 'the packager is not started after the failed gate'
    Check ((@(Get-DistFiles $project) -join ',') -eq 'steam-artwork') "dist holds nothing but the unrelated artwork, an older run's stage and files gone ($((Get-DistFiles $project) -join ', '))"
    Check (Test-Path -LiteralPath (Join-Path $project 'dist\steam-artwork\capsule.png') -PathType Leaf) 'the unrelated steam-artwork folder is untouched'
    Check ($run.Output -match 'RELEASE NOT PREPARED' -and $run.Output -match 'No package is built') 'the run says the release was not prepared and why'

    Write-Step 'A packaging step that fails after writing files leaves nothing that looks like a release'
    $project = New-Project 'package-fails'
    $run = Invoke-Release $project 0 1 @('-SkipLinux')
    Check ($run.Code -ne 0) 'the exit code is not zero'
    Check ($run.Ran -contains 'package') 'the packager ran'
    Check (@(Get-DistFiles $project).Count -eq 0) "dist is empty ($((Get-DistFiles $project) -join ', '))"
    $quarantine = @(Get-ChildItem -LiteralPath (Join-Path $project 'build\package-failed') -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    Check ($quarantine -contains $zip) 'the partial package was moved to build\package-failed'
    Check ($quarantine -contains 'sons_of_sparta.exe' -and $quarantine -contains 'sons_of_sparta.debug') 'the partial stage and its symbols were moved too'
    Check ($run.Output -match 'build\\package-failed') 'the run says where the artifacts went'

    Write-Step 'The tests can be skipped and a failed package is still not left in dist'
    $project = New-Project 'skip-tests'
    $run = Invoke-Release $project 1 1 @('-SkipLinux', '-SkipTests')
    Check ($run.Code -ne 0 -and $run.Ran -notcontains 'tests' -and @(Get-DistFiles $project).Count -eq 0) 'with -SkipTests the failed package still leaves dist empty'

    Write-Step 'A successful run still writes the package, its checksum and the notes'
    $project = New-Project 'succeeds'
    New-Item -ItemType Directory -Force -Path (Join-Path $project "dist\$stem"), (Join-Path $project 'dist\steam-artwork') | Out-Null
    Set-Content -LiteralPath (Join-Path $project "dist\$stem\old.dll") -Value 'stale'
    Set-Content -LiteralPath (Join-Path $project 'dist\steam-artwork\capsule.png') -Value 'artwork'
    $run = Invoke-Release $project 0 0 @('-SkipLinux')
    $files = @(Get-DistFiles $project)
    Check ($run.Code -eq 0) 'the exit code is zero'
    Check ($files -contains $zip -and $files -contains "$zip.sha256" -and $files -contains "release-notes-v$version.md") "dist holds the package, its checksum and the notes ($($files -join ', '))"
    Check ($files -contains $stem -and (Test-Path -LiteralPath (Join-Path $project "dist\$stem") -PathType Container)) 'the staged Windows folder stays in dist after a successful run'
    Check ($run.Ran -notcontains 'stale-stage-present' -and -not (Test-Path -LiteralPath (Join-Path $project "dist\$stem\old.dll"))) 'the stage of an older run is removed before packaging'
    Check ($files -contains "$stem-symbols" -and @(Get-ChildItem -LiteralPath (Join-Path $project "dist\$stem") -Recurse -File | Where-Object { $_.Extension -eq '.debug' }).Count -eq 0) 'the symbols stay beside the stage, outside the player folder'
    Check (Test-Path -LiteralPath (Join-Path $project 'dist\steam-artwork\capsule.png') -PathType Leaf) 'the unrelated steam-artwork folder is preserved'
    Check ($run.Ran -contains "verify:$stem" -and $run.Ran -contains "verify:$zip") 'the retained stage and the ZIP are both verified with package.ps1 -VerifyOnly'
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $project "dist\$zip"))
    try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
    Check (@($entries | Where-Object { -not $_.StartsWith("$stem/") -or $_ -match '\.debug$|-symbols' }).Count -eq 0 -and $entries -contains "$stem/sons_of_sparta.exe" -and $entries -contains "$stem/trg-updater.exe") 'the ZIP holds the stage programs under one folder and no symbols'
    Check (@(Compare-ReleaseStageWithZip (Join-Path $project "dist\$stem") (Join-Path $project "dist\$zip") $stem).Count -eq 0) 'the ZIP of the run matches the retained stage'
    $notes = Get-Content -LiteralPath (Join-Path $project "dist\release-notes-v$version.md") -Raw
    $recorded = ((Get-Content -LiteralPath (Join-Path $project "dist\$zip.sha256") -Raw).Trim() -split '\s+')[0]
    Check ($notes -match [regex]::Escape($recorded) -and $notes -match [regex]::Escape($version)) 'the notes carry the version and the checksum of the package'
    Check (-not (Test-Path -LiteralPath (Join-Path $project 'build\package-failed'))) 'nothing was moved out of dist'
    $stageDir = Join-Path $project "dist\$stem"
    $gameSha = (Get-FileHash -LiteralPath (Join-Path $stageDir 'sons_of_sparta.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    $updaterSha = (Get-FileHash -LiteralPath (Join-Path $stageDir 'trg-updater.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    Check ($notes -match [regex]::Escape($gameSha) -and $notes -match [regex]::Escape($updaterSha)) 'the notes carry the checksums of the game program and the updater'

    $scan = @(Get-Content -LiteralPath (Join-Path $project "dist\virustotal-v$version.json") -Raw | ConvertFrom-Json | ForEach-Object { $_ })
    $scanHashes = @($scan | ForEach-Object { $_.sha256 })
    Check ($scan.Count -eq 4 -and @($scan | Where-Object { $_.virustotal -ne 'PENDING' }).Count -eq 0) 'the scan list has the four files, all PENDING'
    Check ($scanHashes -contains $recorded -and $scanHashes -contains $gameSha -and $scanHashes -contains $updaterSha) 'the scan list carries the hashes of this run'
    $scanPaths = @($scan | ForEach-Object { $_.path })
    $expectedPaths = @("dist\$zip", "dist\$stem\sons_of_sparta.exe", "dist\$stem\trg-updater.exe", "dist\SonsOfSpartaPC-v$version-x86_64.AppImage")
    Check ((@($expectedPaths | Where-Object { $scanPaths -notcontains $_ }).Count -eq 0) -and $scanPaths.Count -eq 4) "the scan list points at the dist files ($($scanPaths -join ', '))"
    $scanText = Get-Content -LiteralPath (Join-Path $project "dist\virustotal-v$version.txt") -Raw
    Check (($scanPaths -join "`n") -notmatch 'build\\package' -and $scanText -notmatch 'build\\package' -and $notes -notmatch 'build\\package') 'no manifest or note points at build\package'
    Check (-not (Test-Path -LiteralPath (Join-Path $project 'dist\virustotal'))) 'no dist\virustotal folder is made'

    Write-Step 'The release-notes template'
    $template = Get-Content -LiteralPath (Join-Path $root 'packaging\release-notes.md') -Raw -Encoding UTF8
    function Get-Section([string]$Title) { return [regex]::Match($template, '(?ms)^## ' + [regex]::Escape($Title) + '\r?\n(.*?)(?=^## |\z)').Groups[1].Value }
    Check ($template.StartsWith('# God of War: Sons of Sparta PC v@VERSION@')) 'the title is the port name and version'
    Check ($template -match 'unofficial PC port' -and $template -match 'not affiliated with or endorsed by Sony Interactive Entertainment, Santa Monica Studio') 'it calls the port unofficial and names who it is not affiliated with'
    $released = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(sons_of_sparta_pc VERSION (\d+\.\d+\.\d+)').Groups[1].Value
    Check ($released -ne '' -and $template -notmatch '(?i)first release') "the notes do not present v$released as the first release"
    Check ($template -notmatch '(?i)first PC release|\bofficial\b') 'it does not read as an official release'
    Check ($template -match '\*\*No game files are included\.\*\*' -and $template -match 'CUSA57767` USA \*\*1\.00\*\*' -and $template -match 'CUSA57767` USA \*\*1\.01\*\*') 'the no-game-files notice names both supported builds'
    $sections = @([regex]::Matches($template, '(?m)^## (.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    Check (($sections -join '|') -eq "What's new in v$released|What's fixed in v$released|Bug Reports addressed|GitHub Issues addressed|Highlights|Also included|Validation|VirusTotal|Known limitations|Downloads|SHA-256|Credits") "the changes since the previous release come first, then the Dragon release layout ($($sections -join ', '))"
    $new = Get-Section "What's new in v$released"
    $cheats = @('God Mode', 'Infinite Spartan Spirit', 'Infinite Magic', 'Damage Multiplier', 'Movement Speed', 'Jump Height', 'Add 1000 Blood Orbs', 'Add 10 Upgrade Materials', 'F10')
    Check ((@($cheats | Where-Object { -not $new.Contains($_) }).Count -eq 0) -and $new -match '\*\*Off by default\*\*' -and $new -match 'never saved' -and $new -notmatch '(?i)Pass Through Gates') 'the new cheats are listed, Off by default, with session-only F10 changes'
    $changes = Get-Section "What's fixed in v$released"
    $fixedAt = $changes.IndexOf('### Fixed'); $diagnosticsAt = $changes.IndexOf('### Diagnostics improved')
    Check ($fixedAt -ge 0 -and $diagnosticsAt -gt $fixedAt) 'confirmed fixes and improved diagnostics are separate lists'
    Check ($diagnosticsAt -lt 0 -or $changes.Substring($diagnosticsAt) -match 'UHD 630[^\n]*\*\*that crash is not confirmed fixed yet\*\*') 'the Intel UHD 630 crash is listed as diagnostics, not as a fix'
    Check ($fixedAt -lt 0 -or $diagnosticsAt -lt 0 -or $changes.Substring($fixedAt, $diagnosticsAt - $fixedAt) -notmatch 'UHD 630') 'no confirmed fix names the Intel UHD 630'
    $reports = Get-Section 'Bug Reports addressed'
    $reportIds = @([regex]::Matches($reports, '(?m)^\| \*\*(SOS-[A-Z0-9]{6})\*\* \|') | ForEach-Object { $_.Groups[1].Value })
    Check ($reportIds.Count -gt 0 -and $reports -match 'do not have public URLs unless the same problem also has a GitHub Issue' -and $reports -notmatch 'https://[^)]*SOS-') "Bug Report IDs are listed without public URLs ($($reportIds -join ', '))"
    $issues = Get-Section 'GitHub Issues addressed'
    function Get-IssueRow([int]$Number) { return [regex]::Match($issues, '(?m)^\| \[#' + $Number + '\]\(https://github\.com/saulob/god-of-war-sons-of-sparta-pc/issues/' + $Number + '\).*$').Value }
    Check ((Get-IssueRow 2) -match '\*\*not fixed\*\*' -and (Get-IssueRow 2) -notmatch '\*\*Fixed\*\*') 'Issue #2 (Intel UHD 630) is diagnostics only, not a fix'
    Check ((Get-IssueRow 4) -eq '' -and (Get-Section 'Known limitations') -match 'issues/4\)[^\n]*\*\*not fixed\*\*') 'Issue #4 (ultrawide) is not claimed as addressed and stays listed as not fixed'
    Check ((Get-IssueRow 7) -match '\*\*Partially addressed\*\*' -and (Get-IssueRow 7) -match 'not implemented') 'Issue #7 (cheats) is only partially addressed'
    Check ($template -notmatch '(?i)reporter[- ]confirmed|confirmed by the reporter') 'no reporter confirmation is claimed'
    $headline = Get-Section 'Highlights'
    $positions = @('True Ultrawide Hor+', 'Up to 4K', 'High frame rates', 'The Pit of Agonies from the beginning', 'Local two-player co-op in The Pit', 'Unlock The Pit Early', 'Unlock Hermes Mode', 'Skip Intro', 'Xbox and PlayStation controllers', 'Automatic controller prompts' | ForEach-Object { $headline.IndexOf("- **$_") })
    Check (($positions -notcontains -1) -and ((($positions | Sort-Object) -join ',') -eq ($positions -join ','))) 'the highlights are in the requested order'
    Check ($headline -match '21:9 and 32:9' -and $headline -notmatch '16:10|4:3|5:4' -and $headline -match '30, 60 \(the original\), 90, \*\*120\*\*, 144, 165 or 240 FPS, or Unlimited' -and $headline -match 'Kratos and Deimos' -and $headline -match 'campaign stays single-player') 'True Ultrawide, frame rates and The Pit co-op are described exactly'
    Check ($template -notmatch '(?i)\bonline\b|\blobb(y|ies)\b|\bfriends?\b|\brelay\b') 'it advertises nothing online'
    $scan = Get-Section 'VirusTotal'
    Check ($scan.Contains('@VIRUSTOTAL_STATUS@') -and $template -notmatch '(?i)checked with VirusTotal|PENDING') 'the scan status comes from the release run, never from the template'
    foreach ($row in @(@('@WINDOWS_ASSET@', 'WINDOWS_SHA256', 'WINDOWS'), @('@LINUX_ASSET@', 'LINUX_SHA256', 'LINUX'), @('sons_of_sparta.exe', 'WINDOWS_EXE_SHA256', 'WINDOWS_EXE'), @('trg-updater.exe', 'UPDATER_EXE_SHA256', 'UPDATER_EXE'))) {
        Check ($scan.Contains("| ``$($row[0])`` | ``@$($row[1])@`` | @$($row[2])_VIRUSTOTAL@ |")) "the VirusTotal table pairs $($row[0]) with its hash and scan"
    }
    $sha = Get-Section 'SHA-256'
    Check (@('@WINDOWS_ASSET@', '@LINUX_ASSET@', 'sons_of_sparta.exe', 'trg-updater.exe' | Where-Object { -not $sha.Contains("| ``$_`` |") }).Count -eq 0 -and $sha.Contains('<sha256> <file name>')) 'the SHA-256 table has the four files and explains the .sha256 format'
    $downloads = Get-Section 'Downloads'
    $base = 'https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v@VERSION@/'
    Check ($downloads.Contains("($base@WINDOWS_ASSET@)") -and $downloads.Contains("($base@WINDOWS_ASSET@.sha256)") -and $downloads.Contains("($base@LINUX_ASSET@)") -and $downloads.Contains("($base@LINUX_ASSET@.sha256)") -and $downloads.Contains('**sons_of_sparta.exe**')) 'the downloads point at this repository and version'
    $groups = @([regex]::Matches((Get-Content -LiteralPath (Join-Path $root 'tools\test.ps1') -Raw), "(?m)^    @\{ Name = '")).Count
    Check ((Get-Section 'Known limitations') -match 'Linux and Steam Deck remain experimental' -and (Get-Section 'Known limitations') -match 'runtime coverage is still more limited than on Windows' -and (Get-Section 'Validation') -match "$groups of $groups groups" -and (Get-Section 'Validation') -match 'not yet been confirmed on those systems') "the Linux status and the release gate of $groups groups are stated"
    Check ($template -notmatch '(?i)\b\d+\s+(malicious|suspicious|detections?|engines?)\b|no detections|virus.free|malware.free|100% safe|guaranteed|\bclean\b') 'it gives no detection count and no safety claim'
    Check ($template -notmatch '(?i)dragon|DragonsCrown') 'nothing in it names another port'
    foreach ($name in 'VERSION', 'WINDOWS_ASSET', 'WINDOWS_SHA256', 'WINDOWS_EXE_SHA256', 'UPDATER_EXE_SHA256', 'LINUX_ASSET', 'LINUX_SHA256', 'VIRUSTOTAL_STATUS', 'WINDOWS_VIRUSTOTAL', 'WINDOWS_EXE_VIRUSTOTAL', 'UPDATER_EXE_VIRUSTOTAL', 'LINUX_VIRUSTOTAL') {
        Check ($template.Contains("@$name@")) "it takes @$name@ from the release run"
    }

    Write-Step 'The README'
    $readme = Get-Content -LiteralPath (Join-Path $root 'README.md') -Raw -Encoding UTF8
    foreach ($badge in 'https://img.shields.io/github/v/release/saulob/god-of-war-sons-of-sparta-pc?', 'https://img.shields.io/github/downloads/saulob/god-of-war-sons-of-sparta-pc/total?', 'platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental') {
        Check ($readme.Contains($badge)) "the README has the badge $badge"
    }
    Check ($readme -notmatch '(?i)dragons-crown-pro-pc|DragonsCrownProPC|OpenCrown|Boss Battle|dragons_crown') 'the README links no other port and copies none of its features'
    Check ($readme.Contains('<a href="https://github.com/sponsors/saulob" target="_blank"><img src="https://img.shields.io/badge/GitHub%20Sponsors-Sponsor-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors"></a>')) 'the README has the GitHub Sponsors button'
    Check ($readme -match 'CUSA57767' -and $readme -match '1\.00' -and $readme -match '1\.01' -and $readme -match '\*\*The game itself is not included\*\*') 'the README names the supported builds and says the game is not included'
    Check ($readme -match '30, 60 \(Original, default\), 90, 120, 144, 165, 240 or Unlimited' -and $readme -match '21:9' -and $readme -match '32:9' -and $readme -match 'Kratos and Deimos') 'the README states the frame rates, True Ultrawide and The Pit co-op'
    Check ($readme -notmatch '(?i)\bonline (co-?op|play|multiplayer)\b|\blobb(y|ies)\b') 'the README advertises nothing online'
    foreach ($link in [regex]::Matches($readme, '\]\((?!https?://|#)([^)#]+)') | ForEach-Object { $_.Groups[1].Value }) {
        Check (Test-Path -LiteralPath (Join-Path $root $link)) "the README link $link exists"
    }
    foreach ($image in [regex]::Matches($readme, 'src="(?!https?://)([^"]+)"') | ForEach-Object { $_.Groups[1].Value }) {
        Check (Test-Path -LiteralPath (Join-Path $root $image) -PathType Leaf) "the README image $image exists"
    }

    Write-Step 'Release notes for packages already in dist (-NotesOnly)'
    $project = New-Project 'notes-only'
    $run = Invoke-Release $project 0 0 @('-SkipLinux')
    $dist = Join-Path $project 'dist'
    $appImage = "SonsOfSpartaPC-v$version-x86_64.AppImage"
    Set-Content -LiteralPath (Join-Path $dist $appImage) -Value 'appimage'
    $appSha = (Get-FileHash -LiteralPath (Join-Path $dist $appImage) -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath (Join-Path $dist "$appImage.sha256") -Value "$appSha  $appImage"
    Check ($run.Code -eq 0) 'a full run prepared the fixture'
    $fresh = Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw
    Check ($fresh -match 'VirusTotal: PENDING' -and $fresh -notmatch 'checked with VirusTotal before publication' -and $fresh -notmatch '\[VirusTotal\]\(') 'a full run writes PENDING notes and links no scan'
    $paths = [ordered]@{
        "dist\$zip" = (Join-Path $dist $zip)
        "dist\$stem\sons_of_sparta.exe" = (Join-Path $dist "$stem\sons_of_sparta.exe")
        "dist\$stem\trg-updater.exe" = (Join-Path $dist "$stem\trg-updater.exe")
        "dist\$appImage" = (Join-Path $dist $appImage)
    }
    $before = @{}
    foreach ($key in $paths.Keys) { $item = Get-Item -LiteralPath $paths[$key]; $before[$key] = "$((Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant())|$($item.LastWriteTimeUtc.Ticks)" }
    function Write-Scans([hashtable]$Urls) {
        $entries = @(foreach ($key in $paths.Keys) { [ordered]@{ path = $key; sha256 = $before[$key].Split('|')[0]; virustotal = $(if ($Urls.ContainsKey($key)) { $Urls[$key] } else { 'PENDING' }) } })
        [IO.File]::WriteAllText((Join-Path $dist "virustotal-v$version.json"), (ConvertTo-Json -InputObject $entries -Depth 3))
    }
    function Get-Scans { return @(Get-Content -LiteralPath (Join-Path $dist "virustotal-v$version.json") -Raw | ConvertFrom-Json | ForEach-Object { $_ }) }
    $urls = @{}
    foreach ($key in $paths.Keys) { $urls[$key] = 'https://www.virustotal.com/gui/file/' + $before[$key].Split('|')[0] + $(if ($key -like '*trg-updater.exe') { '' } else { '?nocache=1' }) }

    Write-Scans $urls
    $ranBefore = @(Get-Content -LiteralPath (Join-Path $project 'ran.txt')).Count
    $run = Invoke-Release $project 0 0 @('-NotesOnly')
    $notes = Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw
    Check ($run.Code -eq 0 -and $run.Ran.Count -eq $ranBefore) "with four valid scans -NotesOnly succeeds and runs no test or packager ($($run.Output.Trim().Split("`n")[-1]))"
    Check ($notes -match "The exact final v$version release artifacts and executables were checked with VirusTotal before publication" -and $notes -match 'not an absolute guarantee of safety' -and $notes -notmatch 'PENDING') 'the notes say the exact files were checked, with the caveat, and nothing is PENDING'
    Check (@($urls.Values | Where-Object { -not $notes.Contains("[VirusTotal]($_)") }).Count -eq 0) 'each row links its own scan'
    $text = Get-Content -LiteralPath (Join-Path $dist "virustotal-v$version.txt") -Raw
    Check (@(Get-Scans | Where-Object { $_.virustotal -ne $urls[$_.path] }).Count -eq 0 -and @($urls.Values | Where-Object { -not $text.Contains($_) }).Count -eq 0) 'the JSON and TXT keep the four scan URLs'
    $after = @{}
    foreach ($key in $paths.Keys) { $item = Get-Item -LiteralPath $paths[$key]; $after[$key] = "$((Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant())|$($item.LastWriteTimeUtc.Ticks)" }
    Check (@($paths.Keys | Where-Object { $before[$_] -ne $after[$_] }).Count -eq 0) 'no package or program was touched'

    $partial = @{} + $urls
    $partial["dist\$stem\trg-updater.exe"] = 'https://www.virustotal.com/gui/file/' + ('0' * 64)
    $partial["dist\$appImage"] = 'https://example.com/gui/file/' + $appSha
    Write-Scans $partial
    $run = Invoke-Release $project 0 0 @('-NotesOnly')
    $notes = Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw
    $scans = @(Get-Scans)
    Check ($run.Code -eq 0 -and $notes -match 'VirusTotal: PENDING' -and $notes -notmatch 'checked with VirusTotal before publication') 'a scan of another hash or another site keeps the notes PENDING'
    Check (@($scans | Where-Object { $_.path -like '*trg-updater.exe' -or $_.path -like '*.AppImage' } | Where-Object { $_.virustotal -ne 'PENDING' }).Count -eq 0 -and @($scans | Where-Object { $_.virustotal -eq $urls[$_.path] }).Count -eq 2) 'only the two scans of the exact files are kept'

    Write-Scans $urls
    $run = Invoke-Release $project 0 0 @('-SkipLinux')
    $notes = Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw
    Check ($run.Code -eq 0 -and $notes -match 'VirusTotal: PENDING' -and @(Get-Scans | Where-Object { $_.virustotal -ne 'PENDING' }).Count -eq 0) 'a new full run starts PENDING again, whatever an older manifest said'

    Write-Scans $urls
    $run = Invoke-Release $project 0 0 @('-NotesOnly')
    Set-Content -LiteralPath (Join-Path $dist "$appImage.sha256") -Value (('0' * 64) + "  $appImage")
    $notesBefore = Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw
    $run = Invoke-Release $project 0 0 @('-NotesOnly')
    Check ($run.Code -ne 0 -and $run.Output -match 'Nothing in dist was changed' -and (Test-Path -LiteralPath (Join-Path $dist $appImage)) -and (Test-Path -LiteralPath (Join-Path $dist $stem)) -and (Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw) -eq $notesBefore) 'a checksum mismatch fails -NotesOnly without moving or rewriting anything'
    Set-Content -LiteralPath (Join-Path $dist "$appImage.sha256") -Value "$appSha  $appImage"
    Add-Content -LiteralPath (Join-Path $dist "$stem\trg-updater.exe") -Value 'changed after the ZIP'
    $run = Invoke-Release $project 0 0 @('-NotesOnly')
    Check ($run.Code -ne 0 -and $run.Output -match 'no longer matches' -and (Test-Path -LiteralPath (Join-Path $dist $zip)) -and (Get-Content -LiteralPath (Join-Path $dist "release-notes-v$version.md") -Raw) -eq $notesBefore) 'a stage that no longer matches the ZIP fails -NotesOnly without moving or rewriting anything'

    Write-Step 'A staged program that is missing is a failure and leaves dist empty'
    $project = New-Project 'no-programs'
    $run = Invoke-Release $project 0 0 @('-SkipLinux') -WithoutPrograms
    Check ($run.Code -ne 0 -and @(Get-DistFiles $project).Count -eq 0 -and $run.Output -match 'staged Windows folder has no') 'the release is not prepared and dist is empty'

    Write-Step 'A checksum that does not match the package is a failure'
    $project = New-Project 'bad-checksum'
    Set-Content -LiteralPath (Join-Path $project 'tools\package.ps1') -Value @'
param([switch]$VerifyOnly, [string]$Target = '')
$dist = Join-Path $PSScriptRoot '..\dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$name = 'SonsOfSpartaPC-v9.8.7-windows-x64'
New-Item -ItemType Directory -Force -Path (Join-Path $dist $name) | Out-Null
Set-Content -LiteralPath (Join-Path $dist "$name\sons_of_sparta.exe") -Value 'game program'
Set-Content -LiteralPath (Join-Path $dist "$name\trg-updater.exe") -Value 'updater program'
Set-Content -LiteralPath (Join-Path $dist "$name.zip") -Value 'zip'
Set-Content -LiteralPath (Join-Path $dist "$name.zip.sha256") -Value ('0' * 64 + "  $name.zip")
exit 0
'@
    $run = Invoke-Release $project 0 0 @('-SkipLinux')
    Check ($run.Code -ne 0 -and @(Get-DistFiles $project).Count -eq 0) 'the release is not prepared and dist is empty, the stage included'

    Write-Step 'A stage that fails -VerifyOnly is not a candidate'
    $project = New-Project 'verify-fails'
    $run = Invoke-Release $project 0 0 @('-SkipLinux') -FailVerify
    $quarantine = @(Get-ChildItem -LiteralPath (Join-Path $project 'build\package-failed') -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    Check ($run.Code -ne 0 -and @(Get-DistFiles $project).Count -eq 0 -and $quarantine -contains 'sons_of_sparta.exe') 'the release is not prepared and the stage is out of dist'
    Check (-not (Test-Path -LiteralPath (Join-Path $project 'dist\release-notes-v9.8.7.md'))) 'no notes are written for it'

    Write-Step 'The ZIP is checked against the stage it was made from'
    $fixture = Join-Path $temp 'zip-fixture'
    $fixtureStage = Join-Path $fixture $stem
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtureStage 'assets\launcher'), (Join-Path $fixtureStage 'licenses') | Out-Null
    Set-Content -LiteralPath (Join-Path $fixtureStage 'sons_of_sparta.exe') -Value 'game program'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'trg-updater.exe') -Value 'updater program'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'assets\launcher\header-art.png') -Value 'image'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'licenses\THIRD-PARTY-LICENSES.txt') -Value 'licenses'
    $fixtureZip = Join-Path $fixture "$stem.zip"
    $count = New-ReleaseZip $fixtureStage $fixtureZip $stem
    Check ($count -eq 4) 'the ZIP is made from every file of the stage'
    Check (@(Compare-ReleaseStageWithZip $fixtureStage $fixtureZip $stem).Count -eq 0) 'the ZIP and the stage match: names, count, sizes and contents'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'trg-updater.exe') -Value 'updater PROGRAM'
    Check (@(Compare-ReleaseStageWithZip $fixtureStage $fixtureZip $stem) -join ';' -match 'content differs: trg-updater.exe') 'a changed file of the same size is found'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'trg-updater.exe') -Value 'updater program'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'extra.dll') -Value 'late'
    Check (@(Compare-ReleaseStageWithZip $fixtureStage $fixtureZip $stem) -join ';' -match 'missing from the ZIP: extra.dll') 'a file added to the stage after the ZIP is found'
    Remove-Item -LiteralPath (Join-Path $fixtureStage 'extra.dll')
    Remove-Item -LiteralPath (Join-Path $fixtureStage 'licenses\THIRD-PARTY-LICENSES.txt')
    Check (@(Compare-ReleaseStageWithZip $fixtureStage $fixtureZip $stem) -join ';' -match 'not in the stage: licenses/THIRD-PARTY-LICENSES.txt') 'a file missing from the stage is found'
    Set-Content -LiteralPath (Join-Path $fixtureStage 'licenses\THIRD-PARTY-LICENSES.txt') -Value 'licenses'
    $withoutFolder = Join-Path $fixture 'wrong-folder.zip'
    $wrong = [IO.Compression.ZipFile]::Open($withoutFolder, [IO.Compression.ZipArchiveMode]::Create)
    try { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($wrong, (Join-Path $fixtureStage 'sons_of_sparta.exe'), 'sons_of_sparta.exe') } finally { $wrong.Dispose() }
    Check (@(Compare-ReleaseStageWithZip $fixtureStage $withoutFolder $stem).Count -gt 0) 'a ZIP without the wrapper folder is refused'

    Write-Step 'Only release candidates directly inside dist are removed'
    $dist = Join-Path $temp 'guard\dist'
    New-Item -ItemType Directory -Force -Path (Join-Path $dist $stem), (Join-Path $dist 'steam-artwork'), (Join-Path $temp 'guard\outside') | Out-Null
    Set-Content -LiteralPath (Join-Path $dist 'steam-artwork\capsule.png') -Value 'artwork'
    Set-Content -LiteralPath (Join-Path $temp "guard\outside\$stem") -Value 'not in dist'
    $refused = @{}
    foreach ($candidate in @{ root = $dist; parent = (Join-Path $dist '..'); artwork = (Join-Path $dist 'steam-artwork'); outside = (Join-Path $temp "guard\outside\$stem");
            nested = (Join-Path $dist "$stem\assets"); empty = ''; drive = ([IO.Path]::GetPathRoot($temp)); otherName = (Join-Path $dist 'SonsOfSpartaPC-windows-x64'); otherVersion = (Join-Path $dist 'SonsOfSpartaPC-v1.0.0-windows-x64') }.GetEnumerator()) {
        try { Remove-ReleaseCandidate $dist $candidate.Value $stem; $refused[$candidate.Key] = $false } catch { $refused[$candidate.Key] = $true }
    }
    Check (@($refused.Values | Where-Object { -not $_ }).Count -eq 0) "dist, its parent, the artwork, a path outside, a nested path, an empty path, a drive root and another name are refused ($(($refused.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key }) -join ', '))"
    Check ((Test-Path -LiteralPath (Join-Path $dist 'steam-artwork\capsule.png')) -and (Test-Path -LiteralPath (Join-Path $temp "guard\outside\$stem")) -and (Test-Path -LiteralPath (Join-Path $dist $stem))) 'nothing was deleted by a refused call'
    $linkTarget = Join-Path $temp 'guard\link-target'
    New-Item -ItemType Directory -Force -Path $linkTarget | Out-Null
    Set-Content -LiteralPath (Join-Path $linkTarget 'keep.txt') -Value 'keep'
    $linked = Join-Path $dist "$stem-symbols"
    New-Item -ItemType Junction -Path $linked -Target $linkTarget | Out-Null
    $linkRefused = $false
    try { Remove-ReleaseCandidate $dist $linked $stem } catch { $linkRefused = $true }
    Check ($linkRefused -and (Test-Path -LiteralPath (Join-Path $linkTarget 'keep.txt'))) 'a link that carries a candidate name is refused and its target is untouched'
    (Get-Item -LiteralPath $linked -Force).Delete()
    Remove-ReleaseCandidate $dist (Join-Path $dist $stem) $stem
    Check (-not (Test-Path -LiteralPath (Join-Path $dist $stem)) -and (Test-Path -LiteralPath (Join-Path $dist 'steam-artwork\capsule.png'))) 'the stage is removed and the artwork stays'

    Write-Step 'The real scripts and the documentation'
    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.ps1') -Raw
    $orchestrator = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-release.ps1') -Raw
    $security = Get-Content -LiteralPath (Join-Path $root 'docs\RELEASE_SECURITY.md') -Raw
    Check ($package -match '\$stageRoot = Join-Path \$distDir \$packageStem\r?\n' -and $package -match '\$symbolsDir = Join-Path \$distDir "\$packageStem-symbols"') 'package.ps1 stages in dist and keeps the symbols beside it'
    Check ($package -notmatch 'build\\package') 'package.ps1 has no private copy of the stage under build\package'
    Check ($package -match 'New-ReleaseZip \$stageRoot \$zipPath \$packageStem' -and $package -match 'Test-ReleaseZip \$zipPath \$packageStem \$version \$objdump \$stageRoot') 'package.ps1 makes the ZIP from the stage and compares it with that stage'
    Check ($package -match '(?s)finally \{\s+if \(-not \$completed\) \{.*Remove-ReleaseCandidate \$distDir \$old') 'package.ps1 removes the candidate when it fails'
    Check ($package -match '\$checkTarget = if \(\$Target\) \{ \$Target \} else \{ \$stageRoot \}') 'package.ps1 -VerifyOnly checks the retained stage by default'
    Check ($orchestrator -notmatch 'build\\package\\' -and $orchestrator -match '-VerifyOnly -Target \$target' -and $orchestrator -match 'foreach \(\$target in \$stage, ') 'package-release.ps1 reads the stage from dist and verifies the stage and the ZIP'
    Check ($orchestrator -notmatch 'Remove-Item[^\r\n]*\$stage\b' -and $orchestrator -match 'Remove-ReleaseCandidate \$distDir \$folder') 'package-release.ps1 removes the stage only before packaging and only through the guard'
    foreach ($file in "dist\SonsOfSpartaPC-v<version>-windows-x64.zip", "dist\SonsOfSpartaPC-v<version>-windows-x64\sons_of_sparta.exe", "dist\SonsOfSpartaPC-v<version>-windows-x64\trg-updater.exe", "dist\SonsOfSpartaPC-v<version>-x86_64.AppImage") {
        Check ($security.Contains($file)) "RELEASE_SECURITY.md names $file"
    }
    Check ($security -notmatch 'build\\package' -and $security -match 'PENDING' -and $security -match 'not a guarantee') 'RELEASE_SECURITY.md has no build\package path and keeps PENDING and the no-guarantee wording'
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) check(s) failed." -ForegroundColor Red
    exit 1
}
Write-Host 'All package-release failure-path checks passed.'
exit 0
