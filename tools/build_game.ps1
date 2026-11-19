# Facts about the installed game, shared by tools\build.ps1 and the headless install tests.
Set-StrictMode -Version Latest
$script:PreparedMarker = '.sos_view.json'
$script:ImageFiles = @('boot.bin', 'content.bin', 'image.json')
# Representative complete-game data; the launcher's profile requires the same files.
$script:FullGameFiles = @(
    'Media/globalgamemanagers.assets',
    'Media/Resources/unity default resources',
    'Media/StreamingAssets/aa/catalog.json'
)

# The installed builds: game\prepared\<TITLE_ID>-<version> folders the launcher completed, each with its own image folder.
function Get-PreparedBuilds([string]$PreparedRoot, [string]$ImageRoot) {
    $builds = @()
    if (-not (Test-Path -LiteralPath $PreparedRoot)) { return $builds }
    foreach ($dir in Get-ChildItem -LiteralPath $PreparedRoot -Directory) {
        if ($dir.Name -notmatch '^CUSA\d{5}-\d+\.\d+$') { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $dir.FullName $script:PreparedMarker))) { continue }
        $builds += [pscustomobject]@{
            Folder = $dir.Name
            View = $dir.FullName
            Image = (Join-Path $ImageRoot $dir.Name)
        }
    }
    return $builds
}

function Get-PreparationScope([string]$View) {
    $marker = Join-Path $View $script:PreparedMarker
    if (-not (Test-Path -LiteralPath $marker)) { return '' }
    $text = Get-Content -LiteralPath $marker -Raw
    if ($text -match '"scope"\s*:\s*"([^"]*)"') { return $Matches[1] }
    return ''
}

# A playable installation: the launcher recorded a complete installation and the game data is there.
function Test-FullPreparedView([string]$View, [string[]]$RequiredFiles = $script:FullGameFiles) {
    if ((Get-PreparationScope $View) -ne 'full') { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $View 'eboot.bin'))) { return $false }
    foreach ($name in $RequiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $View $name))) { return $false }
    }
    return $true
}

function Test-ImageFile([string]$Path, [string]$Magic) {
    $bytes = New-Object byte[] $Magic.Length
    $stream = [IO.File]::OpenRead($Path)
    try { $read = $stream.Read($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
    return $read -eq $bytes.Length -and [Text.Encoding]::ASCII.GetString($bytes) -eq $Magic
}

# The installed boot image was built from this view and nothing it depends on is newer.
function Test-ImageCurrent([string]$View, [string]$Image, [string[]]$BuilderSources = @()) {
    $marker = Join-Path $View $script:PreparedMarker
    if (-not (Test-Path -LiteralPath $marker)) { return $false }
    $newest = (Get-Item -LiteralPath $marker).LastWriteTimeUtc
    foreach ($source in $BuilderSources) {
        if (-not (Test-Path -LiteralPath $source)) { continue }
        $time = (Get-Item -LiteralPath $source).LastWriteTimeUtc
        if ($time -gt $newest) { $newest = $time }
    }
    foreach ($name in $script:ImageFiles) {
        $file = Join-Path $Image $name
        if (-not (Test-Path -LiteralPath $file)) { return $false }
        if ((Get-Item -LiteralPath $file).LastWriteTimeUtc -lt $newest) { return $false }
    }
    $content = Join-Path $Image 'content.bin'
    return (Test-ImageFile (Join-Path $Image 'boot.bin') 'SOSBOOT1') -and
           (Test-ImageFile $content 'SOSCONT1') -and
           ((Get-Item -LiteralPath $content).Length -eq 28)
}
