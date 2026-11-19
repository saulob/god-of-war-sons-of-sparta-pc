<#
.SYNOPSIS
  Checks the prerequisites for building the project and says what is missing.
.PARAMETER InstallPython
  Creates .venv and installs the one Python package the manual package scripts need (cryptography). The launcher
  installs the game itself; the build does not extract packages.
#>
[CmdletBinding()]
param([switch]$InstallPython)

. (Join-Path $PSScriptRoot '_common.ps1')
$problems = 0

Write-Step 'Python (manual package scripts)'
if ($InstallPython) {
    $python = Get-Python
    if (-not $python) { Stop-Tool 'Python 3.10+ is required to create the environment: https://www.python.org/downloads/' }
    if (-not (Get-VenvPython)) {
        Write-Host '  creating .venv'
        & $python.Exe @($python.Args) -m venv (Join-Path $script:RepoRoot '.venv')
        if ($LASTEXITCODE -ne 0) { Stop-Tool 'Could not create .venv' }
    }
    Write-Host '  installing cryptography into .venv'
    & (Get-VenvPython) -m pip install --quiet --disable-pip-version-check cryptography
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'pip install cryptography failed' }
}
$python = Get-Python
if (-not $python) {
    Write-Warn 'Python 3 not found (needed by the manual package scripts)'
} else {
    $version = ((& $python.Exe @($python.Args) '-I' '-c' 'import platform;print(platform.python_version())') | Select-Object -First 1).Trim()
    if ([version]$version -lt [version]'3.10') { Write-Warn "Python $version is too old (need 3.10+)" }
    else { Write-Ok "Python $version ($($python.Exe))" }
    if (Test-PythonModule 'cryptography') { Write-Ok 'cryptography (manual PKG extraction)' }
    else { Write-Warn 'cryptography missing (scripts\pkg_extract.py needs it; the launcher extracts packages itself)' }
}

Write-Step 'Native toolchain (MSYS2 CLANG64)'
$root = Get-Msys2Root
if (-not $root) {
    Write-Bad 'MSYS2 not found (winget install MSYS2.MSYS2, or set MSYS2_ROOT)'; $problems++
} else {
    Write-Ok "MSYS2 at $root"
    $status = Get-ToolchainStatus 'clang64'
    if ($status.Ready) {
        $version = (& (Join-Path $status.Bin 'clang.exe') --version | Select-Object -First 1)
        Write-Ok "clang, CMake, Ninja ($($status.Bin)): $version"
    } else {
        Write-Bad "CLANG64 incomplete, missing: $($status.Missing -join ', ')"; $problems++
        Write-Host '         Install from an "MSYS2 CLANG64" shell:'
        Write-Host '         pacman -S --needed mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,sdl3,freetype,zlib}'
        Write-Host '         pacman -S --needed mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang}'
        Write-Host '         pacman -S --needed mingw-w64-clang-x86_64-{fmt,boost,robin-map,xxhash,libpng,zydis,spirv-tools,spirv-headers,spirv-cross}'
    }
}

Write-Step 'FFmpeg build tools (tools\build-ffmpeg.sh builds the pinned FFmpeg during tools\build.ps1)'
if ($root) {
    $missingTools = @('make', 'nasm', 'xz', 'curl', 'tar', 'sha256sum') | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $root "usr\bin\$_.exe")) -and -not (Test-Path -LiteralPath (Join-Path $root "clang64\bin\$_.exe"))
    }
    if ($missingTools) {
        Write-Bad "missing in MSYS2: $($missingTools -join ', ')"; $problems++
        Write-Host '         pacman -S --needed make nasm xz curl tar coreutils'
    } else { Write-Ok 'make, nasm, xz, curl, tar, sha256sum' }
}

Write-Step 'Game package (optional, only for the manual scripts)'
$found = $false
foreach ($dir in @((Join-Path $script:RepoRoot 'game'), 'E:\Decomp_Downloads\Sons of Sparta')) {
    if (Test-Path -LiteralPath $dir) {
        $pkgs = @(Get-ChildItem -LiteralPath $dir -Filter '*.pkg' -File -ErrorAction SilentlyContinue)
        if ($pkgs.Count -gt 0) { Write-Ok "$dir ($($pkgs.Count) package(s))"; $found = $true }
    }
}
if (-not $found) { Write-Warn 'no .pkg found in game\: install the game from the launcher''s Play page instead' }

Write-Host ''
if ($problems -gt 0) { Write-Host "$problems required item(s) missing." -ForegroundColor Red; exit 1 }
Write-Host 'Ready.' -ForegroundColor Green
