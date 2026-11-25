# Headless checks for the two independent playable builds: CUSA57767 1.00 alone and the optional 1.01 update.
# The selection rules are checked without any game file; the cases that need a real executable use hard links to the extracted
# packages or the prepared folders that are on this machine, and a build without them is reported as NOT INSTALLED, never as a
# failure. No game, launcher or GUI is started.
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

$fixture = Join-Path $root 'build\test-supported-builds'
$dataRoot = if ($env:SOS_EVIDENCE_ROOT) { $env:SOS_EVIDENCE_ROOT } else { $root }

function Find-Source([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'eboot.bin')) { return $candidate }
    }
    return $null
}

$baseSource = Find-Source @((Join-Path $dataRoot 'game\extracted\v1.00'), (Join-Path $dataRoot 'game\prepared\CUSA57767-1.00'))
$updateSource = Find-Source @((Join-Path $dataRoot 'game\prepared\CUSA57767-1.01'), (Join-Path $dataRoot 'game\extracted\CUSA57767-1.01'))
$shared = 'eboot.bin', 'sce_discmap.plt', 'sce_sys/param.sfo', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
          'Media/Modules/PS4Util.prx', 'Media/Modules/Il2cppUserAssemblies.prx'

function Link-File([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force -Path (Split-Path $To -Parent) | Out-Null
    New-Item -ItemType HardLink -Path $To -Target $From | Out-Null
}

function Write-View([string]$Source, [string]$Destination, [string]$Version, [switch]$Update, [switch]$Data,
                    [switch]$NoScope) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($name in $shared) {
        Link-File (Join-Path $Source $name) (Join-Path $Destination $name)
    }
    if ($Update) { Link-File (Join-Path $Source 'sce_discmap_patch.plt') (Join-Path $Destination 'sce_discmap_patch.plt') }
    foreach ($name in $script:FullGameFiles) {
        $path = Join-Path $Destination $name
        if ($Data) {
            New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
            Set-Content -LiteralPath $path -NoNewline -Value 'data'
        }
        else {
            New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        }
    }
    $scope = if ($NoScope) { '' } else { ',"scope":"full"' }
    Set-Content -LiteralPath (Join-Path $Destination '.sos_view.json') -NoNewline -Value ('{"version":"' + $Version + '"' + $scope + '}')
}

