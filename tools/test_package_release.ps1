# Headless check of how tools\package-release.ps1 fails: a copy of the real script runs in a temporary project with stand-ins for the test
# gate and the packager, so nothing is built or packaged. A failed gate must build no package, a failed packaging step must not leave
# anything that looks like a release in dist\, and a successful run must still write the packages, their checksums and the notes.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

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
$zip = "SonsOfSpartaPC-v$version-windows-x64.zip"

function New-Project([string]$Name) {
    $project = Join-Path $temp $Name
    $tools = Join-Path $project 'tools'
    New-Item -ItemType Directory -Force -Path $tools, (Join-Path $project 'packaging') | Out-Null
    foreach ($file in '_common.ps1', '_evidence.ps1', 'package-release.ps1') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $tools }
    Set-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Value "project(sons_of_sparta_pc VERSION $version LANGUAGES C CXX)"
    Set-Content -LiteralPath (Join-Path $project 'packaging\release-notes.md') -Value "v@VERSION@ @WINDOWS_ASSET@ @WINDOWS_SHA256@ @LINUX_ASSET@ @LINUX_SHA256@"
    Set-Content -LiteralPath (Join-Path $tools 'test.ps1') -Value 'Add-Content -LiteralPath (Join-Path $PSScriptRoot ''..\ran.txt'') -Value ''tests''; exit [int]$env:SOS_STUB_TESTS'
    Set-Content -LiteralPath (Join-Path $tools 'package.ps1') -Value @'
Add-Content -LiteralPath (Join-Path $PSScriptRoot '..\ran.txt') -Value 'package'
$dist = Join-Path $PSScriptRoot '..\dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$name = 'SonsOfSpartaPC-v9.8.7-windows-x64'
Set-Content -LiteralPath (Join-Path $dist "$name.zip") -Value 'zip'
New-Item -ItemType Directory -Force -Path (Join-Path $dist "$name-symbols") | Out-Null
if ($env:SOS_STUB_PACKAGE -eq '1') { exit 1 }
$hash = (Get-FileHash -LiteralPath (Join-Path $dist "$name.zip") -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $dist "$name.zip.sha256") -Value "$hash  $name.zip"
exit 0
'@
    return $project
}

function Invoke-Release([string]$Project, [int]$TestsExit, [int]$PackageExit, [string[]]$Arguments) {
    $env:SOS_STUB_TESTS = "$TestsExit"
    $env:SOS_STUB_PACKAGE = "$PackageExit"
    $output = (& $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $Project 'tools\package-release.ps1') @Arguments 2>&1 | Out-String)
    $code = $LASTEXITCODE
    Remove-Item Env:\SOS_STUB_TESTS, Env:\SOS_STUB_PACKAGE -ErrorAction SilentlyContinue
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
    $run = Invoke-Release $project 1 0 @('-SkipLinux')
    Check ($run.Code -ne 0) 'the exit code is not zero'
    Check ($run.Ran -contains 'tests' -and $run.Ran -notcontains 'package') 'the packager is not started after the failed gate'
    Check (@(Get-DistFiles $project).Count -eq 0) "dist is empty, an older run's files included ($((Get-DistFiles $project) -join ', '))"
    Check ($run.Output -match 'RELEASE NOT PREPARED' -and $run.Output -match 'No package is built') 'the run says the release was not prepared and why'

    Write-Step 'A packaging step that fails after writing files leaves nothing that looks like a release'
    $project = New-Project 'package-fails'
    $run = Invoke-Release $project 0 1 @('-SkipLinux')
    Check ($run.Code -ne 0) 'the exit code is not zero'
    Check ($run.Ran -contains 'package') 'the packager ran'
    Check (@(Get-DistFiles $project).Count -eq 0) "dist is empty ($((Get-DistFiles $project) -join ', '))"
    $quarantine = @(Get-ChildItem -LiteralPath (Join-Path $project 'build\package-failed') -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    Check ($quarantine -contains $zip) 'the partial package was moved to build\package-failed'
    Check ($run.Output -match 'build\\package-failed') 'the run says where the artifacts went'

    Write-Step 'The tests can be skipped and a failed package is still not left in dist'
    $project = New-Project 'skip-tests'
    $run = Invoke-Release $project 1 1 @('-SkipLinux', '-SkipTests')
    Check ($run.Code -ne 0 -and $run.Ran -notcontains 'tests' -and @(Get-DistFiles $project).Count -eq 0) 'with -SkipTests the failed package still leaves dist empty'

    Write-Step 'A successful run still writes the package, its checksum and the notes'
    $project = New-Project 'succeeds'
    $run = Invoke-Release $project 0 0 @('-SkipLinux')
    $files = @(Get-DistFiles $project)
    Check ($run.Code -eq 0) 'the exit code is zero'
    Check ($files -contains $zip -and $files -contains "$zip.sha256" -and $files -contains "release-notes-v$version.md") "dist holds the package, its checksum and the notes ($($files -join ', '))"
    $notes = Get-Content -LiteralPath (Join-Path $project "dist\release-notes-v$version.md") -Raw
    $recorded = ((Get-Content -LiteralPath (Join-Path $project "dist\$zip.sha256") -Raw).Trim() -split '\s+')[0]
    Check ($notes -match [regex]::Escape($recorded) -and $notes -match [regex]::Escape($version)) 'the notes carry the version and the checksum of the package'
    Check (-not (Test-Path -LiteralPath (Join-Path $project 'build\package-failed'))) 'nothing was moved out of dist'

    Write-Step 'A checksum that does not match the package is a failure'
    $project = New-Project 'bad-checksum'
    Set-Content -LiteralPath (Join-Path $project 'tools\package.ps1') -Value @'
$dist = Join-Path $PSScriptRoot '..\dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$name = 'SonsOfSpartaPC-v9.8.7-windows-x64'
Set-Content -LiteralPath (Join-Path $dist "$name.zip") -Value 'zip'
Set-Content -LiteralPath (Join-Path $dist "$name.zip.sha256") -Value ('0' * 64 + "  $name.zip")
exit 0
'@
    $run = Invoke-Release $project 0 0 @('-SkipLinux')
    Check ($run.Code -ne 0 -and @(Get-DistFiles $project).Count -eq 0) 'the release is not prepared and dist is empty'
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
