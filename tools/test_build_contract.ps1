# Headless check of the port's contract with CUSA57767 1.00 and 1.01 from the tables of the sources alone: registration, fingerprints,
# the features each build has, every patched and verified range inside the image and no byte shared by two features, and the refusal of
# an unknown build; then mutated copies of the tables that must each fail. No game file, boot image, launcher or window is read or
# started, so it holds on every machine.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_build_contract_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$result = 0
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $launcher = Join-Path $root 'launcher'
    $src = Join-Path $root 'src'
    $cSources = @((Join-Path $src 'sos_skip_intro.c'), (Join-Path $src 'sos_hermes_unlock.c'), (Join-Path $src 'sos_pit_unlock.c'),
        (Join-Path $src 'sos_exit_game.c'), (Join-Path $launcher 'controller_prompts.c'), (Join-Path $launcher 'prompt_dialog.c'),
        (Join-Path $launcher 'image_glyphs.c'))
    $profileSource = Join-Path $launcher 'game_profile.cpp'

    # Compiles the test; a source named in $Replace is taken from its mutated copy.
    function Build-Contract([string]$Folder, [hashtable]$Replace) {
        New-Item -ItemType Directory -Force -Path $Folder | Out-Null
        $objects = @()
        foreach ($source in $cSources) {
            $object = Join-Path $Folder ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
            $from = if ($Replace.ContainsKey($source)) { $Replace[$source] } else { $source }
            & (Join-Path $tc.Bin 'clang.exe') '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O1' '-D_WIN32_WINNT=0x0A00' '-I' $launcher '-I' $src '-c' $from '-o' $object
            if ($LASTEXITCODE -ne 0) { return $null }
            $objects += $object
        }
        $profile = if ($Replace.ContainsKey($profileSource)) { $Replace[$profileSource] } else { $profileSource }
        $exe = Join-Path $Folder 'build_contract_tests.exe'
        & (Join-Path $tc.Bin 'clang++.exe') '--target=x86_64-w64-mingw32' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $root.Replace('\', '/')) `
            '-I' $launcher '-I' $src (Join-Path $root 'tests\build_contract_tests.cpp') $profile @objects '-o' $exe
        if ($LASTEXITCODE -ne 0) { return $null }
        return $exe
    }

    $exe = Build-Contract (Join-Path $temp 'real') @{}
    if (-not $exe) { throw 'could not compile the build contract test' }
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $mutations = @(
        @{ File = 'src\sos_hermes_unlock.c'; Old = '{"CUSA57767-1.00", 168044704u, 0x4be86d2, 0x4be87dd}'; New = '{"CUSA57767-1.00", 168044704u, 0x4c22879, 0x4be87dd}'; What = 'the Hermes Mode patch lands on the Pit check' }
        @{ File = 'src\sos_pit_unlock.c'; Old = '{"CUSA57767-1.00", 168044704u, 0x4c22879, 0x90e2640}'; New = '{"CUSA57767-1.00", 168044704u, 0xffc22879, 0x90e2640}'; What = 'the Pit gate lies outside the image' }
        @{ File = 'src\sos_pit_unlock.c'; Old = '    {"CUSA57767-1.01", 168046032u, 0x4c290b9, 0x90f9b20},'; New = ''; What = '1.01 has no Pit site' }
        @{ File = 'src\sos_pit_unlock.c'; Old = "    return NULL;`n}`n`nstatic int calls"; New = "    return &kSites[0];`n}`n`nstatic int calls"; What = 'an unknown build reaches the Pit site' }
        @{ File = 'src\sos_skip_intro.c'; Old = '    {"CUSA57767-1.00", 168044704u, 0x4fe1d40, 0x49b3aa0, 0x8092f30, 0x8092ee0, 0x528d1a0, 0x52876f0, 0x4bea87c, 0x4be6790},'; New = ''; What = '1.00 has no Skip Intro site' }
        @{ File = 'launcher\game_profile.cpp'; Old = '    if (sha256 == build.eboot_sha256) {'; New = '    if (!sha256.empty() || sha256.empty()) {'; What = 'any executable is a supported build' }
        @{ File = 'launcher\image_glyphs_sites.h'; Old = '{{0x92656f0u, 0x50u,'; New = '{{0x9265740u, 0x50u,'; What = 'two glyph hooks patch the same bytes'; Together = 'launcher\image_glyphs.c' }
    )
    $index = 0
    $detected = 0
    foreach ($m in $mutations) {
        $index++
        $folder = Join-Path $temp "mutation$index"
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $original = Join-Path $root $m.File
        $workspace = Copy-ToWorkspace $folder $original ([IO.Path]::GetFileName($m.File))
        Edit-WorkspaceText $workspace $m.Old $m.New
        $replace = @{}
        if ($m.ContainsKey('Together')) {
            $together = Join-Path $root $m.Together
            $replace[$together] = Copy-ToWorkspace $folder $together ([IO.Path]::GetFileName($m.Together))
        } else {
            $replace[$original] = $workspace
        }
        $mutantExe = Join-Path $folder 'build_contract_tests.exe'
        if (Test-MutationDetected $m.What { $null = Build-Contract $folder $replace; $global:LASTEXITCODE = 0 } $mutantExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "Build contract mutations: $detected of $($mutations.Count) rejected (CUSA57767 1.00 and CUSA57767 1.01)"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
