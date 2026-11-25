# The local evidence of the supported builds: where a headless tool finds the game data of a build, how a test reports it
# (EVIDENCE|<kind>|<build>|<state>|<detail>) and how the test run sums it up. A supported build is a contract of the port, checked on
# every run; what is read from a real boot image or game folder is extra evidence, and its absence is DATA UNAVAILABLE, never a
# failure and never a pass. Works in Windows PowerShell 5.1 and PowerShell 7. SOS_EVIDENCE_ROOT replaces the project folder as the
# place of build\ and game\, for the tests of this file.

$script:EvidenceProject = Split-Path -Parent $PSScriptRoot
$script:EvidenceTitle = 'CUSA57767'
$script:EvidenceBaseVersion = '1.00'

function Get-EvidenceRoot {
    if ($env:SOS_EVIDENCE_ROOT) { return $env:SOS_EVIDENCE_ROOT }
    return $script:EvidenceProject
}

function Get-BuildKey([string]$Version) { return "$script:EvidenceTitle-$Version" }

function Get-BuildSha256([string]$Version) {
    $header = Get-Content -LiteralPath (Join-Path $script:EvidenceProject 'src\game_build.h') -Raw
    $match = [regex]::Match($header, '"' + [regex]::Escape($script:EvidenceTitle) + '", "' + [regex]::Escape($Version) + '",\s*"[^"]*",\s*"([0-9a-f]{64})"')
    if ($match.Success) { return $match.Groups[1].Value }
    return $null
}

