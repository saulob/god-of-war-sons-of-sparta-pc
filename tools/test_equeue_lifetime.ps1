# Headless deterministic concurrency and lifetime proof on the real production equeue sources: synchronized registry,
# strong ownership across lookup/delete/trigger/wait/timers, value-captured timer handles, closed-queue semantics,
# waiter wakeup on delete and multi-waiter level-triggered notification, plus source audit and mutations.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_equeue_lifetime_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $gpu = Join-Path $root 'gpu'
    $clang = Join-Path $tc.Bin 'clang++.exe'
    $flags = @(
        '--target=x86_64-w64-mingw32', '-std=gnu++23', '-Wall', '-Wextra', '-O1',
        '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DBOOST_ASIO_STANDALONE', '-DDCP_EQUEUE_TEST_HOOKS',
        '-Wno-deprecated-declarations', '-Wno-missing-field-initializers', '-Wno-sign-compare',
        '-include', (Join-Path $gpu 'shim/dcp_win_compat.h')
    )

    $equeueCpp = 'gpu/shadps4/core/libraries/kernel/equeue.cpp'
    $equeueH = 'gpu/shadps4/core/libraries/kernel/equeue.h'
    $gnmCpp = 'gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp'
    $gnmEqH = 'gpu/shadps4/core/libraries/gnmdriver/gnm_eq_event.h'
    $voCpp = 'gpu/shadps4/core/libraries/videoout/video_out.cpp'
    $drvH = 'gpu/shadps4/core/libraries/videoout/driver.h'
    $drvCpp = 'gpu/shadps4/core/libraries/videoout/driver.cpp'
    $platH = 'gpu/shadps4/core/platform.h'

    $copyFiles = @(
        $equeueCpp,
        $equeueH,
        $gnmCpp,
        $gnmEqH,
        $voCpp,
        $drvH,
        $drvCpp,
        $platH,
        'gpu/shadps4/core/libraries/videoout/video_out.h',
        'gpu/shadps4/core/libraries/videoout/buffer.h',
        'gpu/shadps4/core/libraries/system/userservice.h',
        'gpu/shadps4/core/libraries/kernel/orbis_error.h',
        'gpu/shadps4/core/libraries/kernel/posix_error.h',
        'gpu/shadps4/core/libraries/kernel/time.h',
        'gpu/shadps4/core/libraries/error_codes.h',
        'gpu/shadps4/common/arch.h',
        'gpu/shadps4/common/types.h',
        'gpu/shadps4/common/rdtsc.h',
        'gpu/shadps4/common/singleton.h',
        'gpu/shadps4/common/polyfill_thread.h',
        'gpu/shim/core/libraries/kernel/kernel.h',
        'gpu/shim/core/libraries/libs.h',
        'gpu/shim/dcp_present.h'
    )

    $assertStub = @'
#pragma once
#include <cstdlib>
#define ASSERT(a) do { if (!(a)) std::abort(); } while (0)
#define ASSERT_MSG(a, ...) do { if (!(a)) std::abort(); } while (0)
#define UNREACHABLE() std::abort()
#define UNREACHABLE_MSG(...) std::abort()
'@

    $logStub = @'
#pragma once
#include <string>
#include <string_view>
namespace spdlog {
enum class level : int { trace, debug, info, warn, err, critical, off };
}
namespace Common::Log {
using Level = spdlog::level;
bool ShouldLog(Level level);
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message);
}
#define LOG_TRACE(log_class, ...) do {} while (false)
#define LOG_DEBUG(log_class, ...) do {} while (false)
#define LOG_INFO(log_class, ...) do {} while (false)
#define LOG_WARNING(log_class, ...) do {} while (false)
#define LOG_ERROR(log_class, ...) do {} while (false)
#define LOG_CRITICAL(log_class, ...) do {} while (false)
'@

    $debugStub = @'
