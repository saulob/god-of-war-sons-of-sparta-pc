# Headless proof of the device-memory fallback policy (memory types tried in order, only a full device memory moves on,
# memoryTypeBits and required properties respected, telemetry only for a real fallback) on the production header, the
# wiring of the allocations that use it, and mutated copies of the policy that the same test must reject. No window, GPU,
# launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_memory_fallback_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $gpu = Join-Path $root 'gpu'
    $rootText = $root.Replace('\', '/')
    $clang = Join-Path $tc.Bin 'clang++.exe'
    $include = @('-I', (Join-Path $gpu 'shim'), '-I', (Join-Path $gpu 'shadps4'), '-I', (Join-Path $root 'src'),
        '-I', (Join-Path $gpu 'third_party/half/include'), '-I', (Join-Path $gpu 'third_party/magic_enum/include'),
        '-I', (Join-Path $prefix 'include'))
    $flags = @('--target=x86_64-w64-mingw32', '-std=gnu++23', '-Wall', '-Wextra', '-O1', '-DNOMINMAX', '-Wno-deprecated-declarations',
        '-Wno-missing-field-initializers', '-include', (Join-Path $gpu 'shim/dcp_win_compat.h'), "-DSOS_SOURCE_DIR=$rootText")
    $policy = Join-Path $gpu 'shadps4/video_core/allocation_fallback.h'
    $test = Join-Path $root 'tests/memory_fallback_tests.cpp'

    $exe = Join-Path $temp 'memory_fallback_tests.exe'
    & $clang @flags @include $test '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the memory fallback test' }
    & $exe
    $result = $LASTEXITCODE

    $mutations = @(
        @{ Name = 'a result other than a full device memory moves on'; Old = 'if (allocation.result != vk::Result::eErrorOutOfDeviceMemory) {'; New = 'if (allocation.result == vk::Result::eSuccess) {' },
        @{ Name = 'a type whose memoryTypeBits bit is clear is used'; Old = 'if (type >= count || ((allowed_bits >> type) & 1) == 0) {'; New = 'if (type >= count) {' },
        @{ Name = 'a required property is dropped'; Old = 'return (flags & required) == required && !(flags & never);'; New = 'return !(flags & never);' },
        @{ Name = 'a protected or lazily allocated type stands in'; Old = 'return (flags & required) == required && !(flags & never);'; New = 'return (flags & required) == required;' },
        @{ Name = 'system memory is tried before device-local types'; Old = 'for (const bool device_local : {true, false}) {'; New = 'for (const bool device_local : {false, true}) {' },
        @{ Name = 'the preferred type is listed twice'; Old = 'if (type != preferred && usable(type) && is_local == device_local) {'; New = 'if (usable(type) && is_local == device_local) {' },
        @{ Name = 'telemetry is emitted when the preferred type succeeds'; Old = 'if (allocation.result != vk::Result::eSuccess || allocation.candidate == original) {'; New = 'if (allocation.result != vk::Result::eSuccess) {' },
        @{ Name = 'telemetry is emitted for a failed allocation'; Old = 'if (allocation.result != vk::Result::eSuccess || allocation.candidate == original) {'; New = 'if (allocation.candidate == original) {' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + $detected + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $copy = Copy-ToWorkspace $work $policy 'video_core/allocation_fallback.h'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'memory_fallback_tests.exe'
        $build = { & $clang @flags '-w' '-I' $work @include $test '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "memory fallback mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
