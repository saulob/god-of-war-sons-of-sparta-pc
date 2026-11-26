# Headless proof of the buffer arenas on the production arena_layout.h: sparse arenas unchanged on devices that support
# sparse buffers; without sparse binding/residency or a sparse-capable queue, or with SOS_FORCE_NON_SPARSE_ARENAS, fully
# resident arenas instead of a stop (mode and variable, page and block sizes within the driver limits, BDA page table
# size, migration with copy, publish, repoint and retire, stale bindings followed, readback runs, indirect draw bind
# order), the source contracts of the buffer cache and its callers, and mutated copies that must fail.
# No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$video = 'gpu/shadps4/video_core'
$layout = "$video/buffer_cache/arena_layout.h"
$cache = "$video/buffer_cache/buffer_cache.cpp"
$cacheH = "$video/buffer_cache/buffer_cache.h"
$buffer = "$video/buffer_cache/buffer.cpp"
$compat = "$video/renderer_vulkan/vk_gpu_compat.h"
$instanceH = "$video/renderer_vulkan/vk_instance.h"
$rasterizer = "$video/renderer_vulkan/vk_rasterizer.cpp"
$rasterizerH = "$video/renderer_vulkan/vk_rasterizer.h"
$hle = "$video/renderer_vulkan/vk_shader_hle.cpp"
$tile = "$video/texture_cache/tile_manager.cpp"
$texture = "$video/texture_cache/texture_cache.cpp"

