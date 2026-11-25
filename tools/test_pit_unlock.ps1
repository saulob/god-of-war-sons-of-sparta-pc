# Headless validation of The Pit early unlock on CUSA57767 1.00 and 1.01: the Gameplay setting and its default, the verified
# site and the two bytes the patch changes on the real boot images, independence from Hermes Mode, Skip Intro and Exit Game,
# unknown-build, changed-image and missing-content refusal, the static proof from the game code and scenes, and mutated
# copies that must fail. No game, launcher or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_pit_unlock_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$result = 0
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $src = Join-Path $root 'src'
    $rootText = $root.Replace('\', '/')

    function Build-Object([string]$Source, [string]$Object) {
        & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $src '-c' $Source '-o' $Object
        if ($LASTEXITCODE -ne 0) { throw "could not compile $Source" }
    }

    $shared = @()
    foreach ($name in 'sos_hermes_unlock', 'sos_exit_game', 'sos_skip_intro') {
        $object = Join-Path $temp "$name.o"
        Build-Object (Join-Path $src "$name.c") $object
        $shared += $object
    }
    $testObject = Join-Path $temp 'pit_unlock_tests.o'
    & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) `
        '-I' $src '-c' (Join-Path $root 'tests\pit_unlock_tests.cpp') '-o' $testObject
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the Pit unlock test' }

    function Build-Test([string]$Object, [string]$Exe) {
        & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' $testObject $Object @shared '-o' $Exe
        if ($LASTEXITCODE -ne 0) { throw 'could not link the Pit unlock test' }
    }

    $object = Join-Path $temp 'sos_pit_unlock.o'
    Build-Object (Join-Path $src 'sos_pit_unlock.c') $object
    $exe = Join-Path $temp 'pit_unlock_tests.exe'
    Build-Test $object $exe
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $python = Get-Python
    if ($python) {
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\pit_unlock_check.py')
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    } else {
        Write-Host '  [fail] Python 3 not found'
        $result = 1
    }

    $mutations = @(
        @{ Old = 'int on = 0;'; New = 'int on = 1;'; What = 'default on' }
        @{ Old = 'else if (false_value(value)) on = 0;'; New = 'else on = !false_value(value);'; What = 'any value turns the option on' }
        @{ Old = '#include "sos_pit_unlock.h"'; New = "#include `"sos_pit_unlock.h`"`n#undef SOS_PIT_UNLOCK_KEY`n#define SOS_PIT_UNLOCK_KEY `"unlock_hermes_mode`""; What = 'The Pit follows the Hermes Mode setting' }
        @{ Old = '0x4c22879'; New = '0x4c22891'; What = '1.00 gate moved to the Hermes Mode branch' }
        @{ Old = '0x4c290b9'; New = '0x4c22879'; What = '1.00 offset used for 1.01' }
        @{ Old = 'size != site->image_size'; New = 'size < 16'; What = 'image size check removed' }
        @{ Old = 'kGate[i] != ANY && image[start + i] != kGate[i]'; New = 'i < kJumpAt && kGate[i] != ANY && image[start + i] != kGate[i]'; What = 'instruction bytes after the test not verified' }
        @{ Old = '(!calls(image, start + kPitCallAt, site->invoke) || !calls(image, start + kHermesCallAt, site->invoke))'; New = '(site->gate == 0 && !calls(image, start, site->invoke))'; What = 'event call targets not verified' }
        @{ Old = 'kJumpLength = 2'; New = 'kJumpLength = 3'; What = 'one extra byte written' }
        @{ Old = 'memset(image + site->gate, 0x90, kJumpLength)'; New = 'image[site->gate] = 0xeb'; What = 'branch forced to hide The Pit' }
        @{ Old = 'if (!sos_pit_unlock_content(game_folder, missing, sizeof(missing))) {'; New = 'if (!sos_pit_unlock_content(game_folder, missing, sizeof(missing)) && !site) {'; What = 'missing content ignored' }
        @{ Old = '{{175, 200}, {202, 219}}'; New = '{{175, 200}, {202, 218}}'; What = 'PitMode scene not required' }
        @{ Old = '    for (size_t i = 0; i < sizeof(kBanks) / sizeof(*kBanks); ++i)'; New = '    for (size_t i = 0; i < 1; ++i)'; What = 'audio banks not required' }
        @{ Old = "    return NULL;`n}`n`nstatic int calls"; New = "    return &kSites[0];`n}`n`nstatic int calls"; What = 'unknown build accepted' }
    )
    $index = 0
    $detected = 0
    foreach ($m in $mutations) {
        $index++
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $workspace = Copy-ToWorkspace $folder (Join-Path $src 'sos_pit_unlock.c') 'sos_pit_unlock.c'
        Edit-WorkspaceText $workspace $m.Old $m.New
        $mutantObject = Join-Path $folder 'sos_pit_unlock.o'
        $mutantExe = Join-Path $folder 'test.exe'
        $compile = {
            Build-Object $workspace $mutantObject
            Build-Test $mutantObject $mutantExe
            $global:LASTEXITCODE = 0
        }
        if (Test-MutationDetected $m.What $compile $mutantExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "Pit unlock mutations: $detected of $($mutations.Count) rejected (CUSA57767 1.00 and CUSA57767 1.01)"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
