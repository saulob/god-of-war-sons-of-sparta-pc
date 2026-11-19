# Headless checks for the build/install split: tools\build.ps1 only builds and refreshes the boot image of an installed
# game; the launcher owns package extraction and the prepared installation. No game, launcher or GUI is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot 'build_game.ps1')

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$Ok, [string]$Name) {
    if ($Ok) { Write-Host "  [ok]   $Name" -ForegroundColor Green }
    else { Write-Host "  [fail] $Name" -ForegroundColor Red; $failures.Add($Name) }
}

$tempParent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $tempParent)) { $tempParent = [IO.Path]::GetTempPath() }
$temp = Join-Path $tempParent ('sos_build_install_' + [guid]::NewGuid().ToString('N'))
$fixture = Join-Path $root 'build\test-install'
$prepared = Join-Path $temp 'prepared'
$images = Join-Path $temp 'image'

function New-View([string]$Folder, [string]$Scope, [switch]$Data, [switch]$Eboot) {
    $view = Join-Path $prepared $Folder
    New-Item -ItemType Directory -Force -Path $view | Out-Null
    Set-Content -LiteralPath (Join-Path $view '.sos_view.json') -NoNewline -Value ('{"version":"1.01","scope":"' + $Scope + '"}')
    if ($Eboot) { Set-Content -LiteralPath (Join-Path $view 'eboot.bin') -NoNewline -Value 'eboot' }
    if ($Data) {
        foreach ($name in $script:FullGameFiles) {
            $path = Join-Path $view $name
            New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
            Set-Content -LiteralPath $path -NoNewline -Value 'data'
        }
    }
    return $view
}

function New-Image([string]$Folder, [datetime]$When, [switch]$BadMagic) {
    $image = Join-Path $images $Folder
    New-Item -ItemType Directory -Force -Path $image | Out-Null
    [IO.File]::WriteAllText((Join-Path $image 'boot.bin'), $(if ($BadMagic) { 'BADBOOT1' } else { 'SOSBOOT1' }))
    $content = New-Object byte[] 28
    [Text.Encoding]::ASCII.GetBytes($(if ($BadMagic) { 'BADS' } else { 'SOSCONT1' })).CopyTo($content, 0)
    [IO.File]::WriteAllBytes((Join-Path $image 'content.bin'), $content)
    [IO.File]::WriteAllText((Join-Path $image 'image.json'), '{}')
    foreach ($name in $script:ImageFiles) { (Get-Item -LiteralPath (Join-Path $image $name)).LastWriteTimeUtc = $When }
}

