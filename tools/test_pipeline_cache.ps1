# Headless proof of the persistent Vulkan pipeline cache on the production headers and sources: the blob seal against
# truncated, zero, bit-flipped and trailing-garbage blobs; serdes bounds; atomic writes that fail; orphan temporary files;
# the bounded IO queue (exact counts, bounded drain, Submit after Close, a throwing task, a full queue); one store per
# game build (1.00 and 1.01) and the take-over of the old single-folder store; game builds ahead of warm-up builds and
# warm-up failures that only skip their entry; plus the wiring (cache_storage.cpp, vk_pipeline_*.cpp, dcpgpu, loader.c)
# and mutated copies that must fail. The test writes only to scratch folders under the system temp folder: never the
# user's cache or the game's files. No window, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$vc = 'gpu/shadps4/video_core'
$vk = "$vc/renderer_vulkan"
$policy = "$vc/pipeline_cache_policy.h"
$files = "$vc/cache_files.h"
$queue = "$vc/cache_request_queue.h"
$storage = "$vc/cache_storage.cpp"
$serdes = 'gpu/shadps4/common/serdes.h'
$inbox = "$vk/vk_warmup_inbox.h"
$cache = "$vk/vk_pipeline_cache.cpp"
$serialization = "$vk/vk_pipeline_serialization.cpp"
$compute = "$vk/vk_compute_pipeline.cpp"
$graphics = "$vk/vk_graphics_pipeline.cpp"
$graphicsH = "$vk/vk_graphics_pipeline.h"
$configH = 'src/dcpgpu.h'
$shim = 'gpu/shim/dcpgpu.cpp'
$loader = 'src/loader.c'

$mutations = @(
    # The bounded IO queue
    @{ Name = 'the drain is unbounded'; File = $queue; Old = 'const bool expired = closed && Clock::now() >= deadline;'; New = 'const bool expired = false;' },
    @{ Name = 'Close is a stop request again (the drain is ignored)'; File = $storage; Old = '    queue->Close(CloseDrainLimit);'; New = '    io_worker.request_stop();' },
    @{ Name = 'the dropped count is lost'; File = $queue; Old = 'dropped += tasks.size();'; New = '(void)0;' },
    @{ Name = 'a refused write is not counted'; File = $queue; Old = '++overflowed;'; New = '(void)0;' },
    @{ Name = 'the queue has no bound'; File = $queue; Old = 'if (tasks.size() >= max_tasks || bytes > max_bytes - queued_bytes) {'; New = 'if (false) {' },
    @{ Name = 'Submit is accepted after Close'; File = $queue; Old = "            if (closed) {`n                return false;`n            }"; New = "            if (false) {`n                return false;`n            }" },
    @{ Name = 'a task that throws ends the IO thread'; File = $queue; Old = '} catch (...) {'; New = '} catch (const std::exception&) {' },
    # One store per game build
    @{ Name = 'the version is missing from the identity'; File = $policy; Old = 'return {.game = GameIdentity(serial, version), .format = format};'; New = 'return {.game = GameIdentity(serial, {}), .format = format};' },
    @{ Name = 'the cache folder ignores the version'; File = $policy; Old = "    if (!version.empty()) {`n        name += '-';`n        name += version;`n    }"; New = '' },
    @{ Name = 'the GPU side never gets the version'; File = $shim; Old = 'info.app_ver = config.version ? config.version : "";'; New = '' },
    @{ Name = 'the loader does not pass the version'; File = $loader; Old = 'render_target.display_height, game_version };'; New = 'render_target.display_height };' },
    @{ Name = 'the legacy store is rebuilt (deleted) instead of kept'; File = $policy; Old = "    if (profile_match && verdict == StoreVerdict::Legacy) {`n        return StoreAction::Adopt;`n    }"; New = '' },
    @{ Name = 'the legacy identity record reads as corrupt'; File = $policy; Old = 'return *format == current.format ? StoreVerdict::Legacy : StoreVerdict::Incompatible;'; New = 'return StoreVerdict::Corrupt;' },
    @{ Name = 'the old folder is taken over although this build has a store'; File = $policy; Old = 'return separate_folder && legacy_has_store && !own_has_store;'; New = 'return separate_folder && legacy_has_store;' },
    @{ Name = 'an unfinished write counts as a store'; File = $files; Old = "        if (it->is_regular_file(entry_error) && IsStoreFile(it->path()) &&`n            !IsTempFile(it->path())) {`n            return true;"; New = "        if (it->is_regular_file(entry_error) && IsStoreFile(it->path())) {`n            return true;" },
    # Game builds ahead of warm-up builds; warm-up failures
    @{ Name = 'the game slot is queued behind warm-up'; File = $policy; Old = 'const BuildGate::Slot slot{gate, BuildGate::Kind::Game};'; New = 'const BuildGate::Slot slot{gate, BuildGate::Kind::Warmup};' },
    @{ Name = 'waiting game builds do not go first'; File = $policy; Old = 'return parallel_builds == 0 && !serial_build && (kind == Kind::Game || games_waiting == 0);'; New = 'return parallel_builds == 0 && !serial_build;' },
    @{ Name = 'a warm-up failure that ran alone turns builds serial'; File = $policy; Old = 'return {true, build_overlapped};'; New = 'return {true, true};' },
    @{ Name = 'SOS_PIPELINE_ON_DEMAND is not read'; File = $serialization; Old = 'std::getenv("SOS_PIPELINE_ON_DEMAND")'; New = 'std::getenv("SOS_PIPELINE_ON_DEMAND_UNUSED")' },
    # Corrupt data
    @{ Name = 'a corrupt blob is accepted'; File = $policy; Old = '        Detail::At<std::uint64_t>(all, payload + 8) != Fnv1a(all.first(payload))) {'; New = '        false) {' },
    @{ Name = 'an oversized blob is accepted'; File = $policy; Old = 'if (bytes.size() < SealSize || bytes.size() - SealSize > MaxBlobBytes) {'; New = 'if (bytes.size() < SealSize) {' },
    @{ Name = 'the store loads a blob without its seal check'; File = $storage; Old = 'if (!PipelineCachePolicy::Unseal(bytes) || bytes.size() % sizeof(T) != 0) {'; New = 'if (bytes.size() % sizeof(T) != 0) {' },
    @{ Name = 'serdes sizes a string from an unchecked length'; File = $serdes; Old = "        Read(length);`n        ar.Require(length);"; New = '        Read(length);' },
    # Files
    @{ Name = 'a temporary file of a running writer is removed'; File = $files; Old = 'if (!entry_error && now - written >= min_age && fs::remove(it->path(), entry_error)) {'; New = 'if (!entry_error && fs::remove(it->path(), entry_error)) {' },
    @{ Name = 'a failed atomic write leaves its temporary file'; File = $files; Old = "    std::error_code error;`n    fs::remove(temp, error);`n    return false;"; New = '    return false;' }
)
$code = Invoke-HostMutationTest -Name 'pipeline_cache' -Root $root -Test 'tests/pipeline_cache_tests.cpp' `
    -Copy @($policy, $files, $queue, $storage, $serdes, 'gpu/shadps4/common/types.h', 'gpu/shadps4/common/polyfill_thread.h', $inbox,
        $cache, $serialization, $compute, $graphics, $graphicsH, $configH, $shim, $loader) -Mutations $mutations
exit $code
