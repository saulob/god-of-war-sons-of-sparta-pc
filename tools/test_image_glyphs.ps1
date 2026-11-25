# Headless test of the menu glyph Images without a controller follower: the sprite mapping, the handlers on fake Images (forced Xbox,
# forced PlayStation, Automatic with each controller kind, controller changes, late icons) and, when the boot images are installed, the
# hooks on the real code of CUSA57767 1.00 and 1.01 and the static proof from the game code, icon maps and scenes; then mutated copies
# of the production source that must each fail. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_image_glyphs_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$result = 0
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $launcher = Join-Path $root 'launcher'
    $src = Join-Path $root 'src'
    $rootText = $root.Replace('\', '/')

    function Build-Object([string]$Source, [string]$Object, [string[]]$Extra = @()) {
        & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $launcher '-I' $src @Extra '-c' $Source '-o' $Object
        if ($LASTEXITCODE -ne 0) { throw "could not compile $Source" }
    }

    $shared = @()
    foreach ($source in @((Join-Path $launcher 'controller_prompts.c'), (Join-Path $launcher 'prompt_dialog.c'), (Join-Path $src 'sos_skip_intro.c'),
            (Join-Path $src 'sos_exit_game.c'), (Join-Path $src 'sos_hermes_unlock.c'), (Join-Path $src 'sos_pit_unlock.c'),
            (Join-Path $src 'redzone_analysis.c'), (Join-Path $src 'isa_emulate.c'))) {
        $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
        Build-Object $source $object
        $shared += $object
    }
    $testObject = Join-Path $temp 'image_glyphs_tests.o'
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) `
        '-I' $launcher '-I' $src '-c' (Join-Path $root 'tests\image_glyphs_tests.cpp') '-o' $testObject
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the image glyph test' }

    function Build-Test([string]$Object, [string]$Exe) {
        & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' $testObject $Object @shared '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $Exe
        if ($LASTEXITCODE -ne 0) { throw 'could not link the image glyph test' }
    }

    $object = Join-Path $temp 'image_glyphs.o'
    Build-Object (Join-Path $launcher 'image_glyphs.c') $object
    $exe = Join-Path $temp 'image_glyphs_tests.exe'
    Build-Test $object $exe
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $python = Get-Python
    if ($python) {
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\image_glyphs.py')
        if ($LASTEXITCODE -ne 0) { $result = 1 }
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\image_glyphs.py') '--self-test'
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    } else {
        Write-Host '  [fail] Python 3 not found'
        $result = 1
    }

    $hasBoot = (Test-Path -LiteralPath (Join-Path $root 'build\image\CUSA57767-1.01\boot.bin')) -and (Test-Path -LiteralPath (Join-Path $root 'build\image\CUSA57767-1.00\boot.bin'))
    $mutations = @(
        @{ Old = 'SOS_GLYPH_LEFT_STICK_PRESS, SOS_GLYPH_LEFT_TRIGGER, SOS_GLYPH_RIGHT_TRIGGER, SOS_GLYPH_LEFT_SHOULDER, SOS_GLYPH_RIGHT_SHOULDER,'; New = 'SOS_GLYPH_LEFT_STICK_PRESS, SOS_GLYPH_LEFT_SHOULDER, SOS_GLYPH_RIGHT_SHOULDER, SOS_GLYPH_LEFT_SHOULDER, SOS_GLYPH_RIGHT_SHOULDER,'; What = 'L2 and R2 shown as LB and RB' }
        @{ Old = '    SHEET("KS_ButtonPrompt_", kOutlineSheet),'; New = '    SHEET("KS_ButtonPromptX_", kOutlineSheet),'; What = 'the pause tab sprites are not recognised (L2 and R2 stay)' }
        @{ Old = '    SHEET("KS_ButtonPrompt_", kOutlineSheet),'; New = '    SHEET("KS_ButtonPrompt_", kIconMapSheet),'; What = 'the outline sheet read with the icon map order' }
        @{ Old = '    SHEET("ButtonPromptSheetTemp(256,64)_", kFilledSheet),'; New = ''; What = 'the death screen Cross is not recognised' }
        @{ Old = 'if (at < length || (digits == 2 && number < 10) || number >= kSheets[i].count)'; New = 'if (number >= kSheets[i].count)'; What = 'malformed sprite names are accepted' }
        @{ Old = 'SOS_GLYPH_RIGHT_STICK_PRESS, SOS_GLYPH_LEFT_STICK_PRESS, SOS_GLYPH_TOUCHPAD,'; New = 'SOS_GLYPH_LEFT_STICK_PRESS, SOS_GLYPH_RIGHT_STICK_PRESS, SOS_GLYPH_TOUCHPAD,'; What = 'L3 and R3 swapped' }
        @{ Old = '        if (glyph != SOS_GLYPH_NONE) track(image, glyph);'; New = '        (void)track;'; What = 'enabled glyph Images are never changed' }
        @{ Old = '    if (!sos_prompt_uses_xbox(g_glyphs.style, sos_pad_kind_current())) return NULL;'; New = ''; What = 'PlayStation controllers get Xbox glyphs' }
        @{ Old = '    for (size_t i = 0; i < g_glyphs.count; ++i) show(&g_glyphs.images[i], icons);'; New = '    (void)icons;'; What = 'an open menu ignores a controller change' }
        @{ Old = '        show(tracked, NULL);
        forget(tracked);'; New = '        forget(tracked);'; What = 'the first glyph covers what AutoButtonSprite shows' }
        @{ Old = '        tracked->glyph = glyph;
        show(tracked, wanted_icons());'; New = '        show(tracked, wanted_icons());'; What = 'a changed sprite keeps the old glyph' }
        @{ Old = '    forget(tracked);
}'; New = '}'; What = 'a disabled Image stays tracked' }
        @{ Old = '        memcpy((uint8_t *)image + g_glyphs.site.override_sprite, &none, sizeof(none));'; New = '        (void)none;'; What = 'a disabled Image keeps the override' }
        @{ Old = '    if (!tracked->shown && pointer_at(tracked->image, g_glyphs.site.override_sprite)) return;'; New = ''; What = 'an override of the game is replaced' }
        @{ Old = 'if (!method || !runtime_ready()) return NULL;'; New = 'if (!method || (g_glyphs.count > 100000 && !runtime_ready())) return NULL;'; What = 'the subsystem lookup runs before GameRuntime exists' }
        @{ Old = '    if (!alive(sprite)) sprite = NULL;'; New = ''; What = 'a destroyed Xbox sprite is shown' }
        @{ Old = '    if (id && g_glyphs.sprites[slot].id == id) return g_glyphs.sprites[slot].glyph;'; New = ''; What = 'the sprite name is read on every call' }
        @{ Old = '    if (g_glyphs.count == TRACKED_IMAGES) return;'; New = '    if (g_glyphs.count == 4) return;'; What = 'too few glyphs are tracked' }
        @{ Old = '    if (wanted == g_glyphs.shown && !(g_glyphs.pending && ++g_glyphs.calls >= RETRY_CALLS)) return;'; New = '    if (wanted == g_glyphs.shown) return;'; What = 'late icons are never picked up' }
        @{ Old = '    return style == SOS_PROMPTS_XBOX ? SOS_GLYPH_HOOK_TEXT_ATLAS : 0;'; New = '    return SOS_GLYPH_HOOK_TEXT_ATLAS;'; What = 'PlayStation prompts install the hooks' }
        @{ Old = '        else if (fnv1a(FNV_OFFSET, image + hook->method, hook->length) != hook->hash) reason = "a hooked method is not the verified code";'; New = ''; What = 'a changed method is hooked'; Real = $true }
        @{ Old = '    if (!reason && services != site->services_hash) reason = "the code the handlers call is not the verified code";'; New = '    (void)services;'; What = 'the called functions are not verified'; Real = $true }
        @{ Old = '(uintptr_t)sos_image_glyphs_on_set_sprite, (uintptr_t)sos_prompt_hook_handler}'; New = '(uintptr_t)sos_image_glyphs_on_set_sprite, (uintptr_t)sos_image_glyphs_on_enable}'; What = 'the text atlas hook calls another handler'; Real = $true }
        @{ Old = '    method[5] = 0x90;'; New = '    method[5] = 0xcc;'; What = 'the sixth stolen byte is a trap'; Real = $true }
        @{ Old = '    put_rel32(at, at + 4, method + SOS_GLYPH_HOOK_STOLEN);'; New = '    put_rel32(at, at + 4, method);'; What = 'the stub returns into the hook'; Real = $true }
    )
    $index = 0
    $detected = 0
    foreach ($mutation in $mutations) {
        $index++
        $real = $mutation.ContainsKey('Real')
        if ($real -and -not $hasBoot) { Write-Host "  [skip] mutation needs the real boot image: $($mutation.What)"; continue }
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $workspace = Copy-ToWorkspace $folder (Join-Path $launcher 'image_glyphs.c') 'image_glyphs.c'
        Edit-WorkspaceText $workspace $mutation.Old $mutation.New
        $mutantObject = Join-Path $folder 'image_glyphs.o'
        $mutantExe = Join-Path $folder 'test.exe'
        if (-not $real) { $env:SOS_TEST_FAST = '1' }
        try {
            $ok = Test-MutationDetected $mutation.What { Build-Object $workspace $mutantObject @('-Wno-unused'); Build-Test $mutantObject $mutantExe; $global:LASTEXITCODE = 0 } $mutantExe
        } finally { Remove-Item Env:\SOS_TEST_FAST -ErrorAction SilentlyContinue }
        if ($ok) { $detected++ } else { $result = 1 }
    }
    Write-Host "Image glyph mutations: $detected of $($mutations.Count) rejected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