# The game folders of a build that exist, the prepared one before the extracted package layers (the update's before the base game's).
function Get-BuildGameFolders([string]$Version) {
    $root = Get-EvidenceRoot
    $names = @("game\prepared\$(Get-BuildKey $Version)", "game\prepared\v$Version", "game\extracted\$(Get-BuildKey $Version)", "game\extracted\v$Version")
    if ($Version -ne $script:EvidenceBaseVersion) {
        $names += "game\extracted\$(Get-BuildKey $script:EvidenceBaseVersion)", "game\extracted\v$script:EvidenceBaseVersion"
    }
    return @($names | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
}

function Find-BuildGameFile([string]$Version, [string]$Relative) {
    foreach ($folder in Get-BuildGameFolders $Version) {
        $path = Join-Path $folder $Relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    return $null
}

# The boot image of a build as @{ Path; State } with State REAL IMAGE (the launcher's build\image) or RESEARCH IMAGE
# (build\research\image), or $null. The image must be of this build's executable.
function Find-BuildBootImage([string]$Version) {
    $root = Join-Path (Get-EvidenceRoot) 'build'
    $sha = Get-BuildSha256 $Version
    foreach ($place in @(@('REAL IMAGE', 'image'), @('RESEARCH IMAGE', 'research\image'))) {
        foreach ($folder in @((Get-BuildKey $Version), "v$Version")) {
            $directory = Join-Path $root (Join-Path $place[1] $folder)
            $manifest = Join-Path $directory 'image.json'
            $boot = Join-Path $directory 'boot.bin'
            if (-not ((Test-Path -LiteralPath $manifest -PathType Leaf) -and (Test-Path -LiteralPath $boot -PathType Leaf))) { continue }
            if ($sha -and (Get-Content -LiteralPath $manifest -Raw) -match ('"eboot_sha256"\s*:\s*"' + $sha + '"')) {
                return [pscustomobject]@{ Path = $boot; State = $place[0] }
            }
        }
    }
    return $null
}

function Write-Evidence([string]$Kind, [string]$Version, [string]$State, [string]$Detail) {
    Write-Host "EVIDENCE|$Kind|$(Get-BuildKey $Version)|$State|$Detail"
}

function Write-GameFileEvidence([string]$Version, [string]$Found, [string]$What) {
    if ($Found) {
        $state = if ($Found -like (Join-Path (Get-EvidenceRoot) 'game\prepared\*')) { 'AVAILABLE' } else { 'EXTRACTED LAYERS' }
        Write-Evidence 'content' $Version $state ($Found.Substring((Get-EvidenceRoot).Length).TrimStart('\').Replace('\', '/'))
    } else {
        Write-Evidence 'content' $Version 'NOT INSTALLED' "no game folder; not read: $What"
    }
}

# --- the summary of a test run ------------------------------------------------------------------------------------------

function ConvertFrom-EvidenceLine([string]$Line) {
    $parts = "$Line".Trim().Split('|', 5)
    if ($parts.Count -ne 5 -or $parts[0] -ne 'EVIDENCE') { return $null }
    return [pscustomobject]@{ Kind = $parts[1]; Build = $parts[2]; State = $parts[3]; Detail = $parts[4] }
}

function Get-EvidenceStates([object[]]$Records, [string]$Build, [string]$Kind) {
    return @($Records | Where-Object { $_.Build -eq $Build -and $_.Kind -eq $Kind })
}

function Format-GroupList([object[]]$Records) {
    $names = @($Records | ForEach-Object { $_.Group } | Select-Object -Unique)
    if ($names.Count -le 4) { return ($names -join ', ') }
    return (($names | Select-Object -First 4) -join ', ') + ", and $($names.Count - 4) more"
}

# One block per build: the contract (every build group passed), the real image (PASS, FAIL or DATA UNAVAILABLE, never mixed up) and the
# prepared content. $Records: Group, Scope, Passed, Kind, Build, State, Detail. $Groups: Name, Scope, Passed of every group that ran.
function Format-BuildEvidenceSummary([string[]]$Builds, [object[]]$Records, [object[]]$Groups) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('Supported build evidence (the contract is checked for every build on every run; local game data is extra, and its absence is reported, not counted as a pass or a failure)')
    $buildGroups = @($Groups | Where-Object { $_.Scope -eq 'build' })
    foreach ($build in $Builds) {
        $lines.Add($build)
        $failed = @($buildGroups | Where-Object { -not $_.Passed })
        $contract = if ($failed.Count) { 'FAIL (' + (($failed | ForEach-Object { $_.Name }) -join ', ') + ')' } else { "PASS ($($buildGroups.Count) build groups)" }
        $lines.Add("  contract:          $contract")

        $images = Get-EvidenceStates $Records $build 'image'
        $real = @($images | Where-Object { $_.State -in 'REAL IMAGE', 'RESEARCH IMAGE' })
        $none = @($images | Where-Object { $_.State -eq 'NO DATA' })
        $realFailed = @($real | Where-Object { -not $_.Passed })
        $source = @($real | ForEach-Object { $_.State } | Select-Object -Unique) -join ' and '
        if ($realFailed.Count) { $image = 'FAIL (' + (Format-GroupList $realFailed) + ')' }
        elseif ($real.Count -and -not $none.Count) { $image = "PASS ($($real.Count) groups, $source)" }
        elseif ($real.Count) { $image = "PASS in $($real.Count) groups ($source); DATA UNAVAILABLE in $($none.Count) groups ($(Format-GroupList $none))" }
        elseif ($none.Count) { $image = "DATA UNAVAILABLE ($($none.Count) groups: no boot image in build\image or build\research\image)" }
        else { $image = 'DATA UNAVAILABLE (no group reported it)' }
        $lines.Add("  real image:        $image")

        $contents = Get-EvidenceStates $Records $build 'content'
        $prepared = @($contents | Where-Object { $_.State -eq 'AVAILABLE' })
        $layers = @($contents | Where-Object { $_.State -eq 'EXTRACTED LAYERS' })
        $absent = @($contents | Where-Object { $_.State -eq 'NOT INSTALLED' })
        if ($prepared.Count -and -not $absent.Count -and -not $layers.Count) { $content = "AVAILABLE ($($prepared.Count) groups)" }
        elseif ($prepared.Count) { $content = "AVAILABLE ($($prepared.Count) groups); not read by $($absent.Count + $layers.Count) groups" }
        elseif ($layers.Count -or $absent.Count) {
            $detail = @()
            if ($layers.Count) { $detail += "extracted package layers read by $($layers.Count) groups" }
            if ($absent.Count) { $detail += "no game data for $($absent.Count) groups ($(Format-GroupList $absent))" }
            $content = 'NOT INSTALLED (' + ($detail -join '; ') + ')'
        } else { $content = 'NOT REPORTED' }
        $lines.Add("  prepared content:  $content")
    }
    return $lines.ToArray()
}
