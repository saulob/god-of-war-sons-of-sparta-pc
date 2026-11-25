function Get-ReleaseFullPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Test-ReleaseCandidate([string]$Dist, [string]$Path, [string]$Stem) {
    if (-not $Dist -or -not $Path -or -not $Stem -or $Stem.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) { return $false }
    $distFull = Get-ReleaseFullPath $Dist
    $full = Get-ReleaseFullPath $Path
    if ([IO.Path]::GetPathRoot($distFull + '\').TrimEnd('\') -eq $distFull) { return $false }
    $parent = [IO.Path]::GetDirectoryName($full)
    if (-not $parent -or -not $parent.Equals($distFull, [StringComparison]::OrdinalIgnoreCase)) { return $false }
    return @($Stem, "$Stem-symbols", "$Stem.zip", "$Stem.zip.sha256") -ccontains [IO.Path]::GetFileName($full)
}

function Remove-ReleaseCandidate([string]$Dist, [string]$Path, [string]$Stem) {
    if (-not (Test-ReleaseCandidate $Dist $Path $Stem)) { throw "Refusing to remove '$Path': it is not a Windows release candidate directly inside '$Dist'" }
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing to remove '$Path': it is a link" }
    Remove-Item -LiteralPath $Path -Recurse -Force
}

function Get-ReleaseFileMap([string]$Folder) {
    $map = New-Object 'System.Collections.Generic.Dictionary[string,System.IO.FileInfo]' ([StringComparer]::Ordinal)
    $skip = (Get-ReleaseFullPath $Folder).Length + 1
    foreach ($file in Get-ChildItem -LiteralPath $Folder -Recurse -File -Force) {
        $map[$file.FullName.Substring($skip).Replace('\', '/')] = $file
    }
    return $map
}

function Compare-ReleaseFolders([string]$Stage, [string]$Extracted) {
    $problems = New-Object System.Collections.Generic.List[string]
    $left = Get-ReleaseFileMap $Stage
    $right = Get-ReleaseFileMap $Extracted
    if ($left.Count -ne $right.Count) { $problems.Add("file count: stage $($left.Count), ZIP $($right.Count)") }
    foreach ($name in ($left.Keys | Sort-Object)) {
        if (-not $right.ContainsKey($name)) { $problems.Add("missing from the ZIP: $name"); continue }
        if ($left[$name].Length -ne $right[$name].Length) { $problems.Add("size differs: $name"); continue }
        $a = (Get-FileHash -LiteralPath $left[$name].FullName -Algorithm SHA256).Hash
        $b = (Get-FileHash -LiteralPath $right[$name].FullName -Algorithm SHA256).Hash
        if ($a -ne $b) { $problems.Add("content differs: $name") }
    }
    foreach ($name in ($right.Keys | Sort-Object)) {
        if (-not $left.ContainsKey($name)) { $problems.Add("not in the stage: $name") }
    }
    return $problems.ToArray()
}

function New-ReleaseZip([string]$Stage, [string]$ZipPath, [string]$Name) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $skip = (Get-ReleaseFullPath $Stage).Length + 1
    $files = @(Get-ChildItem -LiteralPath $Stage -Recurse -File -Force |
        Sort-Object { $_.FullName.Substring($skip).Replace('\', '/') })
    $stream = [IO.File]::Open($ZipPath, [IO.FileMode]::CreateNew)
    try {
        $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in $files) {
                $entry = "$Name/" + $file.FullName.Substring($skip).Replace('\', '/')
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
    return $files.Count
}

function Compare-ReleaseStageWithZip([string]$Stage, [string]$ZipPath, [string]$Name) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $tempRoot = Get-ReleaseFullPath ([IO.Path]::GetTempPath())
    $temp = Join-Path $tempRoot ('sos_zip_compare_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    try {
        $archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
        try {
            foreach ($entry in $archive.Entries) {
                $full = $entry.FullName
                if ($full.Contains('\') -or $full.Contains('..') -or $full.StartsWith('/') -or $full.Contains(':')) { return @("unsafe ZIP entry path: $full") }
                if (-not $full.StartsWith("$Name/")) { return @("ZIP entry outside $Name/: $full") }
            }
        } finally { $archive.Dispose() }
        [IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $temp)
        $content = Join-Path $temp $Name
        if (-not (Test-Path -LiteralPath $content -PathType Container)) { return @("the ZIP has no $Name/ folder") }
        return Compare-ReleaseFolders $Stage $content
    } finally {
        $parent = [IO.Path]::GetDirectoryName((Get-ReleaseFullPath $temp))
        if ((Split-Path -Leaf $temp) -like 'sos_zip_compare_*' -and $parent -eq $tempRoot -and (Test-Path -LiteralPath $temp)) {
            Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
