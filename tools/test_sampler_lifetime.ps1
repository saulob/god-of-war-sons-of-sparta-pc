# Headless proof of the sampler cache lifetime on the production collector and LRU list with stand-in samplers: a sampler is
# stamped with the scheduler tick of the draw that records it and destroyed only after the GPU completed that tick, however
# old its last use, bounded under a lagging GPU, plus the wiring in the texture cache and mutated copies of the collector
# that the same test must reject. No window, GPU, launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_sampler_lifetime_test_' + [guid]::NewGuid().ToString('N'))
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
    $policy = Join-Path $gpu 'shadps4/video_core/texture_cache/sampler_gc.h'
    $test = Join-Path $root 'tests/sampler_lifetime_tests.cpp'

    $exe = Join-Path $temp 'sampler_lifetime_tests.exe'
    & $clang @flags @include $test '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the sampler lifetime test' }
    & $exe
    $result = $LASTEXITCODE

    $mutations = @(
        @{ Name = 'a sampler is destroyed before the GPU completes its last tick'; Old = 'return last_use_tick > completed_tick;'; New = 'return false;' },
        @{ Name = 'a sampler is pinned on the tick the GPU just completed'; Old = 'return last_use_tick > completed_tick;'; New = 'return last_use_tick >= completed_tick;' },
        @{ Name = 'a pinned sampler ends the pass and hides idle ones'; Old = "++run.pinned;`n                return false;"; New = "++run.pinned;`n                return true;" },
        @{ Name = 'a pinned sampler spends the pass budget'; Old = '++run.pinned;'; New = '++run.pinned; --deletions;' },
        @{ Name = 'the pass has no eviction budget'; Old = 'if (deletions == 0) {'; New = 'if (false) {' },
        @{ Name = 'the CPU-side age is ignored'; Old = 'const u64 ticks_to_destroy = std::min<u64>(aggressive ? 160 : pressured ? 80 : 16, gc_tick);'; New = 'const u64 ticks_to_destroy = 0;' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + $detected + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $copy = Copy-ToWorkspace $work $policy 'video_core/texture_cache/sampler_gc.h'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'sampler_lifetime_tests.exe'
        $build = { & $clang @flags '-w' '-I' $work @include $test '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "sampler lifetime mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
