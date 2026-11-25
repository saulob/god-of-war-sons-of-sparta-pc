# Headless test of the Settings confirmation dialog and campfire menu glyphs: the hook tables, the handlers on fake game objects (forced
# Xbox, forced PlayStation, Automatic with each controller kind, restoration, every missing object) and, when the boot images are
# installed, the hooks on the real code of CUSA57767 1.00 and 1.01 and the static proof of the dialogs and menus they cover; then
# mutated copies of the production sources that must each fail. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_prompt_dialog_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$launcher = Join-Path $root 'launcher'
$src = Join-Path $root 'src'
$rootText = $root.Replace('\', '/')

function Build-Objects([string]$Folder, [hashtable]$Replace) {
    $objects = @()
    foreach ($unit in @(@{ Source = (Join-Path $launcher 'controller_prompts.c') }, @{ Source = (Join-Path $launcher 'prompt_dialog.c') }, @{ Source = (Join-Path $src 'sos_skip_intro.c') },
            @{ Source = (Join-Path $src 'redzone_analysis.c') }, @{ Source = (Join-Path $src 'isa_emulate.c') })) {
        $source = $unit.Source
        $name = [IO.Path]::GetFileName($source)
        if ($Replace.ContainsKey($name)) { $source = $Replace[$name] }
        $object = Join-Path $Folder ([IO.Path]::GetFileNameWithoutExtension($source) + '_' + [IO.Path]::GetFileNameWithoutExtension($name) + '.o')
        & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $launcher '-I' $src '-c' $source '-o' $object
        if ($LASTEXITCODE -ne 0) { throw "could not compile $source" }
        $objects += $object
    }
    return $objects
}

function Build-Test([string]$Folder, [string[]]$Objects, [string]$Exe) {
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) '-I' $launcher '-I' $src `
        (Join-Path $root 'tests\prompt_dialog_tests.cpp') @Objects '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $Exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the prompt dialog test' }
}

$result = 0
try {
    $exe = Join-Path $temp 'test_prompt_dialog.exe'
    Build-Test $temp (Build-Objects $temp @{}) $exe
    & $exe
    $result = $LASTEXITCODE

    $python = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($python) {
        & $python.Source -I (Join-Path $root 'scripts\prompt_dialog.py')
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    }
    else { Write-Host '  [skip] no Python: the dialog hook and the dialogs it covers were not read from the game code' }

    # Mutated copies of the production sources: each must make the test fail. Handler mutations run on the fake objects alone; the hook
    # mutations need the real boot images (and are skipped with them).
    $dialog = [IO.File]::ReadAllText((Join-Path $launcher 'prompt_dialog.c')).Replace("`r`n", "`n")
    $prompts = [IO.File]::ReadAllText((Join-Path $launcher 'controller_prompts.c')).Replace("`r`n", "`n")
    $hasBoot = [bool]((Find-BuildBootImage '1.00') -or (Find-BuildBootImage '1.01'))
    $mutations = @(
        @{ File = 'prompt_dialog.c'; Old = '        if (alive(image)) set_glyph(image, confirm);'; New = '        (void)image;'; What = 'the campfire menu prompt no longer receives the Xbox confirm glyph'; Real = $false }
        @{ File = 'prompt_dialog.c'; Old = '    void *const confirm = xbox ? pointer_at(icons, g_dialog.site.xbox_confirm) : NULL;
    if (xbox && !alive(confirm)) return;
    for (uint32_t'; New = '    void *const confirm = xbox ? pointer_at(icons, g_dialog.site.xbox_back) : NULL;
    if (xbox && !alive(confirm)) return;
    for (uint32_t'; What = 'the menu confirm prompt shows the Xbox back sprite'; Real = $false }
        @{ File = 'prompt_dialog.c'; Old = '    for (uint32_t i = 0; i < spec->image_count && i < SOS_PROMPT_MENU_IMAGES; ++i) {'; New = '    for (uint32_t i = 0; i < 1; ++i) {'; What = 'only the first menu prompt Image is set'; Real = $false }
        @{ File = 'prompt_dialog.c'; Old = '    } else if (pointer_at(image, g_dialog.site.override_sprite)) {
        g_dialog.game.set_override_sprite(image, NULL, NULL);
    }'; New = '    }'; What = 'the controller-type restoration (clearing the Xbox override) is removed'; Real = $false }
        @{ File = 'prompt_dialog.c'; Old = '    return sos_prompt_uses_xbox(g_dialog.style, sos_pad_kind_current()) && int_at(overseer, g_dialog.site.input_device) == CONTROL_DEVICE_GAMEPAD;'; New = '    (void)overseer;
    return sos_prompt_uses_xbox(g_dialog.style, sos_pad_kind_current());'; What = 'Xbox glyphs stay on while the keyboard is the input'; Real = $false }
        @{ File = 'prompt_dialog.c'; Old = 'SOS_PROMPT_DIALOG_SYSV void sos_prompt_menu_handler_pit(void *menu) { menu_glyphs(menu, &g_dialog.site.menus[1]); }'; New = 'SOS_PROMPT_DIALOG_SYSV void sos_prompt_menu_handler_pit(void *menu) { (void)menu; }'; What = 'the pit player menu handler does nothing'; Real = $false }
        @{ File = 'controller_prompts.c'; Old = '    return style == SOS_PROMPTS_XBOX || (style == SOS_PROMPTS_AUTOMATIC && pad_kind == SOS_PAD_XBOX);'; New = '    return (style == SOS_PROMPTS_AUTOMATIC && pad_kind == SOS_PAD_XBOX);'; What = 'forced Xbox mode is ignored'; Real = $false }
        @{ File = 'controller_prompts.c'; Old = '    return style == SOS_PROMPTS_XBOX || (style == SOS_PROMPTS_AUTOMATIC && pad_kind == SOS_PAD_XBOX);'; New = '    return style == SOS_PROMPTS_XBOX || pad_kind == SOS_PAD_XBOX;'; What = 'forced PlayStation mode is ignored'; Real = $false }
        @{ File = 'controller_prompts.c'; Old = '    return style == SOS_PROMPTS_XBOX || (style == SOS_PROMPTS_AUTOMATIC && pad_kind == SOS_PAD_XBOX);'; New = '    return style == SOS_PROMPTS_XBOX || (style == SOS_PROMPTS_AUTOMATIC && pad_kind == SOS_PAD_NONE);'; What = 'Automatic Xbox detection is disabled'; Real = $false }
        @{ File = 'prompt_dialog.c'; Old = '    const uint64_t handler = (uint64_t)(uintptr_t)(index == 0 ? sos_prompt_menu_handler_save : sos_prompt_menu_handler_pit);'; New = '    const uint64_t handler = (uint64_t)(uintptr_t)(index == 0 ? sos_prompt_menu_handler_pit : sos_prompt_menu_handler_save);'; What = 'the campfire stub calls the pit player handler'; Real = $true }
        @{ File = 'prompt_dialog.c'; Old = '        else if (fnv1a(UINT64_C(0xcbf29ce484222325), image + menu->method, menu->length) != menu->hash) reason = "the menu method is not the verified code";'; New = ''; What = 'the menu method hash is no longer verified'; Real = $true }
        @{ File = 'prompt_dialog.c'; Old = '            if (!reason && services != site->services) reason = "the code the menu handler calls is not the verified code";'; New = '            (void)services;'; What = 'the code the menu handler calls is no longer verified'; Real = $true }
        @{ File = 'prompt_dialog.c'; Old = '    if (!sos_prompt_menu_verify(image, size, build_key, index, &why)) {
        snprintf(message, message_size, "not applied: %s in %s", why, build_key ? build_key : "this build");
        return 0;
    }
    const SosPromptMenu *menu'; New = '    (void)why;
    (void)size;
    const SosPromptMenu *menu'; What = 'the menu hook installs without verifying the code first'; Real = $true }
        @{ File = 'prompt_dialog.c'; Old = '    *at++ = 0xe9;
    memcpy(at, &back, sizeof(back));
    const int32_t into = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(method + 5));
    method[0] = 0xe9;
    memcpy(method + 1, &into, sizeof(into));
    memset(method + 5, 0x90, SOS_PROMPT_MENU_STOLEN - 5);'; New = '    *at++ = 0xe9;
    memcpy(at, &back, sizeof(back));
    const int32_t into = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(method + 5));
    method[0] = 0xe9;
    memcpy(method + 1, &into, sizeof(into));'; What = 'the menu hook leaves the sixth stolen byte unfilled'; Real = $true }
    )
    $index = 0
    $detected = 0
    foreach ($mutation in $mutations) {
        $index++
        if ($mutation.Real -and -not $hasBoot) { Write-Host "  [data] DATA UNAVAILABLE: mutation needs a real boot image: $($mutation.What)"; continue }
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $workspace = Copy-ToWorkspace $folder (Join-Path $launcher $mutation.File) $mutation.File
        Edit-WorkspaceText $workspace $mutation.Old $mutation.New
        $mutated = Join-Path $folder "test.exe"
        $compile = { Build-Test $folder (Build-Objects $folder @{ ($mutation.File) = $workspace }) $mutated }
        if (-not $mutation.Real) { $env:SOS_TEST_FAST = '1' }
        try {
            $ok = Test-MutationDetected $mutation.What { & $compile; $global:LASTEXITCODE = 0 } $mutated
        } finally { Remove-Item Env:\SOS_TEST_FAST -ErrorAction SilentlyContinue }
        if ($ok) { $detected++ } else { $result = 1 }
    }
    Write-Host "Prompt dialog and menu mutations: $detected of $($mutations.Count) rejected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
