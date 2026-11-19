<#
.SYNOPSIS
  Builds sons_of_sparta.exe incrementally (MSYS2 CLANG64, CMake + Ninja) and, with -Run, launches it.
.DESCRIPTION
  Reuses what is current and updates only what is stale: the video core dependencies (gpu\deps.txt), the CMake
  configuration, the sources and, once the game is installed, the boot image of each installed build. Every DLL the
  game loads ends up beside it. The one build configuration, RelWithDebInfo (-O2 -g -DNDEBUG), is what is tested.
  The game is installed from the launcher (Play page, Install game...), as in a release.
  Every run writes one new log (never overwritten): build\logs\build\build_NNN.log for a build alone, or with -Run the
  session log build\logs\sons_of_sparta_NNN.log, which holds the [build] section and then the loader's [run] lines.
.PARAMETER Run
  After building, starts the game and returns its exit code. The launcher opens first when its rules say so.
.PARAMETER Clean
  Deletes build\cmake first (the installed game, the boot images and the logs are kept).
.PARAMETER CheckImports
  With -Run, runs the headless import diagnostic instead of the game and prints the complete unresolved-import inventory.
.PARAMETER TimeoutSeconds
  Diagnostic: kills the loader after this many seconds (and skips the launcher).
.PARAMETER LoaderArgs
  Extra arguments for sons_of_sparta.exe (with -Run).
.EXAMPLE
  .\tools\build.ps1
  .\tools\build.ps1 -Run
  .\tools\build.ps1 -Run -CheckImports
#>
[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$CheckImports,
    [int]$TimeoutSeconds = 0,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LoaderArgs = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot 'build_game.ps1')
$config = 'RelWithDebInfo'
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
$loader = Join-Path $buildDir 'bin\sons_of_sparta.exe'
$preparedRoot = Join-Path $root 'game\prepared'
$imageRoot = Join-Path $root 'build\image'
# Rebuilding the image depends on these; a newer one makes the installed image stale.
$imageSources = @(
    (Join-Path $root 'launcher\boot_image.cpp'),
    (Join-Path $root 'launcher\boot_image.h'),
    (Join-Path $root 'launcher\game_profile.cpp'),
    (Join-Path $root 'launcher\game_profile.h'),
    (Join-Path $root 'src\sos_image_format.h')
)

$buildMode = if ($Clean) { 'complete (-Clean)' } else { 'incremental' }
$logPlan = Get-BuildLogPlan -Run:$Run
$sessionLog = Start-BuildLog $buildMode $config $logPlan.Directory $logPlan.Name -Session:$logPlan.Session
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    Write-Step "Toolchain clang64 ($($tc.Bin)), $config"
    Sync-GpuDependencies
    Write-Step 'FFmpeg (gpu\ffmpeg.txt)'
    $ffmpegPrefix = Sync-FFmpeg
    $ffmpeg = $ffmpegPrefix.Replace('\', '/')

    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Write-Step 'Cleaning build\cmake'
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    $configured = (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache) -and
        (Select-String -Path $cache -SimpleMatch "CMAKE_BUILD_TYPE:STRING=$config" -Quiet)
    # Another FFmpeg prefix needs a fresh configuration: pkg-config results stay cached otherwise.
    $sameFFmpeg = $configured -and (Select-String -Path $cache -SimpleMatch "DCP_FFMPEG_PREFIX:PATH=$ffmpeg" -Quiet)
    if (-not $configured -or -not $sameFFmpeg) {
        Write-Step 'Configuring'
        Invoke-Native cmake @('-S', $root, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$config", '-DCMAKE_C_COMPILER=clang', "-DDCP_FFMPEG_PREFIX=$ffmpeg", '--fresh')
        if ($LASTEXITCODE -ne 0) { Stop-Tool 'CMake configuration failed' }
    }
    Write-Step 'Building'
    Invoke-Native ninja @('-C', $buildDir, 'sons_of_sparta')
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Build failed' }
    if (-not (Test-Path -LiteralPath $loader)) { Stop-Tool "Missing $loader" }
    Install-FFmpegRuntime $ffmpegPrefix (Split-Path $loader -Parent)

    $installed = @(Get-PreparedBuilds $preparedRoot $imageRoot)
    if ($installed.Count -eq 0) {
        Write-Warn 'The game is not installed yet: start the game and use Install game... on the launcher''s Play page.'
    }
    foreach ($build in $installed) {
        if (-not (Test-FullPreparedView $build.View)) {
            Write-Warn "$($build.Folder) is only partly installed: use Install game... on the launcher's Play page."
            continue
        }
        if (Test-ImageCurrent $build.View $build.Image $imageSources) {
            Write-Ok "Boot image current ($($build.Folder))"
            continue
        }
        Write-Step "Updating the boot image of $($build.Folder)"
        # The call operator waits for a Windows-subsystem program only when its output is piped.
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $loader --prepare-image $build.View $build.Image --logs (Join-Path $root 'build\logs') 2>&1 | Out-BuildLog
        }
        finally { $ErrorActionPreference = $previousPreference }
        if ($LASTEXITCODE -ne 0) { Stop-Tool "Boot image build failed for $($build.Folder)" }
        Write-Ok "Boot image ready ($($build.Folder))"
    }

    Write-Host ''
    Write-Host "Built $loader" -ForegroundColor Green
    Write-BuildLogLine "Built $loader"
}
catch {
    Stop-Tool $_.Exception.Message
}
Close-BuildLog 0
if (-not $Run) { return }

$running = @(Get-Process sons_of_sparta -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $loader })
if ($running.Count -gt 0) {
    Write-Warn "sons_of_sparta.exe is already running (PID $($running.Id -join ', ')); not launching another copy."
    return
}
# The loader finds the installed game, its boot image, user\ and build\logs by itself in this tree, and continues the
# session log. Normal -Run starts the game; -CheckImports runs the headless import diagnostic instead.
$buildSection = [IO.File]::ReadAllText($sessionLog)
$arguments = @()
if ($CheckImports) { $arguments += @('--check-imports', '--list-all') }
if ($TimeoutSeconds -gt 0) { $arguments += '--sos-skip-launcher' }
$arguments += $LoaderArgs
$arguments = @(Get-SessionLoaderArgs $sessionLog $arguments)

Write-Step $(if ($CheckImports) { 'Running import check' } else { 'Starting God of War: Sons of Sparta' })
Push-Location (Split-Path $loader -Parent)
try {
    if ($TimeoutSeconds -gt 0) {
        $process = Start-Process -FilePath $loader -ArgumentList $arguments -NoNewWindow -PassThru
        $null = $process.Handle # keeps the exit code readable after the process ends
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Write-Warn "Stopping sons_of_sparta.exe after $TimeoutSeconds seconds"
            $process.Kill()
            $process.WaitForExit()
        }
        $code = $process.ExitCode
    }
    else {
        & $loader @arguments
        $code = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
Write-Host "sons_of_sparta.exe exit code $code"
Complete-SessionLog $sessionLog $buildSection $code
Write-Host "Session log: $sessionLog"
exit $code
