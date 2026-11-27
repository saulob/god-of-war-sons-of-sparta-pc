# Headless validation of the cheats on CUSA57767 1.00 and 1.01: the model (defaults Off, saved values, F10 session
# overrides), the rules of every hook, the F10 menu model, the detour engine and the real handlers executed on synthetic
# code that holds the game's own site instructions, the item actions, the site tables on the real boot images (every
# site in and out byte for byte), the generated table against the boot images, source contracts of the F10 menu, the
# loader, the launcher page and the saved settings, and mutated copies that must fail. No game, launcher or window starts.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$parent = (Resolve-Path -LiteralPath $parent).Path
$temp = Join-Path $parent ('sos_cheats_' + [guid]::NewGuid().ToString('N').Substring(0, 12))

# Removes only the folder this run made: never an empty path, the repository, an ancestor of it, .git or .claude.
function Remove-RunFolder([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -in @('.', '..')) { return }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $repo = [IO.Path]::GetFullPath($root).TrimEnd('\')
    $leaf = Split-Path $full -Leaf
    $inside = $full.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
    $related = $full.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase) -or $repo.StartsWith($full, [StringComparison]::OrdinalIgnoreCase)
    if (-not $inside -or $related -or -not $leaf.StartsWith('sos_cheats_') -or $leaf -in @('.git', '.claude')) {
        throw "refusing to remove '$Path'"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}

New-Item -ItemType Directory -Path $temp | Out-Null
$sandbox = Join-Path $temp 'sos_cheats_data'
New-Item -ItemType Directory -Path $sandbox | Out-Null

$result = 0
function Report([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name"; $script:result = 1 }
}

function Read-Source([string]$Relative) { [IO.File]::ReadAllText((Join-Path $root $Relative)).Replace("`r`n", "`n") }

# Each failed contract, as text; empty when the sources hold every rule.
function Get-ContractFailures([hashtable]$Text) {
    $failures = New-Object System.Collections.Generic.List[string]
    $overlay = $Text['overlay']
    if ($overlay -notmatch 'bool CapturesInput\(\) \{ return cheat_open; \}') { $failures.Add('the open F10 menu (and its release wait) holds the game input neutral') }
    if ($overlay -notmatch 'menu_key\) \{\s*if \(down\) \{\s*PressCheatMenuKey\(event\.key\.repeat\);\s*\}\s*return true;') { $failures.Add('F10 is consumed by the overlay, never passed to the game') }
    if ($overlay -notmatch 'if \(!cheat_open \|\| event\.key\.scancode == DcpSettings::Get\(\)\.window_mode_key\) \{\s*return false;') { $failures.Add('F11 still reaches the window while the menu is open') }
    if ($overlay -notmatch 'event\.key\.key == SDLK_ESCAPE && down && !event\.key\.repeat\) \{\s*menu_close\.Request\(\);') { $failures.Add('Escape closes the menu') }
    if ($overlay -notmatch 'menu_close\.Step\(menu_pressed\.any\(\)') { $failures.Add('closing waits until the keys pressed in the menu are released') }
    if ($overlay -notmatch 'runtime_cheats_frame\(\);\s*return Visible\(\);') { $failures.Add('an item action no player takes is given up on every presented frame') }
    if ($overlay -match 'ActivateCheatRow[\s\S]*?(get_game_object|is_player|wallet_add)') { $failures.Add('the window thread calls no game function') }
    $pad = $Text['pad']
    if ($pad -notmatch 'if \(dcpgpu_overlay_captures_input\(\)\) return;' -or $pad -notmatch 's->hold_after_capture=1') { $failures.Add('the pad reads neutral input while captured and hides buttons held after it') }
    $loader = $Text['loader']
    $install = $loader.IndexOf('runtime_cheats_install(')
    $protect = $loader.IndexOf('protect(image, round_page(size), 0);')
    $start = $loader.IndexOf('runtime_cheats_start(settings_file);')
    $entry = $loader.IndexOf('sos_startup_phase("guest entry");')
    if ($install -lt 0 -or $protect -lt 0 -or $install -gt $protect) { $failures.Add('the loader verifies the cheat sites and builds their stubs before the image is made executable') }
    if ($start -lt 0 -or $entry -lt 0 -or $start -gt $entry -or $start -lt $protect) { $failures.Add('the saved cheats apply after the image is protected and before the game code runs') }
    $launcher = $Text['launcher']
    $page = [regex]::Match($launcher, 'void PageCheats\(trg::Ui& ui\) \{[\s\S]*?\n    \}').Value
    $order = @('ui.Heading("PLAYER")', 'Toggle::GodMode', 'Toggle::InfiniteSpirit', 'Toggle::InfiniteMagic', 'ui.Heading("COMBAT")', 'Multiplier::Damage',
               'ui.Heading("MOVEMENT")', 'Multiplier::MovementSpeed', 'Multiplier::JumpHeight', 'ImGui::EndDisabled()', 'ui.Heading("ITEMS")', 'kActionsHelp')
    $last = -1
    foreach ($token in $order) {
        $at = $page.IndexOf($token)
        if ($at -le $last) { $failures.Add("the Cheats page lists $token in its place"); break }
        $last = $at
    }
    if ($page -match 'ui\.(Toggle|Combo)\(|Blood Orbs|Upgrade Materials') { $failures.Add('the item actions are not saved launcher settings') }
    if ($Text['options'] -notmatch 'PageId::kGameplay, PageId::kCheats,') { $failures.Add('the Cheats page follows Gameplay in the sidebar') }
    if ($Text['icons'] -notmatch '\{PageId::kCheats, "cheats\.svg"') { $failures.Add('the Cheats page has its sidebar icon') }
    $settings = $Text['settings']
    foreach ($i in 0..2) { if ($settings -notmatch "\{cheats::kToggles\[$i\]\.key, `"0`"\}") { $failures.Add("saved toggle $i defaults to Off") } }
    foreach ($i in 0..2) { if ($settings -notmatch "\{cheats::kMultipliers\[$i\]\.key, `"1`"\}") { $failures.Add("saved multiplier $i defaults to Off") } }
    if ($overlay -notmatch 'case SDL_GAMEPAD_BUTTON_DPAD_UP: return sos::menu::kPadUp;' -or $overlay -notmatch 'case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return sos::menu::kPadDown;' -or
        $overlay -notmatch 'case SDL_GAMEPAD_BUTTON_SOUTH: return sos::menu::kPadActivate;' -or $overlay -notmatch 'case SDL_GAMEPAD_BUTTON_EAST: return sos::menu::kPadClose;') { $failures.Add('D-pad, A/Cross and B/Circle drive the menu') }
    if ($overlay -notmatch 'pad_state\.Button\(event\.gbutton\.which, bit, event\.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN\);\s*\}\s*return cheat_open;') { $failures.Add('controller buttons used by the open menu never reach the game') }
    if ($overlay -notmatch 'pad_state\.StickY\(event\.gaxis\.which, event\.gaxis\.value\);\s*\}\s*return cheat_open;') { $failures.Add('the left stick moves the menu and never reaches the game while it is open') }
    if ($overlay -notmatch 'SDL_EVENT_GAMEPAD_REMOVED:\s*pad_state\.Remove\(event\.gdevice\.which\);') { $failures.Add('an unplugged controller does not hold the close gate') }
    if ($overlay -notmatch 'cheat_pad\.Poll\(pad_state\.Take\(\), ImGui::GetTime\(\)\)' -or $overlay -notmatch 'menu_close\.Step\(menu_pressed\.any\(\) \|\| cheat_pad\.Held\(\) != 0' -or
        $overlay -notmatch 'sos::cheats::ApplyPadStep\(step, cheat_list, menu_close\);') { $failures.Add('the controller steps the shared selection and holds the close gate') }
    if ($overlay -notmatch 'MenuTop\(margin, fps_shown, fps_shown \? FpsBox\(\) : sos::cheats::Box\{\}' -or $overlay -notmatch 'SetNextWindowPos\(ImVec2\(right, top\)') { $failures.Add('the menu is placed below a shown frame counter') }
    if ($overlay -notmatch 'ImGuiKey_UpArrow, true\) \|\| pressed\(ImGuiKey_W, true\)' -or $overlay -notmatch 'ImGuiKey_Enter, false\)' -or $overlay -notmatch 'AddMouseButtonEvent' -or $overlay -notmatch 'AddMousePosEvent') { $failures.Add('keyboard and mouse still work in the menu') }
    foreach ($key in $Text.Keys) { if ($Text[$key] -match 'Pass Through Gates|PassThroughGates|pass_through_gates|kGates|7 of 8') { $failures.Add("Pass Through Gates is gone from $key") } }
    if ($Text['cmake'] -notmatch 'src/sos_cheat_patch\.c' -or $Text['cmake'] -notmatch 'src/runtime_cheats\.cpp') { $failures.Add('the game links the cheat engine and runtime') }
    return ,$failures
}

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $src = Join-Path $root 'src'
    $tests = Join-Path $root 'tests'
    $rootText = $root.Replace('\', '/')
    $clang = Join-Path $tc.Bin 'clang.exe'
    $clangxx = Join-Path $tc.Bin 'clang++.exe'

    function Build-Test([string]$Folder, [string[]]$Include, [hashtable]$Sources) {
        $common = @('--target=x86_64-w64-mingw32', '-Wall', '-Wextra', '-Werror', '-O1', '-D_WIN32_WINNT=0x0A00')
        $includes = @()
        $dirs = if ($Include.Count) { $Include + @($tests) } else { @($src, $tests) }
        foreach ($dir in $dirs) { $includes += @('-I', $dir) }
        $objects = @()
        & $clang @common '-std=gnu11' @includes '-c' $Sources['patch'] '-o' (Join-Path $Folder 'patch.o')
        if ($LASTEXITCODE -ne 0) { return $false }
        & $clangxx @common '-std=c++20' @includes '-c' $Sources['runtime'] '-o' (Join-Path $Folder 'runtime.o')
        if ($LASTEXITCODE -ne 0) { return $false }
        & $clangxx @common '-std=c++20' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) @includes '-c' $Sources['test'] '-o' (Join-Path $Folder 'test.o')
        if ($LASTEXITCODE -ne 0) { return $false }
        $objects = @((Join-Path $Folder 'test.o'), (Join-Path $Folder 'runtime.o'), (Join-Path $Folder 'patch.o'))
        & $clangxx '--target=x86_64-w64-mingw32' '-pthread' @objects '-o' (Join-Path $Folder 'cheats_tests.exe')
        return $LASTEXITCODE -eq 0
    }

    $production = @{ patch = (Join-Path $src 'sos_cheat_patch.c'); runtime = (Join-Path $src 'runtime_cheats.cpp'); test = (Join-Path $tests 'cheats_tests.cpp') }
    $build = Join-Path $temp 'build'
    New-Item -ItemType Directory -Path $build | Out-Null
    if (-not (Build-Test $build @() $production)) { throw 'could not build the cheats test' }
    $run = Invoke-TimedProgram (Join-Path $build 'cheats_tests.exe') 300 "`"$sandbox`""
    Write-Host $run.Text.TrimEnd()
    Report 'cheats test program (model, rules, menu, engine, handlers, item actions, CUSA57767 1.00 and CUSA57767 1.01 images)' ($run.Code -eq 0)

    $python = Get-Python
    if ($python) {
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\cheat_sites.py') 'check'
        Report 'scripts\cheat_sites.py check: the generated table against CUSA57767 1.00 and CUSA57767 1.01' ($LASTEXITCODE -eq 0)
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\cheat_sites.py') '--self-test'
        Report 'scripts\cheat_sites.py self-test' ($LASTEXITCODE -eq 0)
    } else {
        Report 'Python 3 found' $false
    }

    $texts = @{
        overlay = Read-Source 'gpu\shim\dcp_overlay_generic.cpp'; pad = Read-Source 'src\runtime_pad.c'; loader = Read-Source 'src\loader.c'
        launcher = Read-Source 'launcher\launcher.cpp'; options = Read-Source 'launcher\options.h'; icons = Read-Source 'launcher\page_icons.cpp'
        settings = Read-Source 'launcher\settings.cpp'; cmake = Read-Source 'CMakeLists.txt'
        model = Read-Source 'src\sos_cheats.h'; menu = Read-Source 'src\sos_cheat_menu.h'; runtime = Read-Source 'src\runtime_cheats.cpp'
        runtimeh = Read-Source 'src\runtime_cheats.h'; sites = Read-Source 'src\sos_cheat_sites.h'; generator = Read-Source 'scripts\cheat_sites.py'
        docs = Read-Source 'docs\CHEATS.md'; readme = Read-Source 'README.md'; package = Read-Source 'packaging\README.txt'
    }
    $failures = Get-ContractFailures $texts
    foreach ($f in $failures) { Report "contract: $f" $false }
    Report "source contracts of the F10 menu, the pad, the loader, the launcher page and the saved settings ($($failures.Count) broken)" ($failures.Count -eq 0)
    Report 'the Cheats sidebar icon exists' (Test-Path -LiteralPath (Join-Path $root 'assets\launcher\icons\cheats.svg'))

    $contractMutations = @(
        @{ Key = 'overlay'; Old = 'bool CapturesInput() { return cheat_open; }'; New = 'bool CapturesInput() { return false; }'; What = 'F10 menu lets the game input through' }
        @{ Key = 'overlay'; Old = "                PressCheatMenuKey(event.key.repeat);`n            }`n            return true;"; New = "                PressCheatMenuKey(event.key.repeat);`n            }`n            return false;"; What = 'F10 key reaches the game' }
        @{ Key = 'settings'; Old = '{cheats::kToggles[0].key, "0"}'; New = '{cheats::kToggles[0].key, "1"}'; What = 'God Mode saved On by default' }
        @{ Key = 'overlay'; Old = '            pad_state.Button(event.gbutton.which, bit, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);'+"`n        }`n        return cheat_open;"; New = '            pad_state.Button(event.gbutton.which, bit, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);'+"`n        }`n        return false;"; What = 'controller input leaks to the game while the menu is open' }
        @{ Key = 'overlay'; Old = 'MenuTop(margin, fps_shown, fps_shown ? FpsBox() : sos::cheats::Box{}'; New = 'MenuTop(margin, false, sos::cheats::Box{}'; What = 'the overlay ignores a shown frame counter' }
        @{ Key = 'launcher'; Old = '        CheatChoice(ui, cheats::Multiplier::JumpHeight);'; New = "        CheatChoice(ui, cheats::Multiplier::JumpHeight);`n        ui.Toggle(`"Pass Through Gates`", nullptr, `"cheat_pass_through_gates`", false);"; What = 'Pass Through Gates back on the launcher page' }
        @{ Key = 'menu'; Old = '    MultiplierRow(Multiplier::JumpHeight),'; New = "    MultiplierRow(Multiplier::JumpHeight),`n    HeadingRow(`"Pass Through Gates`"),"; What = 'Pass Through Gates back in the F10 menu' }
        @{ Key = 'runtime'; Old = 'enum UnitId { kGod, kSpirit, kMagic, kDamage'; New = 'enum UnitId { kGod, kSpirit, kMagic, kGates, kDamage'; What = 'a dead Gate unit left in the runtime' }
        @{ Key = 'launcher'; Old = '        ui.Heading("ITEMS");'; New = "        ui.Toggle(`"Add 1000 Blood Orbs`", nullptr, `"cheat_add_orbs`", false);`n        ui.Heading(`"ITEMS`");"; What = 'an item action offered as a saved setting' }
    )
    $contractDetected = 0
    foreach ($m in $contractMutations) {
        $copy = $texts.Clone()
        $old = $m.Old.Replace("`r`n", "`n")
        if (-not $copy[$m.Key].Contains($old)) { Report "contract mutation text found: $($m.What)" $false; continue }
        $copy[$m.Key] = $copy[$m.Key].Replace($old, $m.New)
        if ((Get-ContractFailures $copy).Count -gt 0) { $contractDetected++; Write-Host "  mutation detected: $($m.What)" } else { Report "contract mutation '$($m.What)' detected" $false }
    }

    $mutations = @(
        @{ File = 'sos_cheats.h'; Old = "    if (!on || !player) {`n        return in;`n    }`n    const float amount = in.amount < 0.0f"; New = "    if (!on || (!player && !on)) {`n        return in;`n    }`n    const float amount = in.amount < 0.0f"; What = 'God Mode applied to enemies too' }
        @{ File = 'sos_cheats.h'; Old = 'if (factor <= kOff || !source_player || target_player || unmodifiable'; New = 'if (factor <= kOff || (!source_player && !target_player && factor < 0) || unmodifiable'; What = 'Damage Multiplier applied to damage the player receives' }
        @{ File = 'sos_cheats.h'; Old = '{6, "6", "6x"}};' + "`ninline constexpr Choice kMovementChoices"; New = '{5, "6", "6x"}};' + "`ninline constexpr Choice kMovementChoices"; What = 'Damage 6x broken' }
        @{ File = 'sos_cheats.h'; Old = 'std::array<bool, kToggleCount> toggles{};'; New = 'std::array<bool, kToggleCount> toggles{true, true, true};'; What = 'cheats On by default' }
        @{ File = 'sos_cheats.h'; Old = '"loot_epic_spear_tip_status",'; New = '"loot_epic_spear_tip_status", "loot_boss_cyclops",'; What = 'a boss trophy among the upgrade materials' }
        @{ File = 'sos_cheats.h'; Old = 'std::sqrt(static_cast<float>(factor))'; New = 'static_cast<float>(factor)'; What = 'jump scaled linearly instead of by the square root' }
        @{ File = 'sos_cheats.h'; Old = '    if (speed <= kOff || !player) {'; New = '    if (speed <= kOff || (!player && speed < 0)) {'; What = 'Movement Speed applied to every character' }
        @{ File = 'sos_cheats.h'; Old = 'return damage == damage && product == product && product - product == 0.0f && damage - damage == 0.0f;'; New = '(void)product;
    return true;'; What = 'NaN and infinity multiplied' }
        @{ File = 'sos_cheat_menu.h'; Old = "    const float below = fps.y1 + margin * 0.5f;"; New = "    const float below = fps.y0 + margin * 0.5f;"; What = 'the menu overlaps the frame counter' }
        @{ File = 'sos_cheat_menu.h'; Old = "    const float below = fps.y1 + margin * 0.5f;`n    return below > margin ? below : margin;"; New = "    return margin;"; What = 'the menu position ignores a shown frame counter' }
        @{ File = 'sos_menu_input.h'; Old = 'step.move = (pressed & kPadUp) ? -1 : (pressed & kPadDown) ? 1 : 0;'; New = 'step.move = 0;'; What = 'the D-pad does not navigate' }
        @{ File = 'sos_cheat_menu.h'; Old = 'p.stick = value < -kStickThreshold ? menu::kPadUp : value > kStickThreshold ? menu::kPadDown : 0;'; New = '(void)value; p.stick = 0;'; What = 'the left stick does not navigate' }
        @{ File = 'sos_cheat_menu.h'; Old = '    if (step.activate) {'; New = '    if (step.activate && step.close) {'; What = 'A/Cross does not activate' }
        @{ File = 'sos_menu_input.h'; Old = 'step.close = (pressed & kPadClose) != 0;'; New = 'step.close = false;'; What = 'B/Circle does not close' }
        @{ File = 'sos_menu_input.h'; Old = 'step.activate = (pressed & kPadActivate) != 0;'; New = 'step.activate = (current & kPadActivate) != 0;'; What = 'a held A/Cross runs the row several times' }
        @{ File = 'sos_menu_input.h'; Old = 'uint32_t Held() const { return previous_; }'; New = 'uint32_t Held() const { return 0; }'; What = 'a button held at close leaks to the game' }
        @{ File = 'sos_cheat_menu.h'; Old = 'void Remove(uint32_t pad) { pads_.erase(pad); }'; New = 'void Remove(uint32_t pad) { (void)pad; }'; What = 'an unplugged controller keeps the close gate shut' }
        @{ File = 'runtime_cheats.cpp'; Old = "    session.Set(which, saved, on);"; New = "    saved.toggles[Index(which)] = on;`n    session.Set(which, saved, on);"; What = 'an F10 override saved as the launcher value' }
        @{ File = 'runtime_cheats.cpp'; Old = "    session.Reset();`n"; New = "`n"; What = 'a new session keeps the F10 overrides' }
        @{ File = 'runtime_cheats.cpp'; Old = '    const SosCheatWrite result = Transition(unit, false);'; New = '    const SosCheatWrite result = SOS_CHEAT_WRITE_DONE;'; What = 'restore removed' }
        @{ File = 'runtime_cheats.cpp'; Old = '        game.wallet_add(wallet, items[i], amount, hide, nullptr);'; New = '        game.wallet_add(wallet, items[i], amount - game.wallet_amount(wallet, items[i], nullptr), hide, nullptr);'; What = 'Add 1000 Blood Orbs overwrites the count instead of adding' }
        @{ File = 'runtime_cheats.cpp'; Old = "    damage = original;`n    return 1;"; New = "    return 1;"; What = 'the shared damage config is left multiplied' }
        @{ File = 'runtime_cheats.cpp'; Old = '    wallet_action.store(action);'; New = "    game.get_pawn(nullptr, nullptr);`n    wallet_action.store(action);"; What = 'an item action touches game objects on the requesting (window) thread' }
        @{ File = 'runtime_cheats.cpp'; Old = 'game.player_index(controller, nullptr) != 0 ||'; New = ''; What = "player 2's controller takes the action" }
        @{ File = 'sos_cheat_patch.c'; Old = "    if (fnv1a(image + site->method.begin, site->method.end - site->method.begin) != site->method_hash)`n        return fail(why, `"the method holding the site is not the analysed code`");"; New = ''; What = 'patch installed without the method fingerprint' }
        @{ File = 'sos_cheat_patch.c'; Old = 'if (!strcmp(kBuilds[i]->build, build_key)) return kBuilds[i];'; New = 'if (!strcmp(kBuilds[i]->build, build_key)) return kBuilds[0];'; What = 'the 1.00 sites used for 1.01' }
        @{ File = 'sos_cheat_patch.c'; Old = '    if (!memcmp(expected + offset, from, detour->length)) {'; New = '    if (!memcmp(expected + offset, from, detour->length) || 1) {'; What = 'a write that does not check the bytes it replaces' }
        @{ File = 'sos_cheat_patch.c'; Old = '    EMIT(&e, 0x48, 0x8d, 0x64, 0x24, 0x80); /* lea rsp, [rsp - 0x80]: the red zone stays untouched */'; New = '    EMIT(&e, 0x48, 0x8d, 0x64, 0x24, 0x00);'; What = 'the stub writes into the red zone' }
    )
    # Every cheat source is copied, so a header the others include by a quoted path is the mutated one too.
    $cheatSources = @('sos_cheats.h', 'sos_cheat_menu.h', 'sos_menu_input.h', 'sos_cheat_patch.h', 'sos_cheat_patch.c', 'sos_cheat_sites.h', 'runtime_cheats.h', 'runtime_cheats.cpp')
    if ($env:SOS_TEST_NO_MUTATIONS -eq '1') { $mutations = @() }
    $index = 0
    $detected = 0
    foreach ($m in $mutations) {
        $index++
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Path $folder | Out-Null
        $workspace = Join-Path $folder 'src'
        foreach ($name in $cheatSources) { $null = Copy-ToWorkspace $workspace (Join-Path $src $name) $name }
        Edit-WorkspaceText (Join-Path $workspace $m.File) $m.Old $m.New
        $sources = $production.Clone()
        $sources['runtime'] = Join-Path $workspace 'runtime_cheats.cpp'
        $sources['patch'] = Join-Path $workspace 'sos_cheat_patch.c'
        $exe = Join-Path $folder 'cheats_tests.exe'
        $compile = {
            $ok = Build-Test $folder @($workspace) $sources
            $global:LASTEXITCODE = if ($ok) { 0 } else { 1 }
        }
        $mutantSandbox = Join-Path $folder 'sos_cheats_data'
        New-Item -ItemType Directory -Path $mutantSandbox | Out-Null
        if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }
        & $compile
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
            Report "mutation '$($m.What)' compiles" $false
            continue
        }
        $mutant = Invoke-TimedProgram $exe 120 "`"$mutantSandbox`""
        if ($mutant.Code -ne 0) { $detected++; Write-Host "  mutation detected: $($m.What)" } else { Report "mutation '$($m.What)' detected" $false }
    }
    $total = $mutations.Count + $contractMutations.Count
    Write-Host "Cheats mutations: $($detected + $contractDetected) of $total rejected (CUSA57767 1.00 and CUSA57767 1.01)"
    if ($detected -ne $mutations.Count -or $contractDetected -ne $contractMutations.Count) { $result = 1 }
} finally {
    Remove-RunFolder $temp
}
exit $result