function Copy-Tree([string]$From, [string]$To) {
    foreach ($file in Get-ChildItem -LiteralPath $From -Recurse -File) {
        Link-File $file.FullName (Join-Path $To $file.FullName.Substring($From.Length).TrimStart('\'))
    }
}

function Case-Root([string]$Name) {
    return Join-Path $fixture $Name
}

function Add-Build([string]$Case, [string]$Folder, [string]$View, [string]$Image) {
    $root = Case-Root $Case
    Copy-Tree $View (Join-Path $root "prepared\$Folder")
    Copy-Tree $Image (Join-Path $root "image\$Folder")
}

try {
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $fixture | Out-Null

    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $clang = Join-Path $tc.Bin 'clang++.exe'
    $selectionSource = Join-Path $fixture 'selection.cpp'
    @'
#include <cstdio>
#include <filesystem>
#include <string>
#include "game_files.h"
#include "game_profile.h"
namespace fs = std::filesystem;
static int Report(const sos::GameCheck& check) {
    std::printf("%d|%s\n", static_cast<int>(check.state), check.detail.c_str());
    return check.state == sos::GameFileState::kValid ? 0 : 2;
}
int main(int argc, char** argv) {
    if (argc < 3) return 64;
    const std::string mode = argv[1];
    if (mode == "tree") return Report(sos::CheckGameTree(fs::path(argv[2])));
    if (mode == "files") return Report(sos::CheckGameFiles({fs::path(argv[2]), fs::path(argv[3])}));
    const sos::GameRoots roots{fs::path(argv[2]), fs::path(argv[3])};
    if (mode == "prepared") {
        for (const sos::GameBuild* build : sos::PreparedBuilds(roots)) std::printf("%s\n", build->version);
        return 0;
    }
    if (mode == "pinned") {
        if (argc < 5) return 64;
        const sos::PinnedBuild pin = sos::PinnedBuildOf(roots, argv[4]);
        std::printf("%s|%s\n", pin.build ? pin.build->version : "none", pin.error.c_str());
        return pin.build ? 0 : 2;
    }
    if (mode == "active") {
        const sos::GameBuild* build = sos::LastPreparedBuild(roots);
        if (!build) { std::printf("none\n"); return 3; }
        const sos::GameCheck check = sos::CheckGameFiles(sos::BuildPaths(roots, *build));
        std::printf("%s|%s|%s\n", build->version, check.state == sos::GameFileState::kValid ? "valid" : "invalid",
                    check.detail.c_str());
        return check.state == sos::GameFileState::kValid ? 0 : 3;
    }
    return 64;
}
'@ | Set-Content -LiteralPath $selectionSource -Encoding ascii
    $selection = Join-Path $fixture 'selection.exe'
    & $clang --target=x86_64-w64-mingw32 -std=c++20 -O0 '-I' (Join-Path $root 'launcher') $selectionSource `
        (Join-Path $root 'launcher\game_files.cpp') (Join-Path $root 'launcher\game_profile.cpp') `
        (Join-Path $root 'launcher\crypto_win.cpp') '-lbcrypt' '-o' $selection
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the build-selection harness' }

    Write-Step 'Case 0: no game file is needed to refuse an unknown executable'
    $nothing = Case-Root 'case0'
    New-Item -ItemType Directory -Force -Path (Join-Path $nothing 'prepared'), (Join-Path $nothing 'image') | Out-Null
    $active = & $selection active (Join-Path $nothing 'prepared') (Join-Path $nothing 'image') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 3 -and $active.Trim() -eq 'none') "an empty installation has no active build ($($active.Trim()))"
    foreach ($folder in 'CUSA57767-1.00', 'CUSA57767-1.01') {
        $unknown = Join-Path $nothing "prepared\$folder"
        New-Item -ItemType Directory -Force -Path $unknown | Out-Null
        Set-Content -LiteralPath (Join-Path $unknown 'eboot.bin') -NoNewline -Value 'not the supported executable'
        Set-Content -LiteralPath (Join-Path $unknown '.sos_view.json') -NoNewline -Value '{"version":"1.00","scope":"full"}'
    }
    $prepared = & $selection prepared (Join-Path $nothing 'prepared') (Join-Path $nothing 'image') 2>&1 | Out-String
    Check ([string]::IsNullOrWhiteSpace($prepared)) "folders named after the supported builds but holding another executable are not builds ($($prepared.Trim()))"
    foreach ($folder in 'CUSA57767-1.00', 'CUSA57767-1.01') {
        $pinned = & $selection pinned (Join-Path $nothing 'prepared') (Join-Path $nothing 'image') $folder 2>&1 | Out-String
        Check ($LASTEXITCODE -ne 0 -and $pinned -match '^none\|') "$folder cannot be selected with another executable ($($pinned.Trim()))"
    }

    foreach ($pair in @(@('CUSA57767-1.00', $baseSource), @('CUSA57767-1.01', $updateSource))) {
        $state = if ($pair[1]) { 'EXTRACTED LAYERS' } else { 'NOT INSTALLED' }
        if ($pair[1] -and $pair[1] -like (Join-Path $dataRoot 'game\prepared\*')) { $state = 'AVAILABLE' }
        $detail = if ($pair[1]) { $pair[1].Substring($dataRoot.Length).TrimStart('\').Replace('\', '/') } else { 'no game files; the selection of this build was not run on the real executable' }
        Write-Host "EVIDENCE|content|$($pair[0])|$state|$detail"
    }

    if ($baseSource) {
    $builderSource = Join-Path $fixture 'builder.cpp'
    @'
#include <cstddef>
#include <cstdio>
extern "C" int sos_build_boot_image(const char*, const char*, char*, size_t);
int main(int argc, char** argv) {
    if (argc < 3) return 64;
    char error[2048] = {0};
    if (sos_build_boot_image(argv[1], argv[2], error, sizeof(error))) {
        std::fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    return 0;
}
'@ | Set-Content -LiteralPath $builderSource -Encoding ascii
    $builder = Join-Path $fixture 'builder.exe'
    & $clang --target=x86_64-w64-mingw32 -std=c++20 -O0 '-I' (Join-Path $root 'launcher') $builderSource `
        (Join-Path $root 'launcher\boot_image.cpp') (Join-Path $root 'launcher\game_files.cpp') `
        (Join-Path $root 'launcher\game_profile.cpp') (Join-Path $root 'launcher\crypto_win.cpp') `
        '-lbcrypt' '-o' $builder
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the image builder harness' }

    $base = Join-Path $fixture 'base'
    $update = Join-Path $fixture 'update'
    $partial = Join-Path $fixture 'partial'
    $lying = Join-Path $fixture 'lying'
    $baseImage = Join-Path $fixture 'image-base'
    $updateImage = Join-Path $fixture 'image-update'
    Write-View $baseSource $base '1.00' -Data
    & $builder $base $baseImage
    if ($LASTEXITCODE -ne 0) { throw 'could not build the 1.00 fixture image' }
    if ($updateSource) {
        Write-View $updateSource $update '1.01' -Update -Data
        Write-View $updateSource $partial '1.01'                 # the current partial view: no scope
        Write-View $updateSource $lying '1.01' -Data            # a marker that claims full without game data
        Remove-Item -LiteralPath (Join-Path $lying 'Media\globalgamemanagers.assets') -Force
        & $builder $update $updateImage
        if ($LASTEXITCODE -ne 0) { throw 'could not build the 1.01 fixture image' }
    }
    $baseReport = Get-Content -LiteralPath (Join-Path $baseImage 'image.json') -Raw | ConvertFrom-Json
    Check ($baseReport.app_ver -eq '01.00' -and
           $baseReport.eboot_sha256 -eq '19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31') `
        'the 1.00 image is built from the base eboot and param.sfo'
    Check ($baseReport.fs_loads_patched -eq 387) 'the 1.00 image rewrites the TCB loads'
    if ($updateSource) {
    $updateReport = Get-Content -LiteralPath (Join-Path $updateImage 'image.json') -Raw | ConvertFrom-Json
    Check ($updateReport.fs_loads_patched -eq 389) 'the 1.01 image also rewrites its two thread-pointer loads'
    $updateBoot = [IO.File]::ReadAllBytes((Join-Path $updateImage 'boot.bin'))
    $updateImageStart = $updateBoot.Length - [int64][BitConverter]::ToUInt64($updateBoot, 8)
    $threadLoad = [byte[]](0x65, 0x48, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00)
    foreach ($offset in 0x1a2e604, 0x1a2e674) {
        $site = $updateImageStart + [int64]$offset
        $same = $true
        for ($index = 0; $index -lt $threadLoad.Length; ++$index) {
            $same = $same -and $updateBoot[$site + $index] -eq $threadLoad[$index]
        }
        Check $same ('the 1.01 image rewrites image+0x{0:x} to the GS thread-pointer load' -f $offset)
    }
    }
    $baseContent = [IO.File]::ReadAllBytes((Join-Path $baseImage 'content.bin'))
    Check ($baseContent.Length -eq 28 -and [Text.Encoding]::ASCII.GetString($baseContent, 0, 8) -eq 'SOSCONT1' -and
           $baseContent[8] -eq 3) 'the 1.00 content profile is the full game SKU with the base parameters'

    Write-Step 'Case 1: only a complete 1.00 installation'
    Add-Build 'case1' 'CUSA57767-1.00' $base $baseImage
    $case1 = Case-Root 'case1'
    $active = & $selection active (Join-Path $case1 'prepared') (Join-Path $case1 'image') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0 -and $active -match '^1\.00\|valid') "1.00 is the active playable build ($($active.Trim()))"
    $files = & $selection files (Join-Path $case1 'prepared\CUSA57767-1.00') (Join-Path $case1 'image\CUSA57767-1.00') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0) 'the 1.00 Play gate is valid without any update'

    Write-Step 'Case 2: the 1.01 update is absent and optional'
    $prepared = & $selection prepared (Join-Path $case1 'prepared') (Join-Path $case1 'image') 2>&1 | Out-String
    Check ($prepared.Trim() -eq '1.00') "only 1.00 is prepared ($($prepared.Trim()))"
    Check (-not ($prepared -match '1\.01')) 'the missing update is not reported as installed'

    if ($updateSource) {
    Write-Step 'Case 3: base 1.00 and update 1.01 installed together'
    Add-Build 'case3' 'CUSA57767-1.00' $base $baseImage
    Add-Build 'case3' 'CUSA57767-1.01' $update $updateImage
    $case3 = Case-Root 'case3'
    $active = & $selection active (Join-Path $case3 'prepared') (Join-Path $case3 'image') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0 -and $active -match '^1\.01\|valid') "1.01 is preferred and playable ($($active.Trim()))"
    $prepared = & $selection prepared (Join-Path $case3 'prepared') (Join-Path $case3 'image') 2>&1 | Out-String
    Check (($prepared -split "`r?`n" | Where-Object { $_ }) -contains '1.00') 'the 1.00 installation remains prepared'
    $files = & $selection files (Join-Path $case3 'prepared\CUSA57767-1.00') (Join-Path $case3 'image\CUSA57767-1.00') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0) 'the 1.00 installation remains independently valid'
    $pinned = & $selection pinned (Join-Path $case3 'prepared') (Join-Path $case3 'image') 'CUSA57767-1.00' 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0 -and $pinned -match '^1\.00\|') "1.00 can be selected explicitly ($($pinned.Trim()))"

    Write-Step 'Case 4: an invalid 1.01 view does not block 1.00'
    Add-Build 'case4a' 'CUSA57767-1.00' $base $baseImage
    Copy-Tree $partial (Join-Path $fixture 'case4a\prepared\CUSA57767-1.01')
    $case4 = Case-Root 'case4a'
    $active = & $selection active (Join-Path $case4 'prepared') (Join-Path $case4 'image') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0 -and $active -match '^1\.00\|valid') "a scope-less partial 1.01 does not block 1.00 ($($active.Trim()))"
    Add-Build 'case4b' 'CUSA57767-1.00' $base $baseImage
    Copy-Tree $lying (Join-Path $fixture 'case4b\prepared\CUSA57767-1.01')
    Copy-Tree $updateImage (Join-Path $fixture 'case4b\image\CUSA57767-1.01')
    $case4 = Case-Root 'case4b'
    $active = & $selection active (Join-Path $case4 'prepared') (Join-Path $case4 'image') 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0 -and $active -match '^1\.00\|valid') "a data-less 1.01 does not block 1.00 ($($active.Trim()))"
    }

    Write-Step 'Case 5: a complete extracted 1.00 folder'
    $tree = & $selection tree $base 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0) "the extracted 1.00 folder validates as a supported build ($($tree.Trim()))"

    Write-Step 'Case 6: build.ps1 with only 1.00 installed'
    $installed = @(Get-PreparedBuilds (Join-Path $case1 'prepared') (Join-Path $case1 'image'))
    Check ($installed.Count -eq 1 -and $installed[0].Folder -eq 'CUSA57767-1.00') 'build discovery finds the 1.00 installation'
    Check (Test-FullPreparedView $installed[0].View) 'build discovery accepts the complete 1.00 view'
    Check (Test-ImageCurrent $installed[0].View $installed[0].Image @()) 'build discovery finds the 1.00 image current'
    $build = Get-Content -LiteralPath (Join-Path $root 'tools\build.ps1') -Raw
    foreach ($token in 'prepare_game', 'pkg_extract', '.pkg', '--scope', 'Decomp_Downloads', 'game\extracted',
                       'Invoke-Script', '-Packages', '-Version', '1.01') {
        Check (-not $build.Contains($token)) "build.ps1 has no '$token'"
    }
    }
}
finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) check(s) failed." -ForegroundColor Red
    exit 1
}
Write-Host 'All supported-build checks passed.' -ForegroundColor Green
exit 0
