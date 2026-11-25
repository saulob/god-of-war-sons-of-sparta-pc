<#
.SYNOPSIS
  Builds the Windows x64 Release of God of War: Sons of Sparta PC and packages a verified local ZIP under dist\.
.DESCRIPTION
  Produces SonsOfSpartaPC-v<version>-windows-x64.zip (+ .sha256); the Linux counterpart is
  SonsOfSpartaPC-v<version>-x86_64.AppImage (+ .sha256) built by tools/package-linux.sh.
  Builds CMake Release (build\cmake\clang64-release), verifies PE metadata (GUI subsystem 2, ProductVersion ==
  CMakeLists.txt VERSION), splits debug symbols to dist\<name>-symbols\, resolves transitive DLL imports via
  llvm-objdump, verifies LGPL FFmpeg, collects all third-party and MSYS2 licenses, scans for forbidden files,
  magic bytes, local machine paths (ASCII & UTF-16LE), DEBUG_SAULO, and updater-protected paths, writes
  dist\<name>.zip + .zip.sha256, and re-extracts the ZIP to verify every file SHA-256. Never publishes anything.
.PARAMETER VerifyOnly
  Skips building and re-verifies an existing dist\<name>.zip (or folder passed via -Target).
#>
[CmdletBinding()]
param(
    [switch]$VerifyOnly,
    [string]$Target = '',
    [switch]$Clean
)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot

function Get-ProjectVersion {
    $text = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    if ($text -match 'project\s*\(\s*sons_of_sparta_pc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        return $Matches[1]
    }
    Stop-Tool 'Could not read project VERSION from CMakeLists.txt'
}

function Get-PeSubsystem([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return -1 }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 94 -gt $bytes.Length) { return -1 }
    if ($bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45 -or $bytes[$pe + 2] -ne 0 -or $bytes[$pe + 3] -ne 0) { return -1 }
    return [BitConverter]::ToUInt16($bytes, $pe + 24 + 68)
}

function Get-PeMachine([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64) { return 0 }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 6 -gt $bytes.Length) { return 0 }
    return [BitConverter]::ToUInt16($bytes, $pe + 4)
}

function Get-PeImports([string]$Objdump, [string]$Path) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = & $Objdump -p $Path 2>$null } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { Stop-Tool "llvm-objdump -p failed on $Path" }
    $imports = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines) {
        if ($line -match '^\s*DLL Name:\s*(\S+)') { $imports.Add($Matches[1]) }
    }
    return @($imports | Sort-Object -Unique)
}

function Test-WindowsSystemDll([string]$Name) {
    $lower = $Name.ToLowerInvariant()
    if ($lower -match '^(api-ms-win-|ext-ms-win-)') { return $true }
    $sys = @(
        'advapi32.dll', 'avrt.dll', 'bcrypt.dll', 'cfgmgr32.dll', 'comctl32.dll', 'comdlg32.dll',
        'crypt32.dll', 'd3d11.dll', 'd3d12.dll', 'dbghelp.dll', 'dinput8.dll', 'dwmapi.dll',
        'dwrite.dll', 'dxgi.dll', 'gdi32.dll', 'hid.dll', 'imm32.dll', 'iphlpapi.dll', 'kernel32.dll',
        'kernelbase.dll', 'mmdevapi.dll', 'msvcrt.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll',
        'opengl32.dll', 'powrprof.dll', 'propsys.dll', 'psapi.dll', 'rpcrt4.dll', 'secur32.dll',
        'setupapi.dll', 'shell32.dll', 'shlwapi.dll', 'ucrtbase.dll', 'user32.dll', 'userenv.dll',
        'usp10.dll', 'uxtheme.dll', 'version.dll', 'vulkan-1.dll', 'winhttp.dll', 'winmm.dll', 'ws2_32.dll',
        'xinput1_4.dll', 'xinput9_1_0.dll', 'api-ms-win-core-synch-l1-2-0.dll', 'synchronization.dll'
    )
    return $sys -contains $lower
}