$mutations = @(
    @{ Name = 'the forced variable is ignored'; File = $layout; Old = 'if (NonSparseArenasForced(force_env_value)) {'; New = 'if (false) {' },
    @{ Name = '"0" forces fully resident arenas'; File = $layout; Old = 'return env_value && *env_value && std::strcmp(env_value, "0") != 0;'; New = 'return env_value && *env_value;' },
    @{ Name = 'sparse is chosen without queue support'; File = $compat; Old = 'return sparse_binding && sparse_residency_buffer && queue_sparse_binding;'; New = 'return sparse_binding && sparse_residency_buffer;' },
    @{ Name = 'the buffer cache ignores the device sparse support'; File = $cache; Old = 'ArenaLayout::SelectArenaModeFromEnvironment(instance.IsSparseBufferBindingSupported())'; New = 'ArenaLayout::SelectArenaModeFromEnvironment(true)' },
    @{ Name = 'the old stop on missing sparse support comes back'; File = $cache; Old = '    sparse_arenas = !ArenaLayout::IsFullyResident(arena_mode);'; New = "    sparse_arenas = !ArenaLayout::IsFullyResident(arena_mode);`n    if (!sparse_arenas) {`n        UNREACHABLE_MSG(`"GPU: this Vulkan device cannot run the game: sparse buffers are required`");`n    }" },
    @{ Name = 'the resident arena is never repointed'; File = $layout; Old = "        previous = arena;`n        owners[page] = target;"; New = '        previous = arena;' },
    @{ Name = 'a migration copies nothing'; File = $layout; Old = '                ops.Copy(*source, *target, block, chunk_end);'; New = '' },
    @{ Name = 'a stale binding is never followed'; File = $layout; Old = 'if (std::find(replaced.begin(), replaced.end(), arena) == replaced.end()) {'; New = 'if (true) {' },
    @{ Name = 'a readback run is not clipped to its arena'; File = $layout; Old = 'return {arena, start, std::min(end, arena->CpuAddr() + arena->SizeBytes())};'; New = 'return {arena, start, end};' },
    @{ Name = 'resident pages ignore the allocation limit'; File = $layout; Old = 'const u64 limit = ResidentArenaLimit(max_buffer_size, max_allocation_size);'; New = 'const u64 limit = max_buffer_size;' },
    @{ Name = 'resident blocks shrink to 16 KiB (512 MiB BDA table)'; File = $layout; Old = 'const u64 minimum = sparse ? SPARSE_MIN_BLOCK_SIZE : RESIDENT_MIN_BLOCK_SIZE;'; New = 'const u64 minimum = SPARSE_MIN_BLOCK_SIZE;' },
    @{ Name = 'the sparse page sizing changes'; File = $layout; Old = 'while (bits > MIN_SPARSE_PAGE_BITS && (u64{2} << bits) > max_buffer_size) {'; New = 'while (bits > MIN_SPARSE_PAGE_BITS && (u64{1} << bits) > max_buffer_size) {' },
    @{ Name = 'the generation is not bumped'; File = $cache; Old = "    if (retired_arenas.size() != retired) {`n        ++arena_generation;"; New = '    if (retired_arenas.size() != retired) {' },
    @{ Name = 'resident residency allocates sparse backing'; File = $cache; Old = "        PublishArenaBda(arena, {bind_ranges.begin(), bind_ranges.end()});`n        return;"; New = '        PublishArenaBda(arena, {bind_ranges.begin(), bind_ranges.end()});' },
    @{ Name = 'a resident arena skips the low VRAM fallback'; File = $cache; Old = 'std::span<const u32>{backing_memory_types}'; New = 'std::span<const u32>{&arena_memory_type_index, 1}' },
    @{ Name = 'ObtainBuffer does not follow a migration'; File = $cache; Old = 'while (generation != arena_generation) {'; New = 'while (false) {' },
    @{ Name = 'retired arenas are destroyed at once'; File = $cache; Old = 'scheduler.DeferOperation([retired = std::move(retired_arenas)] {'; New = '[retired = std::move(retired_arenas)] {' },
    @{ Name = 'the rasterizer refresh before direct draw binds is removed'; File = $rasterizer; Old = "    RefreshArenaBindings();`n    EmitVertexBuffers();"; New = '    EmitVertexBuffers();' },
    @{ Name = 'the indirect argument buffer is not refreshed'; File = $rasterizer; Old = "    RefreshArenaBindings();`n    buffer_cache.RefreshBinding(buffer, base);`n    if (count_address != 0) {"; New = "    RefreshArenaBindings();`n    if (count_address != 0) {" },
    @{ Name = 'indirect draws emit their binds before the argument lookup'; File = $rasterizer; Old = "    ResolveVertexBuffers(pipeline, nullptr);`n    if (is_indexed) {`n        ResolveIndexBuffer(0, true);`n    }`n`n    const u32 args_size"; New = "    ResolveVertexBuffers(pipeline, nullptr);`n    EmitVertexBuffers();`n    if (is_indexed) {`n        ResolveIndexBuffer(0, true);`n        EmitIndexBuffer();`n    }`n`n    const u32 args_size" },
    @{ Name = 'the refresh always skips'; File = $rasterizer; Old = 'if (buffer_cache.ArenaGeneration() == bind_generation) {'; New = 'if (true) {' },
    @{ Name = 'migrated arenas are never retired'; File = $rasterizer; Old = "    runtime.TickFrame();`n    buffer_cache.RetireMigratedArenas();"; New = '    runtime.TickFrame();' },
    @{ Name = 'the HLE copy source is not refreshed'; File = $hle; Old = '        buffer_cache.RefreshBinding(src_buf, src_buf_offset);'; New = '' },
    @{ Name = 'the detile source is not refreshed'; File = $tile; Old = "    if (buffer_cache) {`n        buffer_cache->RefreshBinding(in_buffer, in_offset);`n    }"; New = '' },
    @{ Name = 'texture uploads detile without the buffer cache'; File = $texture; Old = 'tile_manager.DetileImage(buffer_cache, in_buffer, in_offset, image.info)'; New = 'tile_manager.DetileImage(in_buffer, in_offset, image.info)' }
)
$code = Invoke-HostMutationTest -Name 'buffer_arena' -Root $root -Test 'tests/buffer_arena_tests.cpp' `
    -Copy @($layout, $cache, $cacheH, $buffer, $compat, $instanceH, $rasterizer, $rasterizerH, $hle, $tile, $texture,
        'gpu/shadps4/common/types.h', 'gpu/shadps4/common/alignment.h', 'gpu/shadps4/common/interval_set.h') -Mutations $mutations
exit $code