function Link-File([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force -Path (Split-Path $To -Parent) | Out-Null
    New-Item -ItemType HardLink -Path $To -Target $From | Out-Null
}

try {
    New-Item -ItemType Directory -Force -Path $prepared, $images | Out-Null

    Write-Step 'Case 1: no prepared installation'
    Check ((@(Get-PreparedBuilds $prepared $images).Count) -eq 0) 'no installed build is discovered'
    $build = Get-Content -LiteralPath (Join-Path $root 'tools\build.ps1') -Raw
    foreach ($token in 'prepare_game', 'pkg_extract', '.pkg', '--scope', 'Decomp_Downloads', 'game\extracted',
                       'Invoke-Script', '-Packages', '-Version') {
        Check (-not $build.Contains($token)) "build.ps1 has no '$token'"
    }
    Check ($build.Contains('--prepare-image')) 'build.ps1 refreshes images through the loader'
    Check (-not $build.Contains('prepare_image.py')) 'build.ps1 does not run the Python image builder'

    Write-Step 'Case 2: core or incomplete installation'
    $coreView = New-View 'CUSA57767-1.01' 'core' -Data -Eboot
    Check (-not (Test-FullPreparedView $coreView)) 'a core marker is not a full view'
    Check ((@(Get-PreparedBuilds $prepared $images).Count) -eq 1) 'the core view is discovered, not deleted'
    Remove-Item -LiteralPath $coreView -Recurse -Force
    $partialView = New-View 'CUSA57767-1.01' 'full' -Eboot
    Check (-not (Test-FullPreparedView $partialView)) 'a full marker without game data is not a full view'
    Remove-Item -LiteralPath $partialView -Recurse -Force

    Write-Step 'Case 3: complete installation with a current image'
    $fullView = New-View 'CUSA57767-1.00' 'full' -Data -Eboot
    $markerTime = (Get-Item -LiteralPath (Join-Path $fullView '.sos_view.json')).LastWriteTimeUtc
    New-Image 'CUSA57767-1.00' $markerTime.AddMinutes(1)
    Check (Test-FullPreparedView $fullView) 'the full view passes the required-data check'
    Check (Test-ImageCurrent $fullView (Join-Path $images 'CUSA57767-1.00') @()) 'nothing to do when view and image are current'

    Write-Step 'Case 4: complete installation with a stale image'
    foreach ($name in $script:ImageFiles) {
        (Get-Item -LiteralPath (Join-Path $images 'CUSA57767-1.00' $name)).LastWriteTimeUtc = $markerTime.AddMinutes(-1)
    }
    Check (-not (Test-ImageCurrent $fullView (Join-Path $images 'CUSA57767-1.00') @())) 'a stale image needs only --prepare-image'
    New-Image 'CUSA57767-1.00' $markerTime.AddMinutes(1) -BadMagic
    Check (-not (Test-ImageCurrent $fullView (Join-Path $images 'CUSA57767-1.00') @())) 'an image of another format is rebuilt'

    Write-Step 'Cases 5 and 6: the launcher owns installation'
    $installer = Get-Content -LiteralPath (Join-Path $root 'launcher\game_install.cpp') -Raw
    Check ($installer -match 'package\.Extract\(layout\.app0_stage') 'the base install unpacks the whole package into app0'
    Check (([regex]::Matches($installer, 'WriteMarker\([\s\S]*?"full"\)')).Count -ge 2) 'installs record a full-scope marker'
    Check ($installer -match 'StageUpdate\(base\.app0, update_dir, layout\.app0_stage\)') 'an update starts from the installed base'
    Check ($installer -match 'OverlayUpdate') 'an update lays its files over the base'
    Check ($installer -match 'BuildBootImage\(layout\.app0_stage, layout\.image_stage\)') 'the image is built from the staged tree'
    Check ($installer -match 'CheckGameFiles\(\{layout\.app0_stage, layout\.image_stage\}') 'the staged installation is validated'
    $overlay = [regex]::Match($installer, 'InstallResult OverlayUpdate[\s\S]*?\r?\n\}\r?\n').Value
    Check ($overlay -match 'fs::rename\(from, to') 'update files replace the staged copies'
    Check ($overlay -notmatch 'remove_all\(staged') 'unchanged base files stay in the staged tree'

    $profile = Get-Content -LiteralPath (Join-Path $root 'launcher\game_profile.cpp') -Raw
    foreach ($name in $script:FullGameFiles) {
        $quoted = '"' + $name + '"'
        Check (([regex]::Matches($profile, [regex]::Escape($quoted))).Count -ge 2) "both builds require $name"
    }
    $pythonProfile = Get-Content -LiteralPath (Join-Path $root 'scripts\game_profiles.py') -Raw
    foreach ($name in $script:FullGameFiles) {
        Check (([regex]::Matches($pythonProfile, [regex]::Escape("'$name'"))).Count -ge 2) "the manual profile requires $name"
    }

    Write-Step 'Launcher readiness on real files'
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang++.exe'
    $harnessSource = Join-Path $temp 'game_tree_check.cpp'
    @'
#include <cstdio>
#include <filesystem>
#include "game_files.h"
int main(int argc, char** argv) {
    if (argc < 3) return 64;
    const std::filesystem::path app0 = argv[2];
    const std::filesystem::path image = argc > 3 ? argv[3] : std::filesystem::path();
    const sos::GameCheck check = std::string(argv[1]) == "files" ? sos::CheckGameFiles({app0, image})
                                                                : sos::CheckGameTree(app0);
    std::printf("%d|%s\n", static_cast<int>(check.state), check.detail.c_str());
    return check.state == sos::GameFileState::kValid ? 0 : 2;
}
'@ | Set-Content -LiteralPath $harnessSource -Encoding ascii
    $harness = Join-Path $temp 'game_tree_check.exe'
    & $clang --target=x86_64-w64-mingw32 -std=c++23 -O0 '-I' (Join-Path $root 'launcher') $harnessSource `
        (Join-Path $root 'launcher\game_files.cpp') (Join-Path $root 'launcher\game_profile.cpp') `
        (Join-Path $root 'launcher\crypto_win.cpp') '-lbcrypt' '-o' $harness
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the launcher readiness harness' }

    $source = Join-Path $root 'game\prepared\CUSA57767-1.01'
    if (Test-Path -LiteralPath (Join-Path $source 'eboot.bin')) {
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
        $shared = 'eboot.bin', 'sce_discmap.plt', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
                  'Media/Modules/PS4Util.prx', 'Media/Modules/Il2cppUserAssemblies.prx', 'sce_sys/param.sfo'
        foreach ($name in $shared + @('sce_discmap_patch.plt')) {
            Link-File (Join-Path $source $name) (Join-Path $fixture "partial\$name")
        }
        Set-Content -LiteralPath (Join-Path $fixture 'partial\.sos_view.json') -NoNewline -Value '{"version":"1.01","scope":"full"}'
        $partialOutput = & $harness tree (Join-Path $fixture 'partial') 2>&1 | Out-String
        Check ($LASTEXITCODE -eq 2 -and $partialOutput -match 'missing Media/globalgamemanagers\.assets') 'the launcher rejects an installation without game data'

        Copy-Item -LiteralPath (Join-Path $fixture 'partial') -Destination (Join-Path $fixture 'full') -Recurse
        foreach ($name in $script:FullGameFiles) {
            $path = Join-Path $fixture "full\$name"
            New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
            Set-Content -LiteralPath $path -NoNewline -Value 'data'
        }
        $fullOutput = & $harness tree (Join-Path $fixture 'full') 2>&1 | Out-String
        Check ($LASTEXITCODE -eq 0) "a complete tree passes the launcher check ($($fullOutput.Trim()))"

        $imageSource = Join-Path $root 'build\image\CUSA57767-1.01'
        if (-not (Test-Path -LiteralPath (Join-Path $imageSource 'image.json'))) {
            $imageSource = Join-Path $root 'build\image\v1.01'
        }
        if (Test-Path -LiteralPath (Join-Path $imageSource 'image.json')) {
            foreach ($name in $script:ImageFiles) {
                Link-File (Join-Path $imageSource $name) (Join-Path $fixture "image\$name")
            }
            $fullFiles = & $harness files (Join-Path $fixture 'full') (Join-Path $fixture 'image') 2>&1 | Out-String
            Check ($LASTEXITCODE -eq 0) "the Play gate accepts a complete installation ($($fullFiles.Trim()))"
            $partialFiles = & $harness files (Join-Path $fixture 'partial') (Join-Path $fixture 'image') 2>&1 | Out-String
            Check ($LASTEXITCODE -eq 2) 'the Play gate rejects the partial installation'
        }
    }
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) check(s) failed." -ForegroundColor Red
    exit 1
}
Write-Host 'All build/install checks passed.' -ForegroundColor Green
exit 0
