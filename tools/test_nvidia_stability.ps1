# Headless proof of the NVIDIA clip-distance interface between shader stages on the production rule functions and RuntimeInfo
# (the fragment shader reads the emulated attribute exactly when the hardware vertex stage writes it; a pipeline without a
# pixel shader decides from its own registers; A -> B -> A) and the wiring in the pipeline cache and the SPIR-V emitter, with
# mutated copies of the rule that the same test must reject. No window, GPU, launcher or game is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_shader_interface_test_' + [guid]::NewGuid().ToString('N'))
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
    $policy = Join-Path $gpu 'shadps4/shader_recompiler/runtime_info.h'
    $test = Join-Path $root 'tests/shader_interface_tests.cpp'
    $logSource = Join-Path $gpu 'shim/common/logging/log.cpp'
    $assertSource = Join-Path $gpu 'shim/common/assert.cpp'
    $lostSource = Join-Path $gpu 'shim/common/device_lost.cpp'
    $traceSource = Join-Path $gpu 'shim/common/gpu_trace.cpp'

    $exe = Join-Path $temp 'shader_interface_tests.exe'
    & $clang @flags @include $test $logSource $assertSource $lostSource $traceSource '-L' (Join-Path $prefix 'lib') '-lfmt' '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the shader interface test' }
    & $exe
    $result = $LASTEXITCODE

    $mutations = @(
        @{ Name = 'a geometry pipeline reads the emulated attribute'; Old = 'return geometry ? SwStage::Geometry : tessellation ? SwStage::TessellationEval : SwStage::Vertex;'; New = 'return tessellation ? SwStage::TessellationEval : SwStage::Vertex;' },
        @{ Name = 'a tessellation pipeline reads the emulated attribute'; Old = 'return geometry ? SwStage::Geometry : tessellation ? SwStage::TessellationEval : SwStage::Vertex;'; New = 'return geometry ? SwStage::Geometry : SwStage::Vertex;' },
        @{ Name = 'a non-vertex software stage emulates'; Old = 'return needs_emulation && hw_stage == HwStage::Vertex && sw_stage == SwStage::Vertex;'; New = 'return needs_emulation && hw_stage == HwStage::Vertex;' },
        @{ Name = 'emulation without the NVIDIA need'; Old = 'return needs_emulation && hw_stage == HwStage::Vertex && sw_stage == SwStage::Vertex;'; New = 'return hw_stage == HwStage::Vertex && sw_stage == SwStage::Vertex;' },
        @{ Name = 'user clip planes are ignored by the fragment side'; Old = '(clip_distances || user_clip_planes);'; New = 'clip_distances;' },
        @{ Name = 'the fragment side ignores the clip distance registers'; Old = '(clip_distances || user_clip_planes);'; New = 'true;' },
        @{ Name = 'RuntimeInfo keeps the previous pipeline'; Old = 'memset(this, 0, sizeof(*this));'; New = '' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + $detected + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
        $copy = Copy-ToWorkspace $work $policy 'shader_recompiler/runtime_info.h'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        $mutatedExe = Join-Path $work 'shader_interface_tests.exe'
        $build = { & $clang @flags '-w' '-I' $work @include $test $logSource $assertSource $lostSource $traceSource '-L' (Join-Path $prefix 'lib') '-lfmt' '-o' $mutatedExe }
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "shader interface mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
