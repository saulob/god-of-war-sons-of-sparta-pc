# Headless checks of the launcher: page order, page icons at 100%, 150% and 225%, header art placement, settings,
# key bindings, game process arguments, the game identity, the supported-build names, the TRG patch set and what the
# launcher must not contain (Online, another game's identity). No window, game or network.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_launcher_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw),
    'project\(sons_of_sparta_pc VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'CMakeLists.txt has no project version' }
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clang = Join-Path $tc.Bin 'clang.exe'
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$prefix = Split-Path $tc.Bin -Parent
$target = '--target=x86_64-w64-mingw32'
$include = @('-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'gpu\shim'),
             '-I', (Join-Path $root 'third_party\trg-launcher\include'), '-I', (Join-Path $prefix 'include'))

$icons = @(Get-ChildItem -LiteralPath (Join-Path $root 'assets\launcher\icons') -Filter '*.svg' | Sort-Object Name |
           ForEach-Object { $_.FullName.Replace('\', '/') })
$iconList = Join-Path $temp 'page_icons.txt'
$iconSource = Join-Path $temp 'page_icon_data.cpp'
[IO.File]::WriteAllText($iconList, ($icons -join ';'))
& (Join-Path $tc.Bin 'cmake.exe') "-DINPUT_LIST=$($iconList.Replace('\', '/'))" "-DOUTPUT=$($iconSource.Replace('\', '/'))" `
    '-DSYMBOL=kPageIconFiles' '-P' (Join-Path $root 'launcher\embed_files.cmake')
if ($LASTEXITCODE -ne 0) { throw 'could not embed the page icons' }

$objects = New-Object System.Collections.Generic.List[string]
function Compile([string]$Compiler, [string[]]$Flags, [string]$Source, [string]$Name) {
    $object = Join-Path $temp ($Name + '.o')
    & $Compiler $target @Flags @include (Get-StringDefine 'SOS_PORT_VERSION' $version) '-c' $Source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $Source" }
    $objects.Add($object)
}

$flags20 = @('-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1')
$root_text = $root.Replace('\', '/')
Compile $clangxx ($flags20 + (Get-StringDefine 'SOS_SOURCE_DIR' $root_text)) (Join-Path $root 'tests\launcher_tests.cpp') 'launcher_tests'
foreach ($name in 'options', 'settings', 'page_icons', 'header_art', 'game_process', 'game_files', 'game_profile', 'crypto_win', 'resolution') {
    Compile $clangxx $flags20 (Join-Path $root "launcher\$name.cpp") $name
}
Compile $clangxx $flags20 (Join-Path $root 'launcher\localization\ui_text.cpp') 'ui_text'
Compile $clangxx $flags20 $iconSource 'page_icon_data'
foreach ($name in 'settings', 'platform') {
    Compile $clangxx @('-std=c++17', '-O1', '-D_CRT_SECURE_NO_WARNINGS') (Join-Path $root "third_party\trg-launcher\src\$name.cpp") "trg_$name"
}
Compile $clang @('-std=c11', '-Wall', '-Wextra', '-Werror', '-O1') (Join-Path $root 'src\sos_keys.c') 'sos_keys'
Compile $clang @('-std=c11', '-Wall', '-Wextra', '-Werror', '-O1') (Join-Path $root 'launcher\controller_prompts.c') 'controller_prompts'
Compile $clang @('-std=c11', '-Wall', '-Wextra', '-Werror', '-O1') (Join-Path $root 'src\sos_hermes_unlock.c') 'sos_hermes_unlock'

$exe = Join-Path $temp 'test_launcher.exe'
& $clangxx $target @objects '-L' (Join-Path $prefix 'lib') '-lSDL3' '-lshell32' '-lole32' '-lcomdlg32' '-lwinhttp' '-lbcrypt' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not link the launcher test' }

& $exe
$result = $LASTEXITCODE

# The window fit and the data folder tests share the launcher's objects.
$shared = @($objects | Where-Object { (Split-Path $_ -Leaf) -ne 'launcher_tests.o' })
foreach ($test in @(@{ Name = 'window_fit_tests'; Extra = @() }, @{ Name = 'paths_tests'; Extra = @('paths') })) {
    $testObjects = New-Object System.Collections.Generic.List[string]
    $object = Join-Path $temp ($test.Name + '.o')
    & $clangxx $target @flags20 @include '-c' (Join-Path $root "tests\$($test.Name).cpp") '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $($test.Name)" }
    $testObjects.Add($object)
    foreach ($name in $test.Extra) {
        $extra = Join-Path $temp ($name + '.o')
        & $clangxx $target @flags20 @include (Get-StringDefine 'SOS_PORT_VERSION' $version) '-c' (Join-Path $root "launcher\$name.cpp") '-o' $extra
        if ($LASTEXITCODE -ne 0) { throw "could not compile $name" }
        $testObjects.Add($extra)
    }
    $testExe = Join-Path $temp ($test.Name + '.exe')
    & $clangxx $target @testObjects @shared '-L' (Join-Path $prefix 'lib') '-lSDL3' '-lshell32' '-lole32' '-lcomdlg32' '-lwinhttp' '-lbcrypt' '-o' $testExe
    if ($LASTEXITCODE -ne 0) { throw "could not link $($test.Name)" }
    & $testExe
    if ($LASTEXITCODE -ne 0) { $result = $LASTEXITCODE }
}

Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
