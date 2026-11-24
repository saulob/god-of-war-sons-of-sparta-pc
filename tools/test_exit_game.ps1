# Headless validation of title menu Exit Game for CUSA57767 1.00 and 1.01: build sites, real boot images,
# coexistence with Skip Intro and controller prompts, static scene/layout/navigation/localization verification
# across all 23 languages, and source + static mutation tests. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_exit_game_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$launcher = Join-Path $root 'launcher'
$src = Join-Path $root 'src'
$rootText = $root.Replace('\', '/')

function Build-Objects([string]$Folder, [hashtable]$Replace) {
    $objects = @()
    foreach ($unit in @(
            @{ Source = (Join-Path $src 'sos_exit_game.c') },
            @{ Source = (Join-Path $src 'sos_skip_intro.c') },
            @{ Source = (Join-Path $launcher 'controller_prompts.c') },
            @{ Source = (Join-Path $launcher 'prompt_dialog.c') })) {
        $source = $unit.Source
        $name = [IO.Path]::GetFileName($source)
        if ($Replace.ContainsKey($name)) { $source = $Replace[$name] }
        $object = Join-Path $Folder ([IO.Path]::GetFileNameWithoutExtension($source) + '_' + [IO.Path]::GetFileNameWithoutExtension($name) + '.o')
        & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $src '-I' $launcher '-c' $source '-o' $object
        if ($LASTEXITCODE -ne 0) { throw "could not compile $source" }
        $objects += $object
    }
    return $objects
}

function Build-Test([string]$Folder, [string[]]$Objects, [string]$Exe) {
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) '-I' $src '-I' $launcher `
        (Join-Path $root 'tests\exit_game_tests.cpp') @Objects '-L' (Join-Path (Split-Path $tc.Bin) 'lib') '-lZydis' '-lZycore' '-o' $Exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile exit_game_tests.cpp' }
}

$result = 0
try {
    $baseObjects = Build-Objects $temp @{}
    $exe = Join-Path $temp 'test_exit_game.exe'
    Build-Test $temp $baseObjects $exe
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $python = Get-Python
    if ($python) {
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\exit_game_sites.py')
        if ($LASTEXITCODE -ne 0) { $result = 1 }
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\exit_game_sites.py') '--self-test'
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    } else {
        Write-Host '  [fail] Python 3 not found'
        $result = 1
    }

    $sharedObjs = @($baseObjects | Where-Object { [IO.Path]::GetFileName($_) -notlike '*sos_exit_game*' })
    $mutations = @(
        @{ Old = '    if (!atomic_exchange(&g_exit.installed_logged, 1)) puts("Exit Game: title menu installed");
    return 1;'; New = '    return 0;'; What = 'row count stays 5 (toggle hook stops keeping QuitButton active)'; Real = $false }
        @{ Old = 'SOS_EXIT_GAME_SYSV int sos_exit_game_on_confirm(void *anim_event) {
    if (!g_exit.armed) return 0;
    title_animation_event(anim_event);
    if (!is_menu_quit_button(anim_event)) return 0;
    request_host_exit();
    return 1;'; New = 'SOS_EXIT_GAME_SYSV int sos_exit_game_on_confirm(void *anim_event) {
    if (!g_exit.armed) return 0;
    title_animation_event(anim_event);
    return 0;'; What = 'Exit Game confirm fails to intercept QuitButton'; Real = $false }
        @{ Old = '    if (g_exit.host_exit) (void)g_exit.host_exit();'; New = '    (void)g_exit.host_exit;'; What = 'Exit Game confirm does nothing'; Real = $false }
        @{ Old = '    if (!g_exit.armed) return 0;
    title_animation_event(anim_event);
    if (!is_menu_quit_button(anim_event)) return 0;'; New = '    if (!g_exit.armed) return 0;
    title_animation_event(anim_event);'; What = 'non-QuitButton or Back/other button triggers exit'; Real = $false }
        @{ Old = '    {0, "English", "EXIT", "EXIT GAME"},'; New = '    {0, "English", "EXIT", ""},'; What = 'Exit Game text missing in English'; Real = $false }
        @{ Old = '    {4, "Portuguese (Brazil)", "SAIR", "SAIR DO JOGO"},'; New = '    {4, "Portuguese (Brazil)", "SAIR", "EXIT GAME"},'; What = 'wrong Portuguese translation'; Real = $false }
        @{ Old = '    {14, "Japanese", "終了", "ゲームを終了"},'; New = '    {14, "Japanese", "終了", "EXIT GAME"},'; What = 'wrong Japanese translation'; Real = $false }
        @{ Old = '    {15, "Korean", "나가기", "게임 종료"},'; New = '    {15, "Korean", "나가기", "EXIT GAME"},'; What = 'wrong Korean translation'; Real = $false }
        @{ Old = '    {20, "Simplified Chinese", "退出", "退出游戏"},'; New = '    {20, "Simplified Chinese", "退出", "EXIT GAME"},'; What = 'wrong Simplified Chinese translation'; Real = $false }
        @{ Old = '    {23, "Traditional Chinese", "離開", "離開遊戲"},'; New = '    {23, "Traditional Chinese", "離開", "EXIT GAME"},'; What = 'wrong Traditional Chinese translation'; Real = $false }
        @{ Old = '    if (!object_name_is(go, "Text") && !object_name_is(go, "ActiveText")) return 0;'; New = '    if (!object_name_is(go, "Text")) return 0;'; What = 'highlight sixth row ActiveText omitted'; Real = $false }
        @{ Old = '    return NULL;
}

const SosExitGameLanguage'; New = '    return kBuilds[0].site;
}

const SosExitGameLanguage'; What = 'unknown build accepts patch'; Real = $false }
        @{ Old = '    else if (!reason && hash != site->hash)
        reason = "code fingerprint mismatch";'; New = '    (void)hash;'; What = 'fingerprint check removed'; Real = $true }
        @{ Old = '    if (atomic_exchange(&g_exit.requested, 1)) return;'; New = '    atomic_store(&g_exit.requested, 1);'; What = 'exit request can fire repeatedly'; Real = $false }
    )

    $index = 0
    $detected = 0
    foreach ($m in $mutations) {
        $index++
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $workspace = Copy-ToWorkspace $folder (Join-Path $src 'sos_exit_game.c') 'sos_exit_game.c'
        Edit-WorkspaceText $workspace $m.Old $m.New
        $mutObj = Join-Path $folder 'sos_exit_game.o'
        $mutExe = Join-Path $folder 'test.exe'
        $compile = {
            & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $src '-I' $launcher '-c' $workspace '-o' $mutObj
            if ($LASTEXITCODE -ne 0) { throw "compile failed" }
            Build-Test $folder (@($mutObj) + $sharedObjs) $mutExe
            $global:LASTEXITCODE = 0
        }
        if (-not $m.Real) { $env:SOS_TEST_FAST = '1' }
        try {
            $ok = Test-MutationDetected $m.What $compile $mutExe
        } finally {
            Remove-Item Env:\SOS_TEST_FAST -ErrorAction SilentlyContinue
        }
        if ($ok) { $detected++ } else { $result = 1 }
    }
    Write-Host "Exit Game C mutations: $detected of $($mutations.Count) rejected (CUSA57767 1.00 and CUSA57767 1.01)"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
