# Headless proof of the first-GPU-operation diagnostics (SOS-BXG9UT, gpu/shim/dcp_first_ops.h): the first shader
# translation, SPIR-V module, graphics and compute pipeline, draw, dispatch and acquire are each logged started and
# finished at most once, concurrent first callers elect one owner, failures keep their VkResult, the crash summary names
# the operation in flight with its shaders, and the production call sites and crash reports are wired. Mutated copies
# must fail. No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$ops = 'gpu/shim/dcp_first_ops.h'
$recompiler = 'gpu/shadps4/shader_recompiler/recompiler.cpp'
$spirv = 'gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv.cpp'
$graphics = 'gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp'
$compute = 'gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp'
$rasterizer = 'gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp'
$swapchain = 'gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp'
$watch = 'gpu/shim/dcp_startup_watch.cpp'
$loader = 'src/loader.c'

$mutations = @(
    @{ Name = 'a later caller also starts the first operation'; File = $ops; Old = "        if (states[op].load(std::memory_order_relaxed) != expected ||`n            !states[op].compare_exchange_strong(expected, int(State::Started), std::memory_order_acq_rel)) {`n            return false;`n        }"; New = '        states[op].store(int(State::Started));' },
    @{ Name = 'a failure is recorded as done'; File = $ops; Old = 'int(result == 0 ? State::Finished : State::Failed)'; New = 'int(State::Finished)' },
    @{ Name = 'Finish prints for an operation that was never started by its owner'; File = $ops; Old = "        int expected = int(State::Started);`n        if (!states[op].compare_exchange_strong(expected, int(result == 0 ? State::Finished : State::Failed),`n                                                std::memory_order_acq_rel)) {`n            return;`n        }"; New = '        states[op].store(int(result == 0 ? State::Finished : State::Failed));' },
    @{ Name = 'the operation in flight loses its shaders'; File = $ops; Old = 'if (state == State::Started && detail_ready[op].load(std::memory_order_acquire)) {'; New = 'if (false) {' },
    @{ Name = 'line breaks of a detail reach the log'; File = $ops; Old = "out[i] = (c == '\n' || c == '\r') ? ' ' : c;"; New = 'out[i] = c;' },
    @{ Name = 'shader translation never finishes'; File = $recompiler; Old = '        first_ops.Finish(DcpFirstOps::ShaderTranslate, 0, "translated");'; New = '' },
    @{ Name = 'a failing first graphics pipeline throws without its result'; File = $graphics; Old = "        finish_first(clean_result);`n        throw PipelineCreationFailed{"; New = '        throw PipelineCreationFailed{' },
    @{ Name = 'the first acquire drops its result'; File = $swapchain; Old = 'first_ops.Finish(DcpFirstOps::Acquire, static_cast<int>(result), vk::to_string(result).c_str());'; New = 'first_ops.Finish(DcpFirstOps::Acquire, 0, "Success");' },
    @{ Name = 'the Linux crash report omits the first operations'; File = $loader; Old = '"Crash: first GPU operations: %s\n"'; New = '"Crash: graphics: %s\n"' }
)
$code = Invoke-HostMutationTest -Name 'first_ops' -Root $root -Test 'tests/first_ops_tests.cpp' `
    -Copy @($ops, $recompiler, $spirv, $graphics, $compute, $rasterizer, $swapchain, $watch, $loader) -Mutations $mutations
exit $code
