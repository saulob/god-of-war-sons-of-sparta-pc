# The incremental build of the production GPU target (dcpgpu) and its link smoke target, shared by the groups that need the
# current GPU artifacts. Ninja's dependency graph decides what is stale; nothing is cleaned and no other target is built.
$script:GpuBuildDir = Join-Path $script:RepoRoot 'build\cmake\clang64'
$script:GpuLibrary = Join-Path $script:GpuBuildDir 'gpu\libdcpgpu.a'

function Initialize-GpuBuildDirectory {
    $cache = Join-Path $script:GpuBuildDir 'CMakeCache.txt'
    if ((Test-Path -LiteralPath (Join-Path $script:GpuBuildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache)) { return $true }
    $ffmpeg = (Get-FFmpegPrefix).Replace('\', '/')
    Write-Host "  configuring $script:GpuBuildDir (no configuration exists yet)"
    & cmake -S $script:RepoRoot -B $script:GpuBuildDir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang "-DDCP_FFMPEG_PREFIX=$ffmpeg"
    return ($LASTEXITCODE -eq 0)
}

# Builds the targets (default: the link smoke target, which builds dcpgpu first) and returns $true when they are current.
function Invoke-GpuProductionBuild([string[]]$Targets = @('dcpgpu_link_smoke')) {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $lock = Enter-BuildLock $script:GpuBuildDir
    try {
        if (-not (Initialize-GpuBuildDirectory)) { Write-Host '  [fail] CMake configuration failed'; return $false }
        foreach ($target in $Targets) {
            Write-Host "  building target $target"
            & ninja -C $script:GpuBuildDir $target | Out-Host
            if ($LASTEXITCODE -ne 0) { Write-Host "  [fail] target $target did not compile or link (output above)"; return $false }
        }
    }
    finally { Exit-BuildLock $lock }
    return $true
}