#pragma once
#define HLE_TRACE do {} while (false)
#define TRACE_HINT(...) do {} while (false)
'@

    $newWorkspace = {
        param([string]$Dir)
        foreach ($relative in $copyFiles) {
            Copy-ToWorkspace $Dir (Join-Path $root $relative) $relative | Out-Null
        }
        $aPath = Join-Path $Dir 'stub/common/assert.h'
        New-Item -ItemType Directory -Force -Path (Split-Path $aPath -Parent) | Out-Null
        [IO.File]::WriteAllText($aPath, $assertStub)
        $lPath = Join-Path $Dir 'stub/common/logging/log.h'
        New-Item -ItemType Directory -Force -Path (Split-Path $lPath -Parent) | Out-Null
        [IO.File]::WriteAllText($lPath, $logStub)
        $dPath = Join-Path $Dir 'stub/common/debug.h'
        New-Item -ItemType Directory -Force -Path (Split-Path $dPath -Parent) | Out-Null
        [IO.File]::WriteAllText($dPath, $debugStub)
    }

    $compile = {
        param([string]$Dir, [string]$Exe, [string[]]$Extra)
        $includes = @(
            '-I', (Join-Path $Dir 'stub'),
            '-I', (Join-Path $Dir 'gpu/shim'),
            '-I', (Join-Path $Dir 'gpu/shadps4'),
            '-I', (Join-Path $gpu 'third_party/magic_enum/include'),
            '-I', (Join-Path $prefix 'include')
        )
        $rootText = $Dir.Replace('\', '/')
        $inputs = @(
            (Join-Path $root 'tests/equeue_lifetime_tests.cpp'),
            (Join-Path $Dir $equeueCpp)
        )
        & $clang @flags @Extra "-DSOS_SOURCE_DIR=$rootText" @includes @inputs '-lws2_32' '-o' $Exe
    }

    $mutations = @(
        @{
            Name = 'unsynchronized registry lookup without shared_lock'
            File = $equeueCpp
            Old = "    std::shared_ptr<EqueueInternal> Find(OrbisKernelEqueue handle) const {`n        std::shared_lock lock{mutex};"
            New = "    std::shared_ptr<EqueueInternal> Find(OrbisKernelEqueue handle) const {"
        },
        @{
            Name = 'contains plus operator[] lookup reintroduced in registry'
            File = $equeueCpp
            Old = "        const auto it = queues.find(handle);`n        return it == queues.end() ? nullptr : it->second;"
            New = "        if (!queues.contains(handle)) { return nullptr; }`n        return const_cast<std::unordered_map<OrbisKernelEqueue, std::shared_ptr<EqueueInternal>>&>(queues)[handle];"
        },
        @{
            Name = 'raw-pointer delete while another operation still owns the queue'
            File = $equeueCpp
            Old = "    removed->Close();`n    return ORBIS_OK;"
            New = "    removed->Close();`n    delete removed.get();`n    return ORBIS_OK;"
        },
        @{
            Name = 'timer callback captures raw this instead of handle by value'
            File = $equeueCpp
            Old = "        [handle = m_handle, event_data = event.event,`n         callback](const boost::system::error_code& ec) {"
            New = "        [this, event_data = event.event,`n         callback](const boost::system::error_code& ec) {"
            Old2 = "            callback(handle, event_data);"
            New2 = "            callback(this->m_handle, event_data);"
        },
        @{
            Name = 'callback drops registry shared_ptr ownership before calling queue methods'
            File = $equeueCpp
            Old = "    const auto equeue = kqueues.Find(eq);`n#ifdef DCP_EQUEUE_TEST_HOOKS`n    if (equeue) {`n        EqueueTestHooks::OnTimerCallbackAcquired(eq);`n    }`n#endif"
            New = "    auto* const equeue = kqueues.Find(eq).get();`n#ifdef DCP_EQUEUE_TEST_HOOKS`n    if (equeue) {`n        EqueueTestHooks::OnTimerCallbackAcquired(eq);`n    }`n#endif"
        },
        @{
            Name = 'Find(...).get() temporary-owner bug in public lookup helper'
            File = $equeueCpp
            Old = "std::shared_ptr<EqueueInternal> AcquireEqueue(OrbisKernelEqueue eq) {`n    auto found = kqueues.Find(eq);"
            New = "static EqueueInternal* UnsafeGetEqueue(OrbisKernelEqueue eq) { return kqueues.Find(eq).get(); }`nstd::shared_ptr<EqueueInternal> AcquireEqueue(OrbisKernelEqueue eq) {`n    (void)&UnsafeGetEqueue;`n    auto found = kqueues.Find(eq);"
        },
        @{
            Name = 'periodic timer callback reschedules after delete'
            File = $equeueCpp
            Old = "        m_closed = true;`n        removed_events = std::move(m_events);`n        m_events.clear();"
            New = "        m_closed = true;"
            Old2 = "bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,`n                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {`n    std::scoped_lock lock{m_mutex};`n    if (m_closed) {`n        return false;`n    }"
            New2 = "bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,`n                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {`n    std::scoped_lock lock{m_mutex};"
        },
        @{
            Name = 'delete does not notify sleeping waiters on Close'
            File = $equeueCpp
            Old = "        m_small_timers.clear();`n    }`n    m_cond.notify_all();`n}"
            New = "        m_small_timers.clear();`n    }`n}"
        },
        @{
            Name = 'closed queue still accepts a new timer event'
            File = $equeueCpp
            Old = "    {`n        std::scoped_lock lock{m_mutex};`n        if (m_closed) {`n            return false;`n        }`n`n        // Calculate timer interval"
            New = "    {`n        std::scoped_lock lock{m_mutex};`n`n        // Calculate timer interval"
            Old2 = "bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,`n                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {`n    std::scoped_lock lock{m_mutex};`n    if (m_closed) {`n        return false;`n    }"
            New2 = "bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,`n                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {`n    std::scoped_lock lock{m_mutex};"
        },
        @{
            Name = 'closed queue still accepts a new small HR timer'
            File = $equeueCpp
            Old = "    {`n        std::scoped_lock lock{m_mutex};`n        if (m_closed) {`n            return false;`n        }`n        m_small_timers[st.event.ident] = std::move(st);"
            New = "    {`n        std::scoped_lock lock{m_mutex};`n        m_small_timers[st.event.ident] = std::move(st);"
        },
        @{
            Name = 'notify_one strands a second waiter on level-triggered user event'
            File = $equeueCpp
            Old = "    if (wake) {`n        m_cond.notify_all();`n    }"
            New = "    if (wake) {`n        m_cond.notify_one();`n    }"
        },
        @{
            Name = 'GPU completion interrupts coalesce into one sticky event'
            File = $equeueCpp
            Old = "                } else if (filter == OrbisKernelEvent::Filter::GraphicsCore) {`n                    event.TriggerCompletion(trigger_data);`n                } else {"
            New = "                } else {"
        },
        @{
            Name = 'GnmDriver IRQ handler captures raw pointer via [=] instead of handle eq'
            File = $gnmEqH
            Old = "        [eq, id](Platform::InterruptId irq) -> bool {"
            New = "        [=](Platform::InterruptId irq) -> bool {"
        },
        @{
            Name = 'zero-timeout poll drops m_mutex lock'
            File = $equeueCpp
            Old = "    if (timo != nullptr && *timo == 0) {`n        // Effectively acts as a poll; only events that have already`n        // arrived at the time of this function call can be received`n        std::scoped_lock lock{m_mutex};"
            New = "    if (timo != nullptr && *timo == 0) {"
        },
        @{
            Name = 'stale flip handle not removed after failed AcquireEqueue'
            File = $drvH
            Old = "        std::erase_if(flip_events, [flip_arg](Kernel::OrbisKernelEqueue eq) {`n            const auto equeue = Kernel::AcquireEqueue(eq);`n            if (!equeue) {`n                return true;`n            }"
            New = "        std::erase_if(flip_events, [flip_arg](Kernel::OrbisKernelEqueue eq) {`n            const auto equeue = Kernel::AcquireEqueue(eq);`n            if (!equeue) {`n                return false;`n            }"
        },
        @{
            Name = 'stale vblank handle not removed after failed AcquireEqueue'
            File = $drvH
            Old = "        std::erase_if(vblank_events, [vblank_count](Kernel::OrbisKernelEqueue eq) {`n            const auto equeue = Kernel::AcquireEqueue(eq);`n            if (!equeue) {`n                return true;`n            }"
            New = "        std::erase_if(vblank_events, [vblank_count](Kernel::OrbisKernelEqueue eq) {`n            const auto equeue = Kernel::AcquireEqueue(eq);`n            if (!equeue) {`n                return false;`n            }"
        },
        @{
            Name = 'dead IRQ registration remains active after failed AcquireEqueue'
            File = $gnmEqH
            Old = "            const auto target = Kernel::AcquireEqueue(eq);`n            if (!target) {`n                return false;`n            }"
            New = "            const auto target = Kernel::AcquireEqueue(eq);`n            if (!target) {`n                return true;`n            }"
        },
        @{
            Name = 'IrqController::Signal ignores false return and retains dead handlers'
            File = $platH
            Old = "            if (!it->second(irq)) {`n                it = ctx.persistent_handlers.erase(it);`n            } else {"
            New = "            if (!it->second(irq)) {`n                ++it;`n            } else {"
        },
        @{
            Name = 'unsafe iterator erase inside VideoOut flip_events range-for loop'
            File = $drvH
            Old = "    void TriggerFlipEventsLocked(s64 flip_arg) {`n        std::erase_if(flip_events, [flip_arg](Kernel::OrbisKernelEqueue eq) {`n            const auto equeue = Kernel::AcquireEqueue(eq);`n            if (!equeue) {`n                return true;`n            }`n            equeue->TriggerEvent(`n                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),`n                Kernel::OrbisKernelEvent::Filter::VideoOut,`n                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |`n                                        (flip_arg << 16)));`n            return equeue->IsClosed();`n        });`n    }"
            New = "    void TriggerFlipEventsLocked(s64 flip_arg) {`n        for (auto eq : flip_events) {`n            const auto equeue = Kernel::AcquireEqueue(eq);`n            if (!equeue) {`n                flip_events.erase(std::ranges::find(flip_events, eq));`n                continue;`n            }`n            equeue->TriggerEvent(`n                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),`n                Kernel::OrbisKernelEvent::Filter::VideoOut,`n                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |`n                                        (flip_arg << 16)));`n        }`n    }"
        },
        @{
            Name = 'automatic flip cleanup removes a live subscription'
            File = $drvH
            Old = "            equeue->TriggerEvent(`n                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),`n                Kernel::OrbisKernelEvent::Filter::VideoOut,`n                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |`n                                        (flip_arg << 16)));`n            return equeue->IsClosed();"
            New = "            equeue->TriggerEvent(`n                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),`n                Kernel::OrbisKernelEvent::Filter::VideoOut,`n                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |`n                                        (flip_arg << 16)));`n            return true;"
        },
        @{
            Name = 'unguarded double removal in VideoOutPort::RemoveFlipEvent'
            File = $drvH
            Old = "    void RemoveFlipEvent(Kernel::OrbisKernelEqueue eq) {`n        std::scoped_lock lock{port_mutex};`n        if (const auto it = std::ranges::find(flip_events, eq); it != flip_events.end()) {`n            flip_events.erase(it);`n        }`n    }"
            New = "    void RemoveFlipEvent(Kernel::OrbisKernelEqueue eq) {`n        std::scoped_lock lock{port_mutex};`n        flip_events.erase(std::ranges::find(flip_events, eq));`n    }"
        }
    )

    $baseline = Join-Path $temp 'base'
    & $newWorkspace $baseline
    $exe = Join-Path $baseline 'test.exe'
    & $compile $baseline $exe @()
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the equeue-lifetime test' }
    $run = Invoke-TimedProgram $exe
    $baseText = $run.Text.TrimEnd()
    Write-Host $baseText
    $result = $run.Code
    $checkCount = 0
    if ($baseText -match 'equeue-lifetime:\s*(\d+)\s*checks') { $checkCount = [int]$Matches[1] }

    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        & $newWorkspace $work
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        if ($mutation.ContainsKey('Old2')) {
            Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old2 $mutation.New2
        }
        $mutatedExe = Join-Path $work 'test.exe'
        $build = { & $compile $work $mutatedExe @('-w') }.GetNewClosure()
        if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
    }
    Write-Host "equeue-lifetime: $checkCount baseline passed, $detected of $($mutations.Count) mutations detected"
    exit $result
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