function Get-DllClosure([string]$Objdump, [string[]]$Roots, [string[]]$SearchDirs) {
    $found = [ordered]@{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($r in $Roots) { $queue.Enqueue($r) }
    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        foreach ($dll in Get-PeImports $Objdump $current) {
            if (Test-WindowsSystemDll $dll) { continue }
            $key = $dll.ToLowerInvariant()
            if ($found.Contains($key)) { continue }
            $resolved = $null
            foreach ($dir in $SearchDirs) {
                $candidate = Join-Path $dir $dll
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $resolved = $candidate; break }
            }
            if (-not $resolved) { Stop-Tool "Unresolved non-system DLL '$dll' imported by $(Split-Path $current -Leaf)" }
            $found[$key] = $resolved
            $queue.Enqueue($resolved)
        }
    }
    return $found
}

function Get-UpdaterProtectedRules {
    $cpp = Get-Content -LiteralPath (Join-Path $root 'launcher\updater\update_paths.cpp') -Raw
    $parseList = {
        param([string]$Name)
        if ($cpp -match "${Name}\[\]\s*=\s*\{([^}]+)\}") {
            return @([regex]::Matches($Matches[1], '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() })
        }
        Stop-Tool "Could not parse $Name from launcher\updater\update_paths.cpp"
    }
    return @{
        Names = & $parseList 'kProtectedNames'
        Files = & $parseList 'kProtectedFiles'
        Exts  = & $parseList 'kProtectedExtensions'
    }
}

function Test-ReleaseFolder([string]$Folder, [string]$Version, [string]$Objdump) {
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) {
        $problems.Add("Missing release folder: $Folder")
        return $problems
    }
    $exe = Join-Path $Folder 'sons_of_sparta.exe'
    $updater = Join-Path $Folder 'trg-updater.exe'
    foreach ($req in 'sons_of_sparta.exe', 'trg-updater.exe', 'README.txt', 'LICENSE.txt',
                     'assets\launcher\header-art.png', 'assets\launcher\sponsor-heart.png', 'licenses\THIRD-PARTY-LICENSES.txt',
                     'licenses\VENDOR.txt', 'licenses\components\Boost\LICENSE_1_0.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $Folder $req) -PathType Leaf)) {
            $problems.Add("Missing required file: $req")
        }
    }
    if (Test-Path -LiteralPath $exe) {
        $sub = Get-PeSubsystem $exe
        if ($sub -ne 2) { $problems.Add("sons_of_sparta.exe PE subsystem is $sub (expected 2 GUI)") }
        $mach = Get-PeMachine $exe
        if ($mach -ne 0x8664) { $problems.Add("sons_of_sparta.exe machine is 0x$($mach.ToString('X')) (expected 0x8664 x64)") }
        $vi = (Get-Item -LiteralPath $exe).VersionInfo
        if ($vi.ProductVersion -ne $Version) { $problems.Add("sons_of_sparta.exe ProductVersion='$($vi.ProductVersion)' (expected '$Version')") }
        if (-not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe)).Contains('>UTF-8</activeCodePage>')) {
            $problems.Add('sons_of_sparta.exe has no UTF-8 code page manifest')
        }
        if ($vi.ProductName -ne 'God of War: Sons of Sparta PC') { $problems.Add("sons_of_sparta.exe ProductName='$($vi.ProductName)'") }
    }
    if (Test-Path -LiteralPath $updater) {
        $sub = Get-PeSubsystem $updater
        if ($sub -ne 2) { $problems.Add("trg-updater.exe PE subsystem is $sub (expected 2 GUI)") }
        if ($Objdump) {
            foreach ($imp in Get-PeImports $Objdump $updater) {
                if (-not (Test-WindowsSystemDll $imp)) { $problems.Add("trg-updater.exe imports non-system DLL: $imp") }
            }
        }
    }
    if ($Objdump -and (Test-Path -LiteralPath $exe)) {
        $stagedDlls = @{}
        foreach ($d in Get-ChildItem -LiteralPath $Folder -Filter '*.dll' -File) {
            $stagedDlls[$d.Name.ToLowerInvariant()] = $d.FullName
        }
        $checked = @{}
        $q = New-Object System.Collections.Generic.Queue[string]
        $q.Enqueue($exe)
        while ($q.Count -gt 0) {
            $cur = $q.Dequeue()
            foreach ($imp in Get-PeImports $Objdump $cur) {
                if (Test-WindowsSystemDll $imp) { continue }
                $k = $imp.ToLowerInvariant()
                if (-not $stagedDlls.ContainsKey($k)) {
                    $problems.Add("Missing staged DLL '$imp' required by $(Split-Path $cur -Leaf)")
                    continue
                }
                if (-not $checked.ContainsKey($k)) {
                    $checked[$k] = $true
                    $q.Enqueue($stagedDlls[$k])
                }
            }
        }
    }

    $protected = Get-UpdaterProtectedRules
    $forbiddenNames = @('eboot.bin', 'boot.bin', 'param.sfo', 'content.bin', 'sons_of_sparta.ini', 'renderdoc.dll', 'header-art-source.png')
    $forbiddenExts = @('.pkg', '.pdb', '.debug', '.dmp', '.ilk', '.log', '.self', '.prx', '.sprx', '.sfo', '.trp', '.sav', '.part')
    $needles = @(
        'DEBUG_SAULO',
        'S:\VisualStudio',
        'S:/VisualStudio',
        's:\visualstudio',
        's:/visualstudio',
        'C:\Users\',
        'C:/Users/'
    )
    if ($env:USERPROFILE) { $needles += @($env:USERPROFILE, $env:USERPROFILE.Replace('\', '/')) }
    $needles = @($needles | Where-Object { $_ } | Sort-Object -Unique)

    $files = Get-ChildItem -LiteralPath $Folder -Recurse -File
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($Folder.Length + 1).Replace('\', '/')
        $relLower = $rel.ToLowerInvariant()
        $nameLower = $f.Name.ToLowerInvariant()
        $extLower = $f.Extension.ToLowerInvariant()
        $segments = $relLower -split '/'

        if ($forbiddenNames -contains $nameLower) { $problems.Add("Forbidden file in package: $rel") }
        if ($forbiddenExts -contains $extLower) { $problems.Add("Forbidden extension in package: $rel") }
        if ($nameLower -like 'vklayer_*' -or $nameLower -like 'renderdoc*') { $problems.Add("Forbidden debug/layer file: $rel") }
        if ($protected.Files -contains $nameLower) { $problems.Add("Updater-protected filename staged: $rel") }
        if ($protected.Exts -contains $extLower) { $problems.Add("Updater-protected extension staged: $rel") }
        foreach ($seg in $segments) {
            if ($protected.Names -contains $seg) { $problems.Add("Updater-protected path segment '$seg' in $rel") }
        }

        if ($extLower -eq '.dll' -and ($nameLower -like 'av*.dll' -or $nameLower -like 'sw*.dll')) {
            if (-not (Test-LgplFFmpegDll $f.FullName)) {
                $problems.Add("FFmpeg DLL is not the pinned LGPL build: $rel")
            }
        }

        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        if ($bytes.Length -ge 4) {
            if ($bytes[0] -eq 0x7F -and $bytes[1] -eq 0x43 -and $bytes[2] -eq 0x4E -and $bytes[3] -eq 0x54) { $problems.Add("PS4 PKG magic bytes in $rel") }
            if ($bytes[0] -eq 0x4F -and $bytes[1] -eq 0x15 -and $bytes[2] -eq 0x3D -and $bytes[3] -eq 0x1D) { $problems.Add("PS4 SELF magic bytes in $rel") }
            if ($bytes[0] -eq 0x7F -and $bytes[1] -eq 0x45 -and $bytes[2] -eq 0x4C -and $bytes[3] -eq 0x46) { $problems.Add("ELF magic bytes in Windows package: $rel") }
        }
        $ascii = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $utf16 = if ($bytes.Length -ge 2) { [Text.Encoding]::Unicode.GetString($bytes) } else { '' }
        foreach ($needle in $needles) {
            if ($ascii.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $problems.Add("Forbidden ASCII string '$needle' in $rel")
            }
            if ($utf16.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $problems.Add("Forbidden UTF-16LE string '$needle' in $rel")
            }
        }
        if ($extLower -in '.exe', '.dll') {
            if ($ascii.Contains('.debug_info')) {
                $problems.Add("Unstripped DWARF .debug_info found in $rel")
            }
        }
    }
    return $problems
}

function Test-ReleaseZip([string]$ZipPath, [string]$ExpectedPrefix, [string]$Version, [string]$Objdump, [string]$StagedDir = '') {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('sos_zip_verify_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    try {
        $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
        try {
            if ($zip.Entries.Count -eq 0) { Stop-Tool "ZIP is empty: $ZipPath" }
            foreach ($entry in $zip.Entries) {
                $full = $entry.FullName
                if ($full.Contains('\') -or $full.Contains('..') -or $full.StartsWith('/') -or $full.Contains(':')) {
                    Stop-Tool "Unsafe ZIP entry path: $full"
                }
                if (-not $full.StartsWith("$ExpectedPrefix/")) {
                    Stop-Tool "ZIP entry outside wrapper folder '$ExpectedPrefix/': $full"
                }
            }
        } finally { $zip.Dispose() }

        [IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $temp)
        $extracted = Join-Path $temp $ExpectedPrefix
        $problems = @(Test-ReleaseFolder $extracted $Version $Objdump)
        if ($problems.Count -gt 0) {
            $problems | ForEach-Object { Write-Bad $_ }
            Stop-Tool "Extracted ZIP verification failed ($($problems.Count) problems)"
        }
        if ($StagedDir) {
            $stagedFiles = @(Get-ChildItem -LiteralPath $StagedDir -Recurse -File | Sort-Object FullName)
            $extFiles = @(Get-ChildItem -LiteralPath $extracted -Recurse -File | Sort-Object FullName)
            if ($stagedFiles.Count -ne $extFiles.Count) {
                Stop-Tool "ZIP file count mismatch: staged $($stagedFiles.Count) vs extracted $($extFiles.Count)"
            }
            for ($i = 0; $i -lt $stagedFiles.Count; $i++) {
                $h1 = (Get-FileHash -LiteralPath $stagedFiles[$i].FullName -Algorithm SHA256).Hash
                $h2 = (Get-FileHash -LiteralPath $extFiles[$i].FullName -Algorithm SHA256).Hash
                if ($h1 -ne $h2) { Stop-Tool "SHA-256 mismatch after ZIP round-trip for $($stagedFiles[$i].Name)" }
            }
        }
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$version = Get-ProjectVersion
$packageStem = "SonsOfSpartaPC-v$version-windows-x64"
$packageFile = "SonsOfSpartaPC-v$version-windows-x64.zip"
$distDir = Join-Path $root 'dist'
$zipPath = Join-Path $distDir $packageFile
$shaPath = "$zipPath.sha256"

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$objdump = Join-Path $tc.Bin 'llvm-objdump.exe'
$objcopy = Join-Path $tc.Bin 'llvm-objcopy.exe'
if (-not (Test-Path -LiteralPath $objdump)) { Stop-Tool "Missing $objdump" }
if (-not (Test-Path -LiteralPath $objcopy)) { Stop-Tool "Missing $objcopy" }

if ($VerifyOnly) {
    $checkTarget = if ($Target) { $Target } else { $zipPath }
    Write-Step "Verifying $checkTarget"
    if (Test-Path -LiteralPath $checkTarget -PathType Container) {
        $problems = @(Test-ReleaseFolder $checkTarget $version $objdump)
        if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Bad $_ }; Stop-Tool 'Verification failed' }
    } else {
        Test-ReleaseZip $checkTarget $packageStem $version $objdump
    }
    Write-Ok "Verified $checkTarget"
    return
}

$config = 'Release'
$buildDir = Join-Path $root 'build\cmake\clang64-release'
$binDir = Join-Path $buildDir 'bin'
$exePath = Join-Path $binDir 'sons_of_sparta.exe'
$updaterPath = Join-Path $binDir 'trg-updater.exe'

Write-Step "Windows Release build ($config, v$version)"
$buildLock = Enter-BuildLock $buildDir
try {
    Sync-GpuDependencies
    Write-Step 'FFmpeg (gpu\ffmpeg.txt)'
    $ffmpegPrefix = Sync-FFmpeg
    $ffmpeg = $ffmpegPrefix.Replace('\', '/')
    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    $configured = (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache) -and
        (Select-String -Path $cache -SimpleMatch "CMAKE_BUILD_TYPE:STRING=$config" -Quiet) -and
        (Select-String -Path $cache -SimpleMatch "DCP_FFMPEG_PREFIX:PATH=$ffmpeg" -Quiet)
    if (-not $configured) {
        Write-Step "Configuring $buildDir ($config)"
        Invoke-Native cmake @('-S', $root, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$config", '-DCMAKE_C_COMPILER=clang', "-DDCP_FFMPEG_PREFIX=$ffmpeg", '--fresh')
        if ($LASTEXITCODE -ne 0) { Stop-Tool 'Release CMake configuration failed' }
    }
    Write-Step 'Building sons_of_sparta and trg-updater (Release)'
    Invoke-Native ninja @('-C', $buildDir, 'sons_of_sparta', 'trg-updater')
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Release Ninja build failed' }
} finally {
    Exit-BuildLock $buildLock
}

if (-not (Test-Path -LiteralPath $exePath)) { Stop-Tool "Missing $exePath" }
if (-not (Test-Path -LiteralPath $updaterPath)) { Stop-Tool "Missing $updaterPath" }
Install-FFmpegRuntime $ffmpegPrefix $binDir

$stageRoot = Join-Path $root "build\package\$packageStem"
$symbolsDir = Join-Path $distDir "$packageStem-symbols"
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
if (Test-Path -LiteralPath $symbolsDir) { Remove-Item -LiteralPath $symbolsDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stageRoot, $symbolsDir, $distDir | Out-Null

Write-Step 'Staging executables and splitting debug symbols'
foreach ($bin in $exePath, $updaterPath) {
    $name = Split-Path $bin -Leaf
    $staged = Join-Path $stageRoot $name
    $sym = Join-Path $symbolsDir ([IO.Path]::ChangeExtension($name, '.debug'))
    Copy-Item -LiteralPath $bin -Destination $staged -Force
    & $objcopy --only-keep-debug $staged $sym
    if ($LASTEXITCODE -ne 0) { Stop-Tool "llvm-objcopy --only-keep-debug failed on $name" }
    & $objcopy --strip-debug $staged
    if ($LASTEXITCODE -ne 0) { Stop-Tool "llvm-objcopy --strip-debug failed on $name" }
}

Write-Step 'Resolving transitive DLL dependencies from PE import tables'
$searchDirs = @($binDir, (Join-Path $ffmpegPrefix 'bin'), $tc.Bin)
$closure = Get-DllClosure $objdump @((Join-Path $stageRoot 'sons_of_sparta.exe')) $searchDirs
$msysRoot = Get-Msys2Root
$pacman = if ($msysRoot) { Join-Path $msysRoot 'usr\bin\pacman.exe' } else { $null }
$licensesDir = Join-Path $stageRoot 'licenses'
New-Item -ItemType Directory -Force -Path $licensesDir | Out-Null

$msysPackages = New-Object System.Collections.Generic.HashSet[string]
foreach ($entry in $closure.GetEnumerator()) {
    $dllFile = $entry.Value
    $dllName = Split-Path $dllFile -Leaf
    Copy-Item -LiteralPath $dllFile -Destination (Join-Path $stageRoot $dllName) -Force
    Write-Host "  [dll]  $dllName <= $dllFile"
    if ($pacman -and $dllFile.StartsWith($tc.Bin, [StringComparison]::OrdinalIgnoreCase)) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $pkg = (& $pacman -Qqo ($dllFile -replace '\\', '/') 2>$null | Out-String).Trim()
            if ($LASTEXITCODE -eq 0 -and $pkg) { [void]$msysPackages.Add($pkg) }
        } finally { $ErrorActionPreference = $prev }
    }
}

Write-Step 'Staging assets, README, and licenses'
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot 'assets\launcher') | Out-Null
foreach ($image in 'header-art.png', 'sponsor-heart.png') {
    Copy-Item -LiteralPath (Join-Path $root "assets\launcher\$image") -Destination (Join-Path $stageRoot "assets\launcher\$image") -Force
}
Copy-Item -LiteralPath (Join-Path $root 'packaging\README.txt') -Destination (Join-Path $stageRoot 'README.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stageRoot 'LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'packaging\licenses\THIRD-PARTY-LICENSES.txt') -Destination (Join-Path $licensesDir 'THIRD-PARTY-LICENSES.txt') -Force
$componentsSrc = Join-Path $root 'packaging\licenses\components'
if (-not (Test-Path -LiteralPath $componentsSrc -PathType Container)) { Stop-Tool "Missing $componentsSrc" }
Copy-Item -LiteralPath $componentsSrc -Destination $licensesDir -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'gpu\VENDOR.txt') -Destination (Join-Path $licensesDir 'VENDOR.txt') -Force

$ffmpegLicSrc = Join-Path $ffmpegPrefix 'share\licenses\ffmpeg'
if (Test-Path -LiteralPath $ffmpegLicSrc) {
    $ffmpegLicDst = Join-Path $licensesDir 'ffmpeg'
    New-Item -ItemType Directory -Force -Path $ffmpegLicDst | Out-Null
    foreach ($lf in 'LICENSE.md', 'COPYING.LGPLv2.1', 'BUILD.txt') {
        $p = Join-Path $ffmpegLicSrc $lf
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination (Join-Path $ffmpegLicDst $lf) -Force }
    }
}

if ($pacman -and $msysPackages.Count -gt 0) {
    $pkgLines = New-Object System.Collections.Generic.List[string]
    $pkgLines.Add('MSYS2 CLANG64 runtime packages providing bundled DLLs:')
    $pkgLines.Add('')
    $msysLicRoot = Join-Path (Split-Path $tc.Bin -Parent) 'share\licenses'
    foreach ($pkg in ($msysPackages | Sort-Object)) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $ver = (& $pacman -Q $pkg 2>$null | Out-String).Trim()
            $url = (& $pacman -Qi $pkg 2>$null | Select-String '^URL\s*:\s*(.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } | Select-Object -First 1)
        } finally { $ErrorActionPreference = $prev }
        $pkgLines.Add("$ver  $url")
        $short = $pkg -replace '^mingw-w64-clang-x86_64-', ''
        $srcLic = Join-Path $msysLicRoot $short
        if (Test-Path -LiteralPath $srcLic -PathType Container) {
            $dstLic = Join-Path $licensesDir "msys2\$short"
            New-Item -ItemType Directory -Force -Path $dstLic | Out-Null
            Copy-Item -Path (Join-Path $srcLic '*') -Destination $dstLic -Recurse -Force
        }
    }
    [IO.File]::WriteAllText((Join-Path $licensesDir 'MSYS2-PACKAGES.txt'), ($pkgLines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding($false)))
}

Write-Step 'Verifying staged Windows release folder'
$problems = @(Test-ReleaseFolder $stageRoot $version $objdump)
if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Bad $_ }
    Stop-Tool "Staged release verification failed ($($problems.Count) problems)"
}
Write-Ok 'Staged release folder passed all checks'

Write-Step "Creating $zipPath"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in (Get-ChildItem -LiteralPath $stageRoot -Recurse -File | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, "$packageStem/$rel", [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}

Write-Step 'Reopening and verifying ZIP archive round-trip'
Test-ReleaseZip $zipPath $packageStem $version $objdump $stageRoot
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($shaPath, "$hash  $packageStem.zip`n", (New-Object Text.UTF8Encoding($false)))
$sizeMb = [Math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 2)
Write-Ok "Created $zipPath ($sizeMb MiB)"
Write-Ok "SHA-256: $hash"
