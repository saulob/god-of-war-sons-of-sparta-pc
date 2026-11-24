# Headless validation of True Ultrawide for CUSA57767 1.00 and 1.01: the focused suite on the production resolution code
# and both installed boot images, the render resolution suite, the static gate on the game code and scenes with its
# self-test, and mutated copies of the production sources that must fail. No window, game or network.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_ultrawide_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$cc = Join-Path $tc.Bin 'clang.exe'
$target = '--target=x86_64-w64-mingw32'
$rootText = $root.Replace('\', '/')
$launcher = Join-Path $root 'launcher'
$shim = Join-Path $root 'gpu\shim'
$trgInclude = Join-Path $root 'third_party\trg-launcher\include'

$trg = @()
foreach ($name in 'settings', 'platform') {
    $object = Join-Path $temp "trg_$name.o"
    & $clangxx $target '-std=c++17' '-O1' '-D_CRT_SECURE_NO_WARNINGS' '-I' $trgInclude '-c' (Join-Path $root "third_party\trg-launcher\src\$name.cpp") '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile the TRG $name" }
    $trg += $object
}

# Builds one suite from the production sources, or from the workspace copies a mutation replaced.
function Build-Suite([string]$Exe, [string]$Workspace = '', [hashtable]$Replace = @{}, [string]$Loader = '') {
    $ErrorActionPreference = 'Continue'
    $include = @()
    if ($Workspace) { $include += @('-I', $Workspace) }
    $include += @('-I', $launcher, '-I', $shim, '-I', $trgInclude, '-I', (Join-Path $root 'src'))
    $test = 'tests\ultrawide_tests.cpp'
    $sources = @((Join-Path $root $test))
    foreach ($name in 'resolution.cpp', 'game_profile.cpp') {
        $sources += $(if ($Replace.ContainsKey($name)) { $Replace[$name] } else { Join-Path $launcher $name })
    }
    $objects = @()
    $defines = @((Get-StringDefine 'SOS_SOURCE_DIR' $rootText))
    if ($Loader) { $defines += (Get-StringDefine 'SOS_LOADER_SOURCE' $Loader.Replace('\', '/')) }
    & $clangxx $target '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' @defines @include @sources @objects @trg `
        '-lshell32' '-lole32' '-lcomdlg32' '-lwinhttp' '-o' $Exe
}

$result = 0
try {
    $exe = Join-Path $temp 'test_ultrawide.exe'
    Build-Suite $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile ultrawide_tests.cpp' }
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test_render_resolution.ps1')
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $python = Get-Python
    if ($python) {
        foreach ($arguments in @(@(), @('--self-test'))) {
            & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\ultrawide_check.py') @arguments
            if ($LASTEXITCODE -ne 0) { $result = 1 }
        }
    } else {
        Write-Host '  [fail] Python 3 not found'
        $result = 1
    }

    $resolution = Join-Path $launcher 'resolution.cpp'
    $gameProfile = Join-Path $launcher 'game_profile.cpp'
    $sites = Join-Path $launcher 'sos_resolution_sites.h'
    $present = Join-Path $shim 'dcp_present.h'
    $loader = Join-Path $root 'src\loader.c'
    $mutations = @(
        @{ File = $loader; Old = '    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; New = '    SosUltrawideConfig room_rule = {0};
    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; What = 'a confiner-derived room mask is restored in the loader' }
        @{ File = $loader; Old = '    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; New = '    (void)"dcpgpu_ultrawide_world_mask";
    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; What = 'the Vulkan host mask is restored' }
        @{ File = $loader; Old = '    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; New = '    (void)"mask UI: created render=Overlay";
    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; What = 'a Unity Canvas mask is restored' }
        @{ File = $loader; Old = '    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; New = '    (void)"sos_centred_render_target";
    const int prompt_style = sos_controller_prompts_from_file(settings_file);'; What = 'the centred 16:9 SetResolution fallback is restored' }
        @{ File = $resolution; Old = '{"21:9", "21:9 Ultrawide", 21, 9}'; New = '{"21:9", "21:9 Ultrawide", 16, 9}'; What = '21:9 width calculation stays 16:9' }
        @{ File = $resolution; Old = '{"32:9", "32:9 Super Ultrawide", 32, 9}'; New = '{"32:9", "32:9 Super Ultrawide", 16, 9}'; What = '32:9 width calculation stays 16:9' }
        @{ File = $resolution; Old = 'long clamped = std::clamp(wanted, minimum, kMaxWidth);'; New = 'long clamped = std::clamp(width, minimum, kMaxWidth);'; What = 'the Pro mode pass loses the vertical resolution (1440p 21:9 becomes 3520 x 1510)' }
        @{ File = $resolution; Old = '            std::memcpy(at + 6, &height, 4);'; New = '            const uint32_t taller = height + height / 3;
            std::memcpy(at + 6, &taller, 4);'; What = 'the vertical extent changes' }
        @{ File = $resolution; Old = '            std::memcpy(at + 1, &width, 4);
            std::memcpy(at + 6, &height, 4);'; New = '            (void)width;
            std::memcpy(at + 6, &height, 4);'; What = 'the horizontal world extent does not increase' }
        @{ File = $resolution; Old = 'return site.kind == SOS_SITE_WIDTH || site.kind == SOS_SITE_SET_RESOLUTION || height_sites;'; New = 'return site.kind == SOS_SITE_WIDTH || height_sites;'; What = 'the resolution request patch is removed' }
        @{ File = $resolution; Old = 'const uint32_t width = uint32_t(target->width), height = uint32_t(target->height);
            std::memcpy(at + 1'; New = 'const uint32_t width = uint32_t(target->width), height = uint32_t(target->width) * 9 / 16;
            std::memcpy(at + 1'; What = 'the camera gets the wrong aspect' }
        @{ File = $resolution; Old = '    if (requests != SOS_SET_RESOLUTION_SITE_COUNT)'; New = '    if (requests == 0)'; What = 'a partial set of requests is accepted' }
        @{ File = $resolution; Old = '        if (std::memcmp(image + site.offset, site.original, site.length))'; New = '        if (false && std::memcmp(image + site.offset, site.original, site.length))'; What = 'fingerprint validation removed' }
        @{ File = $resolution; Old = '    if (supported && settings_file && *settings_file) {'; New = '    (void)supported;
    if (settings_file && *settings_file) {'; What = 'an unknown build accepts the patch' }
        @{ File = $loader; Old = '            const char *splash = keep_splash_video_shape();'; New = '            const char *splash = "not applied";'; What = 'the boot splash video loses its 16:9 shape' }
        @{ File = $resolution; Old = '    return long(target->width) * 9 > long(target->height) * 16 + 16;'; New = '    return long(target->width) * 9 > long(target->height) * 32;'; What = 'a 21:9 target is not treated as ultrawide' }
        @{ File = $resolution; Old = '    return bytes && file_length == sos_splash_patch.file_length &&'; New = '    return bytes != nullptr || file_length == sos_splash_patch.file_length ||'; What = 'the splash scene check is removed' }
        @{ File = $sites; Old = '0x00, 0x00, 0xf0, 0x44, 0x00, 0x00, 0x87, 0x44}'; New = '0x00, 0x00, 0x20, 0x45, 0x00, 0x00, 0x87, 0x44}'; What = 'the splash video is stretched to 2560 x 1080' }
        @{ File = $gameProfile; Old = 'kResolutionSites_1_01[] = {SOS_UNITY_RESOLUTION_SITES, SOS_SET_RESOLUTION_SITES_1_01};'; New = 'kResolutionSites_1_01[] = {SOS_UNITY_RESOLUTION_SITES};'; What = 'CUSA57767 1.01 loses its resolution requests' }
        @{ File = $gameProfile; Old = 'kResolutionSites_1_00[] = {SOS_UNITY_RESOLUTION_SITES, SOS_SET_RESOLUTION_SITES_1_00};'; New = 'kResolutionSites_1_00[] = {SOS_UNITY_RESOLUTION_SITES, SOS_SET_RESOLUTION_SITES_1_01};'; What = 'CUSA57767 1.00 uses the 1.01 sites' }
        @{ File = $sites; Old = 'SOS_SET_RESOLUTION_SITE(0x50f541cu)'; New = 'SOS_SET_RESOLUTION_SITE(0x50f5420u)'; What = 'a 1.01 site moves' }
        @{ File = $present; Old = '    if (ratio > image_ratio) {'; New = '    image_ratio = 16.0f / 9.0f;
    if (ratio > image_ratio) {'; What = 'the presenter forces 16:9' }
        @{ File = $present; Old = '    return stretch ? Region{0, 0, surface.width, surface.height} : FitRegion(frame, surface);'; New = '    (void)frame;
    (void)stretch;
    return Region{0, 0, surface.width, surface.height};'; What = 'the presenter stretches the image' }
        @{ File = $loader; Old = '            runtime_redzone_add_volatile(site->offset + 1, 4);'; New = ''; What = 'the rewritten width is not volatile for the red-zone cache' }
        @{ File = $loader; Old = '    if (!read_ok || !sos_splash_patch_matches(bytes, length)) return "unexpected scene data";'; New = '    (void)read_ok;'; What = 'the loader overlays the splash without checking the scene' }
    )
    $detected = 0
    $index = 0
    foreach ($m in $mutations) {
        $index++
        $workspace = Join-Path $temp "mutation_$index"
        New-Item -ItemType Directory -Force -Path $workspace | Out-Null
        $copy = Copy-ToWorkspace $workspace $m.File ([IO.Path]::GetFileName($m.File))
        Edit-WorkspaceText $copy $m.Old $m.New
        $replace = @{}
        $loaderCopy = ''
        switch ([IO.Path]::GetFileName($m.File)) {
            'resolution.cpp' { $replace['resolution.cpp'] = $copy }
            'game_profile.cpp' { $replace['game_profile.cpp'] = $copy }
            'loader.c' { $loaderCopy = $copy }
            'sos_resolution_sites.h' {
                foreach ($name in 'resolution.cpp', 'game_profile.cpp') { $replace[$name] = Copy-ToWorkspace $workspace (Join-Path $launcher $name) $name }
            }
        }
        $mexe = Join-Path $workspace 'test_ultrawide.exe'
        if (Test-MutationDetected $m.What { Build-Suite $mexe $workspace $replace $loaderCopy *> $null } $mexe) { $detected++ }
    }
    Write-Host "  [ultrawide] mutations: $($mutations.Count), detected: $detected"
    if ($detected -ne $mutations.Count) { $result = 1 }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
