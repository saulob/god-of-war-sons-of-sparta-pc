# DEBUG_SAULO: headless validation of the minimal F5 capture trigger, read-only camera telemetry hook,
# baseline GPU invariants, hook site verification in both installed boot images, and mutations.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$temp = Join-Path ([IO.Path]::GetTempPath()) ('sos_uw_probe_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$cc = Join-Path $tc.Bin 'clang.exe'
$target = '--target=x86_64-w64-mingw32'
$rootText = $root.Replace('\', '/')
$src = Join-Path $root 'src'
$probe = Join-Path $src 'sos_uw_probe.c'

function Build-Suite([string]$Exe, [string]$Workspace = '', [string]$Probe = $probe) {
    $ErrorActionPreference = 'Continue'
    $include = @()
    if ($Workspace) { $include += @('-I', $Workspace) }
    $include += @('-I', $src, '-I', (Join-Path $root 'gpu\shim'), '-I', (Join-Path $root 'gpu\shadps4'))
    $object = [IO.Path]::ChangeExtension($Exe, '.probe.o')
    & $cc $target '-std=c11' '-Wall' '-Wextra' '-Werror' '-O1' @include '-c' $Probe '-o' $object
    if ($LASTEXITCODE -ne 0) { return }
    & $clangxx $target '-std=c++20' '-Wall' '-Wextra' '-Werror' '-O1' (Get-StringDefine 'SOS_SOURCE_DIR' $rootText) @include `
        (Join-Path $root 'tests\uw_probe_tests.cpp') $object '-o' $Exe
}

$result = 0
try {
    $exe = Join-Path $temp 'test_uw_probe.exe'
    Build-Suite $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile uw_probe_tests.cpp' }
    & $exe
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    $python = Get-Python
    if ($python) {
        & $python.Exe @($python.Args) '-I' (Join-Path $root 'scripts\analyze_ultrawide_capture.py') '--check-sites'
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    } else {
        Write-Host '  [fail] Python 3 not found'
        $result = 1
    }

    $f5Header = Join-Path $root 'gpu\shim\dcp_f5_capture.h'
    $window = Join-Path $root 'gpu\shim\dcp_capture_window.h'
    $mutations = @(
        @{ File = $window; Old = 'return recorded >= max_passes ? Step::Finish : Step::Record;'; New = '(void)recorded; (void)max_passes; return Step::Record;'; What = 'the capture window is unbounded' }
        @{ File = $window; Old = 'return Step::Begin;'; New = 'return Step::Skip;'; What = 'an armed capture never starts' }
        @{ File = $probe; Old = '    const float x = raw[0] + correction[0], y = raw[1] + correction[1];'; New = '    const float x = raw[0], y = raw[1];'; What = 'the camera ignores the position correction' }
        @{ File = $probe; Old = '                         : memcmp(image + site, kPrefix, sizeof(kPrefix))   ? "CinemachineBrain.PushStateToUnityCamera bytes mismatch"'; New = '                         : memcmp(image + site, kPrefix, 0)   ? "CinemachineBrain.PushStateToUnityCamera bytes mismatch"'; What = 'the hook patches code it did not recognise' }
        @{ File = $f5Header; Old = '        if (repeat) {'; New = '        if (false && repeat) {'; What = 'F5 key repeat is not ignored' }
        @{ File = $f5Header; Old = '        if (current >= kMaxCaptures) {'; New = '        if (current > kMaxCaptures) {'; What = 'F5 allows more than 8 captures' }
    )
    $detected = 0
    $index = 0
    foreach ($m in $mutations) {
        $index++
        $workspace = Join-Path $temp "mutation_$index"
        New-Item -ItemType Directory -Force -Path $workspace | Out-Null
        $copy = Copy-ToWorkspace $workspace $m.File ([IO.Path]::GetFileName($m.File))
        Edit-WorkspaceText $copy $m.Old $m.New
        $mexe = Join-Path $workspace 'test_uw_probe.exe'
        $probePath = if ($m.File -eq $probe) { $copy } else { $probe }
        if (Test-MutationDetected $m.What { Build-Suite $mexe $workspace $probePath *> $null } $mexe) { $detected++ }
    }
    Write-Host "  [uw-probe] mutations: $($mutations.Count), detected: $detected"
    if ($detected -ne $mutations.Count) { $result = 1 }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
