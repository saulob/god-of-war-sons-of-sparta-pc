# Headless test of the Xbox prompt icons on the game's own files (CUSA57767 1.00 and 1.01, when installed): resources.assets with
# resources.assets.resS and the Addressables icon bundle InputOverseer reads its icons from. Detecting the missing LASUP, LASDOWN,
# LASLEFT and LASRIGHT and the missing interaction icons (Button South/East/North Mask), preparing them from the game's own artwork
# into complete copies, validating the copies down to the glyph pixels, partial and unknown sources, the cache and its invalidation
# and the exact path redirection; then mutated copies of the production sources that must each fail. The game's files are only read.
# No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_icons_build_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$launcher = Join-Path $root 'launcher'

function Build-IconsTest([string]$Workspace, [string]$Exe, [string]$SourceDir) {
    $rootText = $SourceDir.Replace('\', '/')
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O2' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) '-I' $Workspace '-I' $launcher `
        (Join-Path $root 'tests\controller_icons_tests.cpp') (Join-Path $Workspace 'controller_icons.cpp') (Join-Path $Workspace 'icon_bundle.cpp') '-o' $Exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the controller icons test' }
}

# A workspace holds copies of the production units the test compiles; a mutation edits one of them.
function New-Workspace([string]$Name) {
    $folder = Join-Path $temp $Name
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    foreach ($file in @('controller_icons.cpp', 'controller_icons.h', 'icon_bundle.cpp', 'icon_bundle.h', 'icon_common.h')) { [void](Copy-ToWorkspace $folder (Join-Path $launcher $file) $file) }
    return $folder
}

$result = 0
try {
    $exe = Join-Path $temp 'test_controller_icons.exe'
    Build-IconsTest $launcher $exe $root
    & $exe
    $result = $LASTEXITCODE

    $hasGame = (Test-Path -LiteralPath (Join-Path $root 'game\prepared\CUSA57767-1.00\Media\StreamingAssets\aa\PS4\0342107720d621839faadc2c6c3deb69.bundle')) -or
        (Test-Path -LiteralPath (Join-Path $root 'game\prepared\v1.00\Media\StreamingAssets\aa\PS4\0342107720d621839faadc2c6c3deb69.bundle'))

    # Mutated copies: each must make the test fail. Data mutations run the bundle tests alone, which need the game's files.
    $mutations = @(
        @{ File = 'icon_common.h'; Old = '{"LASDOWN", "Xbox_LeftStick_Down"}, '; New = ''; What = 'LASDOWN is omitted'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = 'Raw(TextMeshProHash(sprite.icon->sprite))}});'; New = 'Raw(TextMeshProHash(sprite.icon->sprite) + (std::string(sprite.icon->token) == "LASDOWN" ? 1u : 0u))}});'; What = 'the TextMeshPro hash of LASDOWN is wrong'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = 'const auto glyphIndex = static_cast<std::uint32_t>(glyphs.items.size() + index);'; New = 'const auto glyphIndex = static_cast<std::uint32_t>(glyphs.items.size() + (index == 1 ? index + 1 : index));'; What = 'the glyph of LASDOWN points at the wrong atlas index'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = '    rect.Floats({x, y, size, size});

    const Field& subMeshes'; New = '    rect.Floats({x, y + (sprite.icon->token[3] == ''D'' ? 1.0f : 0.0f), size, size});

    const Field& subMeshes'; What = 'the sprite rectangle of LASDOWN is wrong'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = 'return pair.first == sprite.icon->token; });
        Recognise(source != playStationIcons.end()'; New = 'return pair.first == (std::string(sprite.icon->token) == "LASDOWN" ? "LASUP" : sprite.icon->token); });
        Recognise(source != playStationIcons.end()'; What = 'LASDOWN is copied from the left-stick-up artwork'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = '            CopyCell(plan.playStation, sprite.source.x, sprite.source.y, patchedXbox, sprite.cell.x, sprite.cell.y);'; New = '            if (std::string(sprite.icon->token) != "LASDOWN") CopyCell(plan.playStation, sprite.source.x, sprite.source.y, patchedXbox, sprite.cell.x, sprite.cell.y);'; What = 'the LASDOWN cell stays blank'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = '    for (const NewSprite& sprite : sprites) ids.push_back(sprite.id);
    std::sort'; New = '    for (const NewSprite& sprite : sprites) if (std::string(sprite.icon->token) != "LASDOWN") ids.push_back(sprite.id);
    std::sort'; What = 'LASDOWN is left out of the preload list'; Only = 'bundle' }
        @{ File = 'icon_common.h'; Old = '{"Button North Mask", kNorth}, '; New = ''; What = 'the interaction marker''s Button North Mask is not added (white-square fix removed)'; Only = 'bundle' }
        @{ File = 'icon_common.h'; Old = '{"Button South Mask", kSouth}'; New = '{"Button South Mask", kEast}'; What = 'the Xbox confirm mask uses the back sprite'; Only = 'bundle' }
        @{ File = 'icon_common.h'; Old = '{"Button North Mask Filled", -1}'; New = '{"Button North Mask Filled", kNorth}'; What = 'the marker fill uses a button glyph instead of its disc'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = 'constexpr std::uint64_t kSourceFingerprint = 0x190d4048fbb3675d;'; New = 'constexpr std::uint64_t kSourceFingerprint = 0x190d4048fbb3675e;'; What = 'the bundle fingerprint no longer matches the analysed data'; Only = 'bundle' }
        @{ File = 'icon_bundle.cpp'; Old = '    bool owned = false;  // the output is only ours'; New = '    bool owned = true;  // the output is only ours'; What = 'a refused output is removed even when it is the source'; Only = 'bundle' }
        @{ File = 'controller_icons.cpp'; Old = 'constexpr std::size_t kMonoBehaviourHeader = 28;'; New = 'constexpr std::size_t kMonoBehaviourHeader = 27;'; What = 'the icon fingerprint skips the wrong MonoBehaviour header size'; Only = '' }
        @{ File = 'controller_icons.cpp'; Old = '    return files.bundle.empty() || HasBundleIcons(files.bundle);'; New = '    return true;'; What = 'the bundle is not part of a valid prepared set'; Only = '' }
        @{ File = 'controller_icons.cpp'; Old = '    if (!files.bundle.empty()) {
        identity.bundleSize'; New = '    if (false) {
        identity.bundleSize'; What = 'the bundle is not part of the cache identity'; Only = '' }
        @{ File = 'controller_icons.cpp'; Old = '        Recognise(inspection.state == IconSetState::kMissing && bundleState == IconSetState::kMissing,'; New = '        Recognise(inspection.state == IconSetState::kMissing,'; What = 'a set with a complete bundle and the game''s own assets is patched further'; Only = '' }
    )
    $index = 0
    $detected = 0
    foreach ($mutation in $mutations) {
        $index++
        if (-not $hasGame) { Write-Host "  [skip] mutation needs the game's icon files: $($mutation.What)"; continue }
        $workspace = New-Workspace "mutation$index"
        Edit-WorkspaceText (Join-Path $workspace $mutation.File) $mutation.Old $mutation.New
        $mutated = Join-Path $workspace 'test.exe'
        if ($mutation.Only) { $env:SOS_ICONS_TEST_ONLY = $mutation.Only }
        try {
            $ok = Test-MutationDetected $mutation.What { Build-IconsTest $workspace $mutated $root; $global:LASTEXITCODE = 0 } $mutated
        } finally { Remove-Item Env:\SOS_ICONS_TEST_ONLY -ErrorAction SilentlyContinue }
        if ($ok) { $detected++ } else { $result = 1 }
    }

    # The redirection mutations edit a copy of the sources the test reads as text: the test is then run on that tree alone.
    $textMutations = @(
        @{ File = 'src\loader.c'; Old = '            runtime_file_alias("/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb69.bundle", controller_icons.bundle) == 0)'; New = '            1)'; What = 'the prepared bundle is not served through the guest alias' }
        @{ File = 'src\loader.c'; Old = 'controller_icons.streamed[0] && controller_icons.bundle[0] &&'; New = 'controller_icons.streamed[0] &&'; What = 'the aliases are registered without checking that the bundle was prepared' }
        @{ File = 'launcher\launcher.cpp'; Old = '            std::snprintf(icons->bundle, sizeof(icons->bundle), "%s", bundle.c_str());'; New = ''; What = 'the launcher does not hand the prepared bundle to the loader' }
    )
    foreach ($mutation in $textMutations) {
        $index++
        if (-not $hasGame) { Write-Host "  [skip] mutation needs the game's icon files: $($mutation.What)"; continue }
        $fake = Join-Path $temp "tree$index"
        foreach ($file in @('src\loader.c', 'src\runtime_file.c', 'launcher\launcher.cpp', 'launcher\controller_icons.cpp', 'launcher\icon_bundle.cpp')) { [void](Copy-ToWorkspace $fake (Join-Path $root $file) $file) }
        Edit-WorkspaceText (Join-Path $fake $mutation.File) $mutation.Old $mutation.New
        $mutated = Join-Path $temp "tree$index.exe"
        $ok = Test-MutationDetected $mutation.What { Build-IconsTest $launcher $mutated $fake; $global:LASTEXITCODE = 0 } $mutated
        if ($ok) { $detected++ } else { $result = 1 }
    }
    Write-Host "Controller icons mutations: $detected of $($mutations.Count + $textMutations.Count) rejected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
