// Deterministic concurrency and lifetime tests on the production kernel event queue (equeue.cpp / equeue.h).
// Compiles the real production equeue implementation with a controlled boost::asio::io_context and zero-overhead
// test hooks to force critical interleavings deterministically, plus structural checks on the equeue callers.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "core/libraries/gnmdriver/gnm_eq_event.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char*, const char*, const char*, SymbolType, u64) {}
} // namespace Core::Loader

namespace Libraries::Kernel {

boost::asio::io_context io_context;

static std::atomic<int> g_kernel_signals{0};

void KernelSignalRequest() {
    g_kernel_signals.fetch_add(1, std::memory_order_relaxed);
}

static thread_local int t_errno = 0;
int* PS4_SYSV_ABI __Error() {
    return &t_errno;
}

s32 PS4_SYSV_ABI posix_kqueue();
s32 PS4_SYSV_ABI posix_kevent(s32 handle, OrbisKernelEvent* changelist, u64 nchanges,
                              OrbisKernelEvent* eventlist, u64 nevents,
                              OrbisKernelTimespec* timeout);
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo);
s32 PS4_SYSV_ABI sceKernelAddHRTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelTimespec* ts,
                                          void* udata);
int PS4_SYSV_ABI sceKernelDeleteHRTimerEvent(OrbisKernelEqueue eq, int id);
int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelUseconds usec,
                                        void* udata);
int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id);
int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id);
int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue eq, int id);
int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata);
int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id);

namespace EqueueTestHooks {

struct PausePoint {
    std::mutex mutex;
    std::condition_variable cv;
    bool armed = false;
    bool paused = false;
    bool released = false;
    int hits = 0;

    void Arm() {
        std::scoped_lock lock{mutex};
        armed = true;
        paused = false;
        released = false;
        hits = 0;
    }

    void Disarm() {
        std::scoped_lock lock{mutex};
        armed = false;
        released = true;
        cv.notify_all();
    }

    void Hit() {
        std::unique_lock lock{mutex};
        ++hits;
        if (!armed) {
            return;
        }
        armed = false;
        paused = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }

    bool WaitUntilPaused(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, timeout, [this] { return paused; });
    }

    void Release() {
        std::scoped_lock lock{mutex};
        released = true;
        cv.notify_all();
    }
};

static std::mutex g_state_mutex;
static std::condition_variable g_state_cv;
static std::unordered_set<OrbisKernelEqueue> g_destroyed;
static std::vector<OrbisKernelEqueue> g_timer_entered;
static int g_timer_acquired_count = 0;
static std::unordered_map<OrbisKernelEqueue, int> g_acquire_total;
static std::unordered_map<OrbisKernelEqueue, int> g_acquire_miss;
static std::atomic<OrbisKernelEqueue> g_acquire_pause_target{-1};

static PausePoint g_before_timer_cb;
static PausePoint g_timer_acquired_cb;
static PausePoint g_before_reschedule_cb;
static PausePoint g_trigger_acquired_cb;
static PausePoint g_acquire_lookup_cb;

static void ResetHooks() {
    g_before_timer_cb.Disarm();
    g_timer_acquired_cb.Disarm();
    g_before_reschedule_cb.Disarm();
    g_trigger_acquired_cb.Disarm();
    g_acquire_lookup_cb.Disarm();
    g_acquire_pause_target.store(-1, std::memory_order_release);
    std::scoped_lock lock{g_state_mutex};
    g_timer_entered.clear();
    g_timer_acquired_count = 0;
    g_acquire_total.clear();
    g_acquire_miss.clear();
}

static void ArmAcquirePause(OrbisKernelEqueue target) {
    g_acquire_lookup_cb.Arm();
    g_acquire_pause_target.store(target, std::memory_order_release);
}

static int AcquireLookupCount(OrbisKernelEqueue handle) {
    std::scoped_lock lock{g_state_mutex};
    const auto it = g_acquire_total.find(handle);
    return it == g_acquire_total.end() ? 0 : it->second;
}

static int AcquireMissCount(OrbisKernelEqueue handle) {
    std::scoped_lock lock{g_state_mutex};
    const auto it = g_acquire_miss.find(handle);
    return it == g_acquire_miss.end() ? 0 : it->second;
}

static bool WasDestroyed(OrbisKernelEqueue handle) {
    std::scoped_lock lock{g_state_mutex};
    return g_destroyed.contains(handle);
}

static bool WaitDestroyed(OrbisKernelEqueue handle,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    std::unique_lock lock{g_state_mutex};
    return g_state_cv.wait_for(lock, timeout, [handle] { return g_destroyed.contains(handle); });
}

static bool WaitTimerEnterCount(std::size_t expected,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    std::unique_lock lock{g_state_mutex};
    return g_state_cv.wait_for(lock, timeout, [expected] { return g_timer_entered.size() >= expected; });
}

void OnQueueDestroyed(OrbisKernelEqueue handle) {
    {
        std::scoped_lock lock{g_state_mutex};
        g_destroyed.insert(handle);
    }
    g_state_cv.notify_all();
}

void OnBeforeTimerCallback() {
    g_before_timer_cb.Hit();
}

void OnTimerCallbackEnter(OrbisKernelEqueue handle) {
    {
        std::scoped_lock lock{g_state_mutex};
        g_timer_entered.push_back(handle);
    }
    g_state_cv.notify_all();
}

void OnTimerCallbackAcquired(OrbisKernelEqueue) {
    {
        std::scoped_lock lock{g_state_mutex};
        ++g_timer_acquired_count;
    }
    g_state_cv.notify_all();
    g_timer_acquired_cb.Hit();
}

void OnBeforePeriodicReschedule(OrbisKernelEqueue) {
    g_before_reschedule_cb.Hit();
}

void OnTriggerAcquired(OrbisKernelEqueue) {
    g_trigger_acquired_cb.Hit();
}

void OnAcquireLookup(OrbisKernelEqueue handle, bool found) {
    {
        std::scoped_lock lock{g_state_mutex};
        ++g_acquire_total[handle];
        if (!found) {
            ++g_acquire_miss[handle];
        }
    }
    if (found && g_acquire_pause_target.load(std::memory_order_acquire) == handle) {
        g_acquire_pause_target.store(-1, std::memory_order_release);
        g_acquire_lookup_cb.Hit();
    }
}

} // namespace EqueueTestHooks

} // namespace Libraries::Kernel

namespace {

using namespace Libraries::Kernel;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

struct SyncSignal {
    std::mutex mutex;
    std::condition_variable cv;
    bool signaled = false;

    void Notify() {
        std::scoped_lock lock{mutex};
        signaled = true;
        cv.notify_all();
    }

    bool Wait(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, timeout, [this] { return signaled; });
    }
};

struct PhaseBarrier {
    std::mutex mutex;
    std::condition_variable cv;
    int threshold;
    int count = 0;
    int generation = 0;

    explicit PhaseBarrier(int n) : threshold(n) {}

    void ArriveAndWait() {
        std::unique_lock lock{mutex};
        const int gen = generation;
        if (++count == threshold) {
            count = 0;
            ++generation;
            cv.notify_all();
        } else {
            cv.wait(lock, [this, gen] { return generation != gen; });
        }
    }
};

bool WaitForWaiters(const std::shared_ptr<EqueueInternal>& queue, int expected,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (queue->WaiterCount() < expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

class AsioServiceGuard {
public:
    AsioServiceGuard()
        : work_guard(boost::asio::make_work_guard(io_context)),
          worker([] { io_context.run(); }) {}

    ~AsioServiceGuard() {
        work_guard.reset();
        io_context.stop();
        if (worker.joinable()) {
            worker.join();
        }
        io_context.restart();
    }

private:
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
    std::thread worker;
};

#define SOS_STR_IMPL(x) #x
#define SOS_STR(x) SOS_STR_IMPL(x)

std::string ReadSource(const std::string& relative) {
    const std::string path = std::string(SOS_STR(SOS_SOURCE_DIR)) + "/" + relative;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string StripCommentsAndNormalize(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
                out.push_back(' ');
            }
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                ++i;
                out.push_back(' ');
            }
            continue;
        }
        if (in_string) {
            out.push_back(c);
            if (c == '\\' && i + 1 < src.size()) {
                out.push_back(src[++i]);
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
        } else if (c == '/' && next == '/') {
            in_line_comment = true;
            ++i;
        } else if (c == '/' && next == '*') {
            in_block_comment = true;
            ++i;
        } else if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!prev_space) {
                collapsed.push_back(' ');
                prev_space = true;
            }
        } else {
            collapsed.push_back(c);
            prev_space = false;
        }
    }
    return collapsed;
}

// TEST 1: Lookup vs Delete
void TestLookupVsDelete() {
    EqueueTestHooks::ResetHooks();
    OrbisKernelEqueue eq = 0;
    Check(sceKernelCreateEqueue(&eq, "lookup_vs_del") == ORBIS_OK, "T1: create equeue succeeds");
    Check(sceKernelAddUserEventEdge(eq, 1) == ORBIS_OK, "T1: add user event succeeds");

    SyncSignal acquired;
    SyncSignal allow_use;
    SyncSignal finished;
    bool safe_name = false;
    bool closed_seen = false;
    bool trigger_rejected = false;
    bool add_rejected = false;
    bool add_small_rejected = false;
    bool schedule_rejected = false;
    bool poll_closed = false;
    bool wait_closed = false;
    bool small_wait_closed = false;

    std::thread reader([&] {
        auto owned = AcquireEqueue(eq);
        acquired.Notify();
        if (!allow_use.Wait()) {
            return;
        }
        if (owned) {
            safe_name = owned->GetName() == "lookup_vs_del";
            closed_seen = owned->IsClosed();
            trigger_rejected = !owned->TriggerEvent(1, OrbisKernelEvent::Filter::User, nullptr);

            EqueueEvent timer_ev{};
            timer_ev.event.ident = 42;
            timer_ev.event.filter = OrbisKernelEvent::Filter::Timer;
            timer_ev.event.flags = OrbisKernelEvent::Flags::Add;
            timer_ev.event.data = 5;
            add_rejected = !owned->AddEvent(timer_ev);

            OrbisKernelBintime bt{0, 100000 * 0x44b82fa09LL};
            EqueueEvent hr_ev{};
            hr_ev.event.ident = 43;
            hr_ev.event.filter = OrbisKernelEvent::Filter::HrTimer;
            hr_ev.event.data = reinterpret_cast<u64>(&bt);
            add_small_rejected = !owned->AddSmallTimer(hr_ev);

            schedule_rejected = !owned->ScheduleEvent(42, OrbisKernelEvent::Filter::Timer,
                                                      [](OrbisKernelEqueue, const OrbisKernelEvent&) {});

            OrbisKernelEvent out_ev{};
            OrbisKernelUseconds zero = 0;
            poll_closed = owned->WaitForEvents(&out_ev, 1, &zero) == -1;
            wait_closed = owned->WaitForEvents(&out_ev, 1, nullptr) == -1;
            small_wait_closed = owned->WaitForSmallTimer(&out_ev, 1, 0) == -1;
        }
        owned.reset();
        finished.Notify();
    });

    Check(acquired.Wait(), "T1: reader acquired queue ownership");
    const int signals_before = g_kernel_signals.load(std::memory_order_relaxed);
    Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T1: delete succeeds while reader owns queue");
    Check(sceKernelDeleteEqueue(eq) == ORBIS_KERNEL_ERROR_EBADF, "T1: second delete returns EBADF");
    Check(AcquireEqueue(eq) == nullptr, "T1: lookup after delete returns nullptr");
    Check(!EqueueTestHooks::WasDestroyed(eq), "T1: queue stays alive while reader holds ownership");

    allow_use.Notify();
    Check(finished.Wait(), "T1: reader completed operations safely");
    reader.join();

    Check(safe_name, "T1: reader observed intact queue name after delete");
    Check(closed_seen, "T1: reader observed closed state");
    Check(trigger_rejected, "T1: closed queue rejects TriggerEvent");
    Check(add_rejected, "T1: closed queue rejects AddEvent timer");
    Check(add_small_rejected, "T1: closed queue rejects AddSmallTimer");
    Check(schedule_rejected, "T1: closed queue rejects ScheduleEvent");
    Check(poll_closed, "T1: closed queue zero-timeout poll returns -1");
    Check(wait_closed, "T1: closed queue infinite wait returns -1 immediately");
    Check(small_wait_closed, "T1: closed queue WaitForSmallTimer returns -1 immediately");
    Check(g_kernel_signals.load(std::memory_order_relaxed) == signals_before,
          "T1: closed queue never requests asio reschedule");
    Check(EqueueTestHooks::WaitDestroyed(eq), "T1: queue destroyed only after reader released ownership");
}

// TEST 2: Delete vs Timer Callback (before registry reacquire)
void TestDeleteVsTimerCallback() {
    EqueueTestHooks::ResetHooks();
    OrbisKernelEqueue eq = 0;
    Check(sceKernelCreateEqueue(&eq, "del_vs_timer") == ORBIS_OK, "T2: create equeue succeeds");

    EqueueTestHooks::g_before_timer_cb.Arm();
    Check(sceKernelAddTimerEvent(eq, 7, 1000, reinterpret_cast<void*>(0x1234)) == ORBIS_OK,
          "T2: add periodic timer succeeds");

    Check(EqueueTestHooks::g_before_timer_cb.WaitUntilPaused(), "T2: timer callback fired and paused before reacquire");
    const int signals_before = g_kernel_signals.load(std::memory_order_relaxed);

    Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T2: delete equeue while timer callback paused");
    Check(AcquireEqueue(eq) == nullptr, "T2: handle removed from registry");
    Check(EqueueTestHooks::WaitDestroyed(eq), "T2: queue destroyed before callback reacquire");

    EqueueTestHooks::g_before_timer_cb.Release();
    Check(EqueueTestHooks::WaitTimerEnterCount(1), "T2: timer callback resumed and ran cleanly");

    OrbisKernelEqueue observed_handle = -2;
    int acquired_count = -1;
    {
        std::scoped_lock lock{EqueueTestHooks::g_state_mutex};
        if (!EqueueTestHooks::g_timer_entered.empty()) {
            observed_handle = EqueueTestHooks::g_timer_entered.front();
        }
        acquired_count = EqueueTestHooks::g_timer_acquired_count;
    }
    Check(observed_handle == eq, "T2: timer callback passed value-captured handle, not stale this->m_handle");
    Check(acquired_count == 0, "T2: timer callback observed missing queue in registry");
    Check(g_kernel_signals.load(std::memory_order_relaxed) == signals_before,
          "T2: timer callback did not reschedule after delete");
}

// TEST 3: Callback owns queue while delete occurs (both before Trigger and before Reschedule)
void TestCallbackOwnsQueueDuringDelete() {
    // 3A: Delete while callback holds strong reference right after Find
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "cb_owns_3a") == ORBIS_OK, "T3A: create equeue succeeds");

        EqueueTestHooks::g_timer_acquired_cb.Arm();
        Check(sceKernelAddTimerEvent(eq, 8, 1000, nullptr) == ORBIS_OK, "T3A: add periodic timer succeeds");
        Check(EqueueTestHooks::g_timer_acquired_cb.WaitUntilPaused(), "T3A: callback acquired queue and paused");

        const int signals_before = g_kernel_signals.load(std::memory_order_relaxed);
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T3A: delete succeeds while callback holds owner");
        Check(AcquireEqueue(eq) == nullptr, "T3A: deleted handle cannot be reacquired");
        Check(!EqueueTestHooks::WasDestroyed(eq), "T3A: queue stays alive while callback holds owner");

        EqueueTestHooks::g_timer_acquired_cb.Release();
        Check(EqueueTestHooks::WaitDestroyed(eq), "T3A: queue destroyed after callback released owner");
        Check(g_kernel_signals.load(std::memory_order_relaxed) == signals_before,
              "T3A: callback did not reschedule after delete");
    }

    // 3B: Delete while periodic callback holds strong reference right before ScheduleEvent
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "cb_owns_3b") == ORBIS_OK, "T3B: create equeue succeeds");

        EqueueTestHooks::g_before_reschedule_cb.Arm();
        Check(sceKernelAddTimerEvent(eq, 9, 1000, nullptr) == ORBIS_OK, "T3B: add periodic timer succeeds");
        Check(EqueueTestHooks::g_before_reschedule_cb.WaitUntilPaused(),
              "T3B: periodic callback paused before reschedule");

        const int signals_before = g_kernel_signals.load(std::memory_order_relaxed);
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T3B: delete succeeds before periodic reschedule");
        Check(!EqueueTestHooks::WasDestroyed(eq), "T3B: queue stays alive until callback finishes");

        EqueueTestHooks::g_before_reschedule_cb.Release();
        Check(EqueueTestHooks::WaitDestroyed(eq), "T3B: queue destroyed after callback finishes");
        Check(g_kernel_signals.load(std::memory_order_relaxed) == signals_before,
              "T3B: periodic timer refused to reschedule closed queue");
    }
}

// TEST 4: Wait vs Delete (infinite wait, timed wait, posix_kevent, small timer)
void TestWaitVsDelete() {
    // 4A: Infinite wait
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "wait_inf") == ORBIS_OK, "T4A: create equeue");
        Check(sceKernelAddUserEventEdge(eq, 10) == ORBIS_OK, "T4A: add event");

        auto probe = AcquireEqueue(eq);
        int wait_ret = 0;
        int out_num = -1;
        SyncSignal done;
        std::thread waiter([&] {
            OrbisKernelEvent ev{};
            wait_ret = sceKernelWaitEqueue(eq, &ev, 1, &out_num, nullptr);
            done.Notify();
        });

        Check(WaitForWaiters(probe, 1), "T4A: waiter proved sleeping in WaitForEvents");
        probe.reset();

        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T4A: delete while waiter sleeping");
        const bool woke = done.Wait(std::chrono::milliseconds(1500));
        Check(woke, "T4A: infinite waiter woke deterministically on delete");
        if (!woke) {
            std::printf("equeue-lifetime: %d checks, %d failed\n", g_checks, g_failures);
            std::_Exit(1);
        }
        waiter.join();

        Check(wait_ret == ORBIS_KERNEL_ERROR_EBADF, "T4A: infinite waiter returns EBADF");
        Check(out_num == 0, "T4A: infinite waiter sets out = 0");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T4A: queue destroyed after waiter exited");
    }

    // 4B: Timed wait (long 10 s timeout must wake immediately on delete)
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "wait_timed") == ORBIS_OK, "T4B: create equeue");
        Check(sceKernelAddUserEventEdge(eq, 11) == ORBIS_OK, "T4B: add event");

        auto probe = AcquireEqueue(eq);
        int wait_ret = 0;
        int out_num = -1;
        SyncSignal done;
        std::thread waiter([&] {
            OrbisKernelEvent ev{};
            OrbisKernelUseconds timo = 10'000'000u;
            wait_ret = sceKernelWaitEqueue(eq, &ev, 1, &out_num, &timo);
            done.Notify();
        });

        Check(WaitForWaiters(probe, 1), "T4B: timed waiter proved sleeping");
        probe.reset();

        const auto start = std::chrono::steady_clock::now();
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T4B: delete while timed waiter sleeping");
        Check(done.Wait(), "T4B: timed waiter woke on delete");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        waiter.join();

        Check(elapsed < std::chrono::seconds(2), "T4B: timed waiter woke immediately, not via 10 s timeout");
        Check(wait_ret == ORBIS_KERNEL_ERROR_EBADF, "T4B: timed waiter returns EBADF");
        Check(out_num == 0, "T4B: timed waiter sets out = 0");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T4B: queue destroyed after timed waiter exited");
    }

    // 4C: posix_kqueue + posix_kevent wait vs delete
    {
        EqueueTestHooks::ResetHooks();
        const s32 kq = posix_kqueue();
        Check(kq >= 0x40000, "T4C: posix_kqueue created handle");

        auto probe = AcquireEqueue(kq);
        s32 kevent_ret = 0;
        int kevent_err = 0;
        SyncSignal done;
        std::thread waiter([&] {
            OrbisKernelEvent ev{};
            *__Error() = 0;
            kevent_ret = posix_kevent(kq, nullptr, 0, &ev, 1, nullptr);
            kevent_err = *__Error();
            done.Notify();
        });

        Check(WaitForWaiters(probe, 1), "T4C: posix_kevent waiter proved sleeping");
        probe.reset();

        Check(sceKernelDeleteEqueue(kq) == ORBIS_OK, "T4C: delete posix kqueue while waiting");
        const bool woke = done.Wait(std::chrono::milliseconds(1500));
        Check(woke, "T4C: posix_kevent woke on delete");
        if (!woke) {
            std::printf("equeue-lifetime: %d checks, %d failed\n", g_checks, g_failures);
            std::_Exit(1);
        }
        waiter.join();

        Check(kevent_ret == ORBIS_FAIL, "T4C: posix_kevent returns ORBIS_FAIL on deleted queue");
        Check(kevent_err == POSIX_EBADF, "T4C: posix_kevent sets POSIX_EBADF");
        Check(EqueueTestHooks::WaitDestroyed(kq), "T4C: posix kqueue destroyed after waiter exited");
    }
}

// TEST 5: Trigger vs Delete
void TestTriggerVsDelete() {
    // 5A: Delete wins before Trigger
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "trig_5a") == ORBIS_OK, "T5A: create equeue");
        Check(sceKernelAddUserEventEdge(eq, 20) == ORBIS_OK, "T5A: add user event");
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T5A: delete equeue");
        Check(sceKernelTriggerUserEvent(eq, 20, nullptr) == ORBIS_KERNEL_ERROR_EBADF,
              "T5A: trigger after delete returns EBADF");
    }

    // 5B: Trigger acquires owner, pauses, Delete runs concurrently
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "trig_5b") == ORBIS_OK, "T5B: create equeue");
        Check(sceKernelAddUserEventEdge(eq, 21) == ORBIS_OK, "T5B: add user event");

        EqueueTestHooks::g_trigger_acquired_cb.Arm();
        int trigger_ret = 0;
        SyncSignal done;
        std::thread triggerer([&] {
            trigger_ret = sceKernelTriggerUserEvent(eq, 21, reinterpret_cast<void*>(0x55));
            done.Notify();
        });

        Check(EqueueTestHooks::g_trigger_acquired_cb.WaitUntilPaused(),
              "T5B: triggerer acquired queue ownership and paused");
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T5B: delete succeeds while triggerer owns queue");
        Check(!EqueueTestHooks::WasDestroyed(eq), "T5B: queue stays alive while triggerer holds owner");

        EqueueTestHooks::g_trigger_acquired_cb.Release();
        Check(done.Wait(), "T5B: triggerer finished safely");
        triggerer.join();

        Check(trigger_ret == ORBIS_KERNEL_ERROR_EBADF, "T5B: trigger against closed queue returns EBADF");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T5B: queue destroyed after triggerer released owner");
    }

    // 5C: Zero-timeout poll remains locked and functional
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "poll_5c") == ORBIS_OK, "T5C: create equeue");
        Check(sceKernelAddUserEventEdge(eq, 22) == ORBIS_OK, "T5C: add edge user event");

        OrbisKernelEvent ev{};
        int out = -1;
        OrbisKernelUseconds zero = 0;
        Check(sceKernelWaitEqueue(eq, &ev, 1, &out, &zero) == ORBIS_KERNEL_ERROR_ETIMEDOUT && out == 0,
              "T5C: zero-timeout poll returns ETIMEDOUT when empty");

        void* tag = reinterpret_cast<void*>(0xbeef);
        Check(sceKernelTriggerUserEvent(eq, 22, tag) == ORBIS_OK, "T5C: trigger event succeeds");
        Check(sceKernelWaitEqueue(eq, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 && ev.ident == 22 &&
                  ev.udata == tag,
              "T5C: zero-timeout poll delivers triggered event");
        Check(sceKernelWaitEqueue(eq, &ev, 1, &out, &zero) == ORBIS_KERNEL_ERROR_ETIMEDOUT && out == 0,
              "T5C: edge-triggered event cleared after poll");
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T5C: delete succeeds");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T5C: queue destroyed");
    }
}

// TEST 6: Concurrent Insert / Find / Erase in deterministic phases
void TestConcurrentRegistryPhases() {
    EqueueTestHooks::ResetHooks();
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 24;
    constexpr int kTotal = kWriters * kPerWriter;
    constexpr int kReaders = 4;

    std::vector<OrbisKernelEqueue> handles(kTotal, 0);
    PhaseBarrier create_barrier(kWriters);

    // Phase 1: Concurrent creation
    {
        std::vector<std::thread> threads;
        for (int w = 0; w < kWriters; ++w) {
            threads.emplace_back([&, w] {
                create_barrier.ArriveAndWait();
                for (int i = 0; i < kPerWriter; ++i) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "q_%d_%d", w, i);
                    OrbisKernelEqueue eq = 0;
                    if (sceKernelCreateEqueue(&eq, name) == ORBIS_OK) {
                        handles[w * kPerWriter + i] = eq;
                    }
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }

    std::unordered_set<OrbisKernelEqueue> unique_handles(handles.begin(), handles.end());
    Check(unique_handles.size() == kTotal && !unique_handles.contains(0),
          "T6: concurrent creates produced unique non-zero handles");

    // Phase 2: Concurrent readers (live + bogus handles) + racing deleters on first half + new creators
    constexpr int kHalf = kTotal / 2;
    std::atomic<int> ok_deletes{0};
    std::atomic<int> ebadf_deletes{0};
    std::atomic<bool> bogus_found{false};
    std::atomic<bool> bad_name{false};
    std::vector<OrbisKernelEqueue> extra_handles(kPerWriter, 0);
    PhaseBarrier phase2_barrier(kReaders + 3);

    std::vector<std::thread> phase2_threads;
    for (int r = 0; r < kReaders; ++r) {
        phase2_threads.emplace_back([&, r] {
            phase2_barrier.ArriveAndWait();
            for (int iter = 0; iter < 60; ++iter) {
                for (int i = 0; i < kTotal; ++i) {
                    if (auto q = AcquireEqueue(handles[i])) {
                        if (q->GetName().rfind("q_", 0) != 0) {
                            bad_name.store(true, std::memory_order_relaxed);
                        }
                    }
                }
                const OrbisKernelEqueue bogus = 0x700000 + r * 1000 + iter;
                if (AcquireEqueue(bogus) != nullptr) {
                    bogus_found.store(true, std::memory_order_relaxed);
                }
            }
        });
    }

    // Two deleters race on the exact same half of handles
    for (int d = 0; d < 2; ++d) {
        phase2_threads.emplace_back([&] {
            phase2_barrier.ArriveAndWait();
            for (int i = 0; i < kHalf; ++i) {
                const int rc = sceKernelDeleteEqueue(handles[i]);
                if (rc == ORBIS_OK) {
                    ok_deletes.fetch_add(1, std::memory_order_relaxed);
                } else if (rc == ORBIS_KERNEL_ERROR_EBADF) {
                    ebadf_deletes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // One concurrent creator during reads and deletes
    phase2_threads.emplace_back([&] {
        phase2_barrier.ArriveAndWait();
        for (int i = 0; i < kPerWriter; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "extra_%d", i);
            OrbisKernelEqueue eq = 0;
            if (sceKernelCreateEqueue(&eq, name) == ORBIS_OK) {
                extra_handles[i] = eq;
            }
        }
    });

    for (auto& t : phase2_threads) {
        t.join();
    }

    Check(ok_deletes.load() == kHalf, "T6: racing deleters deleted each handle exactly once");
    Check(ebadf_deletes.load() == kHalf, "T6: racing deleters received EBADF on every duplicate delete");
    Check(!bogus_found.load(), "T6: unknown handles never inserted or found");
    Check(!bad_name.load(), "T6: every acquired handle stayed valid for its reader");

    bool deleted_reacquired = false;
    for (int i = 0; i < kHalf; ++i) {
        if (AcquireEqueue(handles[i]) != nullptr) {
            deleted_reacquired = true;
        }
    }
    Check(!deleted_reacquired, "T6: deleted handles cannot be reacquired");

    bool survivor_missing = false;
    for (int i = kHalf; i < kTotal; ++i) {
        if (AcquireEqueue(handles[i]) == nullptr) {
            survivor_missing = true;
        }
    }
    Check(!survivor_missing, "T6: surviving handles remain acquirable");

    for (int i = kHalf; i < kTotal; ++i) {
        Check(sceKernelDeleteEqueue(handles[i]) == ORBIS_OK, "T6: cleanup surviving handle");
    }
    for (OrbisKernelEqueue eq : extra_handles) {
        Check(eq != 0 && sceKernelDeleteEqueue(eq) == ORBIS_OK, "T6: cleanup extra handle");
    }
    bool all_destroyed = true;
    for (OrbisKernelEqueue eq : handles) {
        if (!EqueueTestHooks::WaitDestroyed(eq)) {
            all_destroyed = false;
            break;
        }
    }
    for (OrbisKernelEqueue eq : extra_handles) {
        if (!EqueueTestHooks::WaitDestroyed(eq)) {
            all_destroyed = false;
            break;
        }
    }
    Check(all_destroyed, "T6: every deleted queue was destroyed");
}

// TEST 7: Source-level structural gate for raw pointers, registry safety, and timer captures
void TestStructuralSourceGate() {
    const std::string equeue_h = StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/kernel/equeue.h"));
    const std::string equeue_cpp = StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/kernel/equeue.cpp"));
    const std::string gnmdriver_cpp =
        StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp"));
    const std::string gnm_eq_h =
        StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/gnmdriver/gnm_eq_event.h"));
    const std::string video_out_cpp =
        StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/videoout/video_out.cpp"));
    const std::string driver_h =
        StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/videoout/driver.h"));
    const std::string driver_cpp =
        StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp"));
    const std::string platform_h = StripCommentsAndNormalize(ReadSource("gpu/shadps4/core/platform.h"));

    Check(!equeue_h.empty() && !equeue_cpp.empty(), "T7: loaded equeue production sources");
    Check(!gnmdriver_cpp.empty() && !gnm_eq_h.empty() && !video_out_cpp.empty() && !driver_h.empty() &&
              !driver_cpp.empty() && !platform_h.empty(),
          "T7: loaded VideoOut, GnmDriver and platform sources");

    // 1. No raw-pointer GetEqueue in header or callers
    Check(!std::regex_search(equeue_h, std::regex(R"(EqueueInternal\s*\*\s*GetEqueue\s*\()")),
          "T7: equeue.h does not declare raw-pointer GetEqueue");
    Check(std::regex_search(equeue_h, std::regex(R"(std::shared_ptr\s*<\s*EqueueInternal\s*>\s*AcquireEqueue\s*\()")),
          "T7: equeue.h declares strong-owning AcquireEqueue");

    // 2. No unsafe kqueues.contains / kqueues[...] / Find(...).get() / raw unordered_map outside wrapper
    Check(!std::regex_search(equeue_cpp, std::regex(R"(\b(?:k?queues)\s*\.\s*contains\s*\()")),
          "T7: equeue.cpp has no contains() lookup on handle table");
    Check(!std::regex_search(equeue_cpp, std::regex(R"(\b(?:k?queues)\s*\[)")),
          "T7: equeue.cpp has no operator[] access on handle table");
    Check(!std::regex_search(equeue_cpp, std::regex(R"(Find\s*\([^)]*\)\s*\.\s*get\s*\()")),
          "T7: equeue.cpp has no Find(...).get() temporary-owner leak");
    Check(!std::regex_search(equeue_cpp, std::regex(R"(unordered_map\s*<[^>]+,\s*EqueueInternal\s*\*\s*>)")),
          "T7: equeue.cpp has no raw EqueueInternal* handle map");
    Check(std::regex_search(equeue_cpp, std::regex(R"(std::shared_mutex\s+\w+)")),
          "T7: equeue.cpp synchronizes registry with shared_mutex");
    Check(std::regex_search(equeue_cpp, std::regex(R"(std::shared_lock\s+\w+\s*\{\s*mutex\s*\})")) &&
              std::regex_search(equeue_cpp, std::regex(R"(std::unique_lock\s+\w+\s*\{\s*mutex\s*\})")),
          "T7: EqueueTable uses shared_lock for readers and unique_lock for writers");
    Check(!std::regex_search(equeue_cpp, std::regex(R"(delete\s+kqueues)")),
          "T7: equeue.cpp does not raw-delete registry pointers");

    // 3. No timer async_wait lambda capturing raw this
    Check(!std::regex_search(equeue_cpp, std::regex(R"(async_wait\s*\(\s*\[[^\]]*\bthis\b)")),
          "T7: timer async_wait lambda never captures raw this");
    Check(!std::regex_search(equeue_cpp, std::regex(R"(async_wait\s*\(\s*\[=)")),
          "T7: timer async_wait lambda never uses default capture [=]");
    Check(std::regex_search(equeue_cpp, std::regex(R"(async_wait\s*\(\s*\[\s*handle\s*=\s*m_handle)")),
          "T7: timer async_wait lambda captures handle by value");

    // 4. Zero-timeout poll stays locked
    Check(std::regex_search(
              equeue_cpp,
              std::regex(R"(if\s*\(\s*timo\s*!=\s*nullptr\s*&&\s*\*timo\s*==\s*0\s*\)\s*\{\s*std::(?:scoped_lock|unique_lock)\s+\w+\s*\{\s*m_mutex\s*\})")),
          "T7: zero-timeout poll locks m_mutex");

    // 5. Callers migrated off raw GetEqueue and clean up stale subscriptions safely
    Check(!std::regex_search(gnmdriver_cpp, std::regex(R"(\bGetEqueue\s*\()")),
          "T7: gnmdriver.cpp has no raw GetEqueue call");
    Check(!std::regex_search(gnm_eq_h, std::regex(R"(\bGetEqueue\s*\()")),
          "T7: gnm_eq_event.h has no raw GetEqueue call");
    Check(!std::regex_search(video_out_cpp, std::regex(R"(\bGetEqueue\s*\()")),
          "T7: video_out.cpp has no raw GetEqueue call");
    Check(!std::regex_search(driver_h, std::regex(R"(\bGetEqueue\s*\()")),
          "T7: driver.h has no raw GetEqueue call");
    Check(!std::regex_search(driver_cpp, std::regex(R"(\bGetEqueue\s*\()")),
          "T7: driver.cpp has no raw GetEqueue call");
    Check(std::regex_search(gnm_eq_h, std::regex(R"(AcquireEqueue\s*\(\s*eq\s*\))")),
          "T7: gnm_eq_event.h uses AcquireEqueue");
    Check(std::regex_search(gnm_eq_h, std::regex(R"(\[\s*eq\s*,\s*id\s*\]\s*\(\s*Platform::InterruptId)")),
          "T7: gnm_eq_event.h IRQ handler captures handle eq by value, not raw pointer");
    Check(std::regex_search(gnm_eq_h, std::regex(R"(RegisterManaged\s*\()")),
          "T7: gnm_eq_event.h registers managed self-cleaning IRQ handler");
    Check(std::regex_search(video_out_cpp, std::regex(R"(Kernel::AcquireEqueue\s*\()")),
          "T7: video_out.cpp uses Kernel::AcquireEqueue");
    Check(std::regex_search(driver_cpp, std::regex(R"(Kernel::AcquireEqueue\s*\()")),
          "T7: driver.cpp uses Kernel::AcquireEqueue");
    Check(std::regex_search(driver_h, std::regex(R"(std::erase_if\s*\(\s*flip_events\s*,)")),
          "T7: driver.h uses iterator-safe std::erase_if for flip_events");
    Check(std::regex_search(driver_h, std::regex(R"(std::erase_if\s*\(\s*vblank_events\s*,)")),
          "T7: driver.h uses iterator-safe std::erase_if for vblank_events");
    Check(!std::regex_search(driver_h, std::regex(R"(for\s*\([^{]*flip_events[^{]*\{[^}]*flip_events\s*\.\s*erase)")),
          "T7: driver.h never calls flip_events.erase inside a range-for loop");
    Check(!std::regex_search(driver_h, std::regex(R"(for\s*\([^{]*vblank_events[^{]*\{[^}]*vblank_events\s*\.\s*erase)")),
          "T7: driver.h never calls vblank_events.erase inside a range-for loop");
    Check(std::regex_search(driver_h, std::regex(R"(it\s*!=\s*flip_events\s*\.\s*end\s*\(\s*\))")) &&
              std::regex_search(driver_h, std::regex(R"(it\s*!=\s*vblank_events\s*\.\s*end\s*\(\s*\))")),
          "T7: driver.h guards explicit flip/vblank removal against missing entries");
    Check(!std::regex_search(platform_h, std::regex(R"(std::unordered_map\s*<\s*InterruptId\s*,\s*IrqContext\s*>)")),
          "T7: platform.h IrqController does not use unsynchronized unordered_map for irq_contexts");
    Check(std::regex_search(platform_h, std::regex(R"(it\s*=\s*ctx\s*\.\s*persistent_handlers\s*\.\s*erase\s*\(\s*it\s*\))")),
          "T7: platform.h Signal erases stale handlers using iterator-safe erase(it)");
}

// TEST 8: Periodic timer reschedules while alive, stops on delete, plus HR timer (long and small)
void TestPeriodicAndHrTimers() {
    // 8A: Periodic timer fires twice while alive, stops after delete
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "periodic_8a") == ORBIS_OK, "T8A: create equeue");
        Check(sceKernelAddTimerEvent(eq, 31, 2000, reinterpret_cast<void*>(0x31)) == ORBIS_OK,
              "T8A: add 2 ms periodic timer");

        OrbisKernelEvent ev1{};
        int out1 = 0;
        OrbisKernelUseconds timo = 3'000'000u;
        Check(sceKernelWaitEqueue(eq, &ev1, 1, &out1, &timo) == ORBIS_OK && out1 == 1 && ev1.ident == 31,
              "T8A: periodic timer delivered first tick");

        OrbisKernelEvent ev2{};
        int out2 = 0;
        Check(sceKernelWaitEqueue(eq, &ev2, 1, &out2, &timo) == ORBIS_OK && out2 == 1 && ev2.ident == 31,
              "T8A: periodic timer rescheduled and delivered second tick");

        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T8A: delete periodic timer queue");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T8A: periodic queue destroyed");
        const int signals_after_delete = g_kernel_signals.load(std::memory_order_relaxed);
        Check(sceKernelWaitEqueue(eq, &ev2, 1, &out2, &timo) == ORBIS_KERNEL_ERROR_EBADF,
              "T8A: wait after delete returns EBADF");
        Check(g_kernel_signals.load(std::memory_order_relaxed) == signals_after_delete,
              "T8A: no reschedule signals after delete");
    }

    // 8B: Small HR timer (< 1.2 ms) delivers event, and long HR timer (>= 1.2 ms) fires via asio callback
    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "hr_small_8b") == ORBIS_OK, "T8B: create small HR equeue");
        OrbisKernelTimespec small_ts{0, 100'000}; // 100 us
        Check(sceKernelAddHRTimerEvent(eq, 41, &small_ts, reinterpret_cast<void*>(0x41)) == ORBIS_OK,
              "T8B: add small HR timer");

        OrbisKernelEvent ev{};
        int out = 0;
        OrbisKernelUseconds timo = 2'000'000u;
        Check(sceKernelWaitEqueue(eq, &ev, 1, &out, &timo) == ORBIS_OK && out == 1 && ev.ident == 41,
              "T8B: small HR timer expired and delivered event");
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T8B: delete small HR equeue");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T8B: small HR equeue destroyed");
    }

    {
        EqueueTestHooks::ResetHooks();
        OrbisKernelEqueue eq = 0;
        Check(sceKernelCreateEqueue(&eq, "hr_long_8c") == ORBIS_OK, "T8C: create long HR equeue");
        OrbisKernelTimespec long_ts{0, 2'000'000}; // 2 ms >= 1.2 ms
        Check(sceKernelAddHRTimerEvent(eq, 42, &long_ts, reinterpret_cast<void*>(0x42)) == ORBIS_OK,
              "T8C: add long HR timer");

        OrbisKernelEvent ev{};
        int out = 0;
        OrbisKernelUseconds timo = 3'000'000u;
        Check(sceKernelWaitEqueue(eq, &ev, 1, &out, &timo) == ORBIS_OK && out == 1 && ev.ident == 42,
              "T8C: long HR timer delivered event via HrTimerCallback");
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T8C: delete long HR equeue");
        Check(EqueueTestHooks::WaitDestroyed(eq), "T8C: long HR equeue destroyed");
    }
}

// TEST 9: Multiple waiters on level-triggered user event (proves notify_all prevents stranded waiter)
void TestMultipleWaitersNotification() {
    EqueueTestHooks::ResetHooks();
    OrbisKernelEqueue eq = 0;
    Check(sceKernelCreateEqueue(&eq, "multi_wait") == ORBIS_OK, "T9: create equeue");
    Check(sceKernelAddUserEvent(eq, 99) == ORBIS_OK, "T9: add level-triggered user event");

    auto probe = AcquireEqueue(eq);
    int ret1 = 0;
    int ret2 = 0;
    int out1 = 0;
    int out2 = 0;
    OrbisKernelEvent ev1{};
    OrbisKernelEvent ev2{};
    SyncSignal done1;
    SyncSignal done2;

    std::thread w1([&] {
        ret1 = sceKernelWaitEqueue(eq, &ev1, 1, &out1, nullptr);
        done1.Notify();
    });
    std::thread w2([&] {
        ret2 = sceKernelWaitEqueue(eq, &ev2, 1, &out2, nullptr);
        done2.Notify();
    });

    Check(WaitForWaiters(probe, 2), "T9: both waiters proved sleeping on condition variable");
    probe.reset();

    void* tag = reinterpret_cast<void*>(0x99);
    Check(sceKernelTriggerUserEvent(eq, 99, tag) == ORBIS_OK, "T9: single trigger fires");

    const bool woke1 = done1.Wait(std::chrono::milliseconds(2000));
    const bool woke2 = done2.Wait(std::chrono::milliseconds(2000));
    if (!woke1 || !woke2) {
        // Unblock any stranded waiter before joining so failure terminates cleanly.
        sceKernelDeleteEqueue(eq);
    }
    w1.join();
    w2.join();

    Check(woke1 && woke2, "T9: both sleeping waiters woke from single level-triggered event");
    Check(ret1 == ORBIS_OK && out1 == 1 && ev1.ident == 99 && ev1.udata == tag, "T9: waiter 1 received event");
    Check(ret2 == ORBIS_OK && out2 == 1 && ev2.ident == 99 && ev2.udata == tag, "T9: waiter 2 received event");
    if (woke1 && woke2) {
        Check(sceKernelDeleteEqueue(eq) == ORBIS_OK, "T9: delete equeue");
    }
    Check(EqueueTestHooks::WaitDestroyed(eq), "T9: equeue destroyed");
}

void AddVideoOutEventToQueue(OrbisKernelEqueue eq, Libraries::VideoOut::OrbisVideoOutInternalEventId id, void* udata) {
    auto equeue = AcquireEqueue(eq);
    if (!equeue) {
        return;
    }
    EqueueEvent ev{};
    ev.event.ident = static_cast<u64>(id);
    ev.event.filter = OrbisKernelEvent::Filter::VideoOut;
    ev.event.flags = OrbisKernelEvent::Flags::Add;
    ev.event.udata = udata;
    equeue->AddEvent(ev);
}

// TEST 10: VideoOut Flip stale subscription cleanup + concurrent delete during dispatch
void TestVideoOutFlipStaleCleanup() {
    using namespace Libraries::VideoOut;
    EqueueTestHooks::ResetHooks();

    VideoOutPort port{};
    OrbisKernelEqueue eq_dead = 0;
    OrbisKernelEqueue eq_live = 0;
    Check(sceKernelCreateEqueue(&eq_dead, "vo_flip_dead") == ORBIS_OK, "T10: create dead flip equeue");
    Check(sceKernelCreateEqueue(&eq_live, "vo_flip_live") == ORBIS_OK, "T10: create live flip equeue");

    AddVideoOutEventToQueue(eq_dead, OrbisVideoOutInternalEventId::Flip, reinterpret_cast<void*>(0x101));
    AddVideoOutEventToQueue(eq_live, OrbisVideoOutInternalEventId::Flip, reinterpret_cast<void*>(0x102));
    port.AddFlipEvent(eq_dead);
    port.AddFlipEvent(eq_live);

    Check(sceKernelDeleteEqueue(eq_dead) == ORBIS_OK, "T10: delete equeue without removing flip event");
    Check(EqueueTestHooks::WaitDestroyed(eq_dead), "T10: deleted flip equeue destroyed immediately");

    EqueueTestHooks::ResetHooks();
    {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(0x55);
    }
    Check(EqueueTestHooks::AcquireLookupCount(eq_dead) == 1 && EqueueTestHooks::AcquireMissCount(eq_dead) == 1,
          "T10: pass 1 observed deleted flip handle via AcquireEqueue");
    Check(port.flip_events.size() == 1 && port.flip_events[0] == eq_live,
          "T10: stale flip subscription removed while live handle preserved");

    OrbisKernelEvent ev{};
    int out = 0;
    OrbisKernelUseconds zero = 0;
    Check(sceKernelWaitEqueue(eq_live, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 &&
              ev.ident == static_cast<u64>(OrbisVideoOutInternalEventId::Flip) && (ev.data >> 16) == 0x55,
          "T10: live flip equeue received flip event on pass 1");

    {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(0x66);
    }
    Check(EqueueTestHooks::AcquireLookupCount(eq_dead) == 1,
          "T10: pass 2 did not re-attempt dead flip handle lookup");
    Check(sceKernelWaitEqueue(eq_live, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 && (ev.data >> 16) == 0x66,
          "T10: live flip equeue received flip event on pass 2");
    Check(AcquireEqueue(eq_dead) == nullptr, "T10: dead flip equeue not resurrected");

    // Race A: concurrent delete while TriggerFlipEventsLocked holds strong shared_ptr
    OrbisKernelEqueue eq_race = 0;
    Check(sceKernelCreateEqueue(&eq_race, "vo_flip_race") == ORBIS_OK, "T10R: create racing flip equeue");
    AddVideoOutEventToQueue(eq_race, OrbisVideoOutInternalEventId::Flip, nullptr);
    port.AddFlipEvent(eq_race);

    EqueueTestHooks::ResetHooks();
    EqueueTestHooks::ArmAcquirePause(eq_race);
    SyncSignal flip_done;
    std::thread flipper([&] {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(0x77);
        flip_done.Notify();
    });

    Check(EqueueTestHooks::g_acquire_lookup_cb.WaitUntilPaused(),
          "T10R: flip dispatch acquired strong ownership and paused");
    Check(sceKernelDeleteEqueue(eq_race) == ORBIS_OK, "T10R: delete flip equeue concurrently with dispatch");
    Check(!EqueueTestHooks::WasDestroyed(eq_race), "T10R: equeue stays alive while flip dispatch holds owner");

    EqueueTestHooks::g_acquire_lookup_cb.Release();
    Check(flip_done.Wait(), "T10R: flip dispatch completed safely");
    flipper.join();

    Check(EqueueTestHooks::WaitDestroyed(eq_race), "T10R: racing flip equeue destroyed after dispatch");
    Check(port.flip_events.size() == 1 && port.flip_events[0] == eq_live,
          "T10R: closed racing flip handle removed immediately on same pass");

    port.RemoveFlipEvent(eq_live);
    Check(sceKernelDeleteEqueue(eq_live) == ORBIS_OK, "T10: cleanup live flip equeue");
    Check(EqueueTestHooks::WaitDestroyed(eq_live), "T10: live flip equeue destroyed");
}

// TEST 11: VideoOut Vblank stale subscription cleanup + concurrent delete during dispatch
void TestVideoOutVblankStaleCleanup() {
    using namespace Libraries::VideoOut;
    EqueueTestHooks::ResetHooks();

    VideoOutPort port{};
    OrbisKernelEqueue eq_dead = 0;
    OrbisKernelEqueue eq_live = 0;
    Check(sceKernelCreateEqueue(&eq_dead, "vo_vb_dead") == ORBIS_OK, "T11: create dead vblank equeue");
    Check(sceKernelCreateEqueue(&eq_live, "vo_vb_live") == ORBIS_OK, "T11: create live vblank equeue");

    AddVideoOutEventToQueue(eq_dead, OrbisVideoOutInternalEventId::Vblank, reinterpret_cast<void*>(0x201));
    AddVideoOutEventToQueue(eq_live, OrbisVideoOutInternalEventId::Vblank, reinterpret_cast<void*>(0x202));
    port.AddVblankEvent(eq_dead);
    port.AddVblankEvent(eq_live);

    Check(sceKernelDeleteEqueue(eq_dead) == ORBIS_OK, "T11: delete equeue without removing vblank event");
    Check(EqueueTestHooks::WaitDestroyed(eq_dead), "T11: deleted vblank equeue destroyed immediately");

    EqueueTestHooks::ResetHooks();
    {
        std::scoped_lock lock{port.vo_mutex};
        port.TriggerVblankEventsLocked(10);
    }
    Check(EqueueTestHooks::AcquireLookupCount(eq_dead) == 1 && EqueueTestHooks::AcquireMissCount(eq_dead) == 1,
          "T11: pass 1 observed deleted vblank handle via AcquireEqueue");
    Check(port.vblank_events.size() == 1 && port.vblank_events[0] == eq_live,
          "T11: stale vblank subscription removed while live handle preserved");

    OrbisKernelEvent ev{};
    int out = 0;
    OrbisKernelUseconds zero = 0;
    Check(sceKernelWaitEqueue(eq_live, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 &&
              ev.ident == static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) && (ev.data >> 16) == 10,
          "T11: live vblank equeue received vblank event on pass 1");

    {
        std::scoped_lock lock{port.vo_mutex};
        port.TriggerVblankEventsLocked(11);
    }
    Check(EqueueTestHooks::AcquireLookupCount(eq_dead) == 1,
          "T11: pass 2 did not re-attempt dead vblank handle lookup");
    Check(sceKernelWaitEqueue(eq_live, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 && (ev.data >> 16) == 11,
          "T11: live vblank equeue received vblank event on pass 2");

    // Race B: concurrent delete while TriggerVblankEventsLocked holds strong shared_ptr
    OrbisKernelEqueue eq_race = 0;
    Check(sceKernelCreateEqueue(&eq_race, "vo_vb_race") == ORBIS_OK, "T11R: create racing vblank equeue");
    AddVideoOutEventToQueue(eq_race, OrbisVideoOutInternalEventId::Vblank, nullptr);
    port.AddVblankEvent(eq_race);

    EqueueTestHooks::ResetHooks();
    EqueueTestHooks::ArmAcquirePause(eq_race);
    SyncSignal vb_done;
    std::thread present([&] {
        std::scoped_lock lock{port.vo_mutex};
        port.TriggerVblankEventsLocked(12);
        vb_done.Notify();
    });

    Check(EqueueTestHooks::g_acquire_lookup_cb.WaitUntilPaused(),
          "T11R: vblank dispatch acquired strong ownership and paused");
    Check(sceKernelDeleteEqueue(eq_race) == ORBIS_OK, "T11R: delete vblank equeue concurrently with dispatch");
    Check(!EqueueTestHooks::WasDestroyed(eq_race), "T11R: equeue stays alive while vblank dispatch holds owner");

    EqueueTestHooks::g_acquire_lookup_cb.Release();
    Check(vb_done.Wait(), "T11R: vblank dispatch completed safely");
    present.join();

    Check(EqueueTestHooks::WaitDestroyed(eq_race), "T11R: racing vblank equeue destroyed after dispatch");
    Check(port.vblank_events.size() == 1 && port.vblank_events[0] == eq_live,
          "T11R: closed racing vblank handle removed immediately on same pass");

    port.RemoveVblankEvent(eq_live);
    Check(sceKernelDeleteEqueue(eq_live) == ORBIS_OK, "T11: cleanup live vblank equeue");
    Check(EqueueTestHooks::WaitDestroyed(eq_live), "T11: live vblank equeue destroyed");
}

// TEST 12: GnmDriver / Platform::IrqC stale subscription cleanup + Race C
void TestIrqStaleCleanup() {
    using Platform::InterruptId;
    using Platform::IrqC;
    EqueueTestHooks::ResetHooks();

    OrbisKernelEqueue eq_dead = 0;
    OrbisKernelEqueue eq_live = 0;
    Check(sceKernelCreateEqueue(&eq_dead, "irq_dead") == ORBIS_OK, "T12: create dead IRQ equeue");
    Check(sceKernelCreateEqueue(&eq_live, "irq_live") == ORBIS_OK, "T12: create live IRQ equeue");

    const u64 eop_id = static_cast<u64>(InterruptId::GfxEop);
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_dead, eop_id, reinterpret_cast<void*>(0x301)) == ORBIS_OK,
          "T12: register dead GfxEop IRQ event");
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_live, eop_id, reinterpret_cast<void*>(0x302)) == ORBIS_OK,
          "T12: register live GfxEop IRQ event");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 2,
          "T12: both IRQ subscriptions registered");

    Check(sceKernelDeleteEqueue(eq_dead) == ORBIS_OK, "T12: delete equeue without sceGnmDeleteEqEvent");
    Check(EqueueTestHooks::WaitDestroyed(eq_dead), "T12: deleted IRQ equeue destroyed immediately");

    EqueueTestHooks::ResetHooks();
    IrqC::Instance()->Signal(InterruptId::GfxEop);
    Check(EqueueTestHooks::AcquireLookupCount(eq_dead) == 1 && EqueueTestHooks::AcquireMissCount(eq_dead) == 1,
          "T12: first IRQ observed deleted handle via AcquireEqueue");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 1,
          "T12: stale IRQ subscription removed on first signal");

    OrbisKernelEvent ev{};
    int out = 0;
    OrbisKernelUseconds zero = 0;
    Check(sceKernelWaitEqueue(eq_live, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 && ev.ident == eop_id &&
              ev.filter == OrbisKernelEvent::Filter::GraphicsCore,
          "T12: live IRQ subscription delivered event on signal 1");

    IrqC::Instance()->Signal(InterruptId::GfxEop);
    Check(EqueueTestHooks::AcquireLookupCount(eq_dead) == 1,
          "T12: second IRQ did not execute stale callback path");
    Check(sceKernelWaitEqueue(eq_live, &ev, 1, &out, &zero) == ORBIS_OK && out == 1 && ev.ident == eop_id,
          "T12: live IRQ subscription delivered event on signal 2");

    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_dead, eop_id) == ORBIS_KERNEL_ERROR_EBADF,
          "T12: explicit later unregister of already-cleaned handle is safe and returns EBADF");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 1,
          "T12: explicit later unregister leaves live subscription intact");

    // Race C: concurrent delete while IRQ callback holds strong shared_ptr
    OrbisKernelEqueue eq_race = 0;
    const u64 comp_id = static_cast<u64>(InterruptId::Compute0RelMem);
    Check(sceKernelCreateEqueue(&eq_race, "irq_race") == ORBIS_OK, "T12R: create racing IRQ equeue");
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_race, comp_id, nullptr) == ORBIS_OK,
          "T12R: register Compute0RelMem IRQ event");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::Compute0RelMem) == 1,
          "T12R: racing IRQ handler registered");

    EqueueTestHooks::ResetHooks();
    EqueueTestHooks::ArmAcquirePause(eq_race);
    SyncSignal irq_done;
    std::thread gpu([&] {
        IrqC::Instance()->Signal(InterruptId::Compute0RelMem);
        irq_done.Notify();
    });

    Check(EqueueTestHooks::g_acquire_lookup_cb.WaitUntilPaused(),
          "T12R: IRQ callback acquired strong ownership and paused");
    Check(sceKernelDeleteEqueue(eq_race) == ORBIS_OK, "T12R: delete IRQ equeue concurrently with signal");
    Check(!EqueueTestHooks::WasDestroyed(eq_race), "T12R: equeue stays alive while IRQ callback holds owner");

    EqueueTestHooks::g_acquire_lookup_cb.Release();
    Check(irq_done.Wait(), "T12R: IRQ signal completed safely");
    gpu.join();

    Check(EqueueTestHooks::WaitDestroyed(eq_race), "T12R: racing IRQ equeue destroyed after callback");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::Compute0RelMem) == 0,
          "T12R: closed racing IRQ subscription removed immediately");

    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_live, eop_id) == ORBIS_OK, "T12: unregister live IRQ");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 0, "T12: GfxEop handler table empty");
    Check(sceKernelDeleteEqueue(eq_live) == ORBIS_OK, "T12: delete live IRQ equeue");
    Check(EqueueTestHooks::WaitDestroyed(eq_live), "T12: live IRQ equeue destroyed");
}

// TEST 13: Explicit remove vs automatic cleanup (both orders, double remove, concurrent races)
void TestExplicitRemoveVsAutoCleanup() {
    using namespace Libraries::VideoOut;
    using Platform::InterruptId;
    using Platform::IrqC;
    EqueueTestHooks::ResetHooks();

    VideoOutPort port{};
    OrbisKernelEqueue eq_keep = 0;
    OrbisKernelEqueue eq_a = 0;
    OrbisKernelEqueue eq_b = 0;
    Check(sceKernelCreateEqueue(&eq_keep, "t13_keep") == ORBIS_OK, "T13: create sentinel live equeue");
    Check(sceKernelCreateEqueue(&eq_a, "t13_a") == ORBIS_OK, "T13: create equeue A");
    Check(sceKernelCreateEqueue(&eq_b, "t13_b") == ORBIS_OK, "T13: create equeue B");

    const u64 eop_id = static_cast<u64>(InterruptId::GfxEop);
    AddVideoOutEventToQueue(eq_keep, OrbisVideoOutInternalEventId::Flip, nullptr);
    AddVideoOutEventToQueue(eq_keep, OrbisVideoOutInternalEventId::Vblank, nullptr);
    port.AddFlipEvent(eq_keep);
    port.AddVblankEvent(eq_keep);
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_keep, eop_id, nullptr) == ORBIS_OK,
          "T13: register sentinel GfxEop");

    port.AddFlipEvent(eq_a);
    port.AddVblankEvent(eq_a);
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_a, eop_id, nullptr) == ORBIS_OK, "T13: register A GfxEop");
    port.AddFlipEvent(eq_b);
    port.AddVblankEvent(eq_b);
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_b, eop_id, nullptr) == ORBIS_OK, "T13: register B GfxEop");

    // Order 1: Automatic cleanup FIRST, then explicit remove TWICE (idempotent)
    Check(sceKernelDeleteEqueue(eq_a) == ORBIS_OK, "T13: delete A");
    {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(1);
    }
    {
        std::scoped_lock lock{port.vo_mutex};
        port.TriggerVblankEventsLocked(1);
    }
    IrqC::Instance()->Signal(InterruptId::GfxEop);

    port.RemoveFlipEvent(eq_a);
    port.RemoveFlipEvent(eq_a);
    port.RemoveVblankEvent(eq_a);
    port.RemoveVblankEvent(eq_a);
    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_a, eop_id) == ORBIS_KERNEL_ERROR_EBADF,
          "T13: explicit IRQ remove after auto cleanup returns EBADF");
    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_a, eop_id) == ORBIS_KERNEL_ERROR_EBADF,
          "T13: repeated explicit IRQ remove is idempotent");

    // Order 2: Explicit remove FIRST, then dispatch/signal runs (zero dead lookup)
    Check(sceKernelDeleteEqueue(eq_b) == ORBIS_OK, "T13: delete B");
    EqueueTestHooks::ResetHooks();
    port.RemoveFlipEvent(eq_b);
    port.RemoveVblankEvent(eq_b);
    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_b, eop_id) == ORBIS_KERNEL_ERROR_EBADF,
          "T13: explicit IRQ remove on deleted B succeeds cleanly");

    {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(2);
    }
    {
        std::scoped_lock lock{port.vo_mutex};
        port.TriggerVblankEventsLocked(2);
    }
    IrqC::Instance()->Signal(InterruptId::GfxEop);
    Check(EqueueTestHooks::AcquireLookupCount(eq_b) == 1,
          "T13: dispatch/signal performed zero dead lookups for explicitly removed B (only 1 inside UnregisterGnmEqEvent)");
    Check(port.flip_events.size() == 1 && port.flip_events[0] == eq_keep, "T13: sentinel flip intact");
    Check(port.vblank_events.size() == 1 && port.vblank_events[0] == eq_keep, "T13: sentinel vblank intact");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 1, "T13: sentinel IRQ intact");

    // Concurrent overlap: dispatch/signal vs explicit unregister vs queue deletion across batch
    constexpr int kBatch = 24;
    std::vector<OrbisKernelEqueue> batch(kBatch, 0);
    for (int i = 0; i < kBatch; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "t13_c_%d", i);
        Check(sceKernelCreateEqueue(&batch[i], name) == ORBIS_OK, "T13C: create batch queue");
        port.AddFlipEvent(batch[i]);
        port.AddVblankEvent(batch[i]);
        Check(Libraries::GnmDriver::RegisterGnmEqEvent(batch[i], eop_id, nullptr) == ORBIS_OK,
              "T13C: register batch IRQ");
    }

    PhaseBarrier start_race(4);
    std::thread t_deleter([&] {
        start_race.ArriveAndWait();
        for (OrbisKernelEqueue eq : batch) {
            sceKernelDeleteEqueue(eq);
        }
    });
    std::thread t_unreg([&] {
        start_race.ArriveAndWait();
        for (int i = 0; i < kBatch; i += 2) {
            port.RemoveFlipEvent(batch[i]);
            port.RemoveVblankEvent(batch[i]);
            Libraries::GnmDriver::UnregisterGnmEqEvent(batch[i], eop_id);
        }
    });
    std::thread t_vo([&] {
        start_race.ArriveAndWait();
        for (int iter = 0; iter < 30; ++iter) {
            {
                std::scoped_lock lock{port.port_mutex};
                port.TriggerFlipEventsLocked(iter);
            }
            {
                std::scoped_lock lock{port.vo_mutex};
                port.TriggerVblankEventsLocked(iter);
            }
        }
    });
    std::thread t_irq([&] {
        start_race.ArriveAndWait();
        for (int iter = 0; iter < 30; ++iter) {
            IrqC::Instance()->Signal(InterruptId::GfxEop);
        }
    });

    t_deleter.join();
    t_unreg.join();
    t_vo.join();
    t_irq.join();

    // One final pass cleans any entry deleted after the race loop finished.
    {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(99);
    }
    {
        std::scoped_lock lock{port.vo_mutex};
        port.TriggerVblankEventsLocked(99);
    }
    IrqC::Instance()->Signal(InterruptId::GfxEop);

    Check(port.flip_events.size() == 1 && port.flip_events[0] == eq_keep,
          "T13C: all stale flip entries cleaned after concurrent race");
    Check(port.vblank_events.size() == 1 && port.vblank_events[0] == eq_keep,
          "T13C: all stale vblank entries cleaned after concurrent race");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 1,
          "T13C: all stale IRQ entries cleaned after concurrent race");

    port.RemoveFlipEvent(eq_keep);
    port.RemoveVblankEvent(eq_keep);
    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_keep, eop_id) == ORBIS_OK, "T13: cleanup sentinel IRQ");
    Check(sceKernelDeleteEqueue(eq_keep) == ORBIS_OK, "T13: cleanup sentinel equeue");
    Check(EqueueTestHooks::WaitDestroyed(eq_keep), "T13: sentinel equeue destroyed");
}

// TEST 14: No regression of live subscriptions (multi-event shared queue + independent queues + void IRQ handler)
void TestLiveSubscriptionsNoRegression() {
    using namespace Libraries::VideoOut;
    using Platform::InterruptId;
    using Platform::IrqC;
    EqueueTestHooks::ResetHooks();

    VideoOutPort port{};
    OrbisKernelEqueue eq_multi = 0;
    OrbisKernelEqueue eq_other = 0;
    Check(sceKernelCreateEqueue(&eq_multi, "t14_multi") == ORBIS_OK, "T14: create multi-subscription equeue");
    Check(sceKernelCreateEqueue(&eq_other, "t14_other") == ORBIS_OK, "T14: create independent equeue");

    const u64 eop_id = static_cast<u64>(InterruptId::GfxEop);
    const u64 comp1_id = static_cast<u64>(InterruptId::Compute1RelMem);
    AddVideoOutEventToQueue(eq_multi, OrbisVideoOutInternalEventId::Flip, reinterpret_cast<void*>(0x401));
    AddVideoOutEventToQueue(eq_multi, OrbisVideoOutInternalEventId::Vblank, reinterpret_cast<void*>(0x402));
    AddVideoOutEventToQueue(eq_other, OrbisVideoOutInternalEventId::Flip, reinterpret_cast<void*>(0x403));
    port.AddFlipEvent(eq_multi);
    port.AddVblankEvent(eq_multi);
    port.AddFlipEvent(eq_other);
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_multi, eop_id, reinterpret_cast<void*>(0x404)) == ORBIS_OK,
          "T14: register multi GfxEop");
    Check(Libraries::GnmDriver::RegisterGnmEqEvent(eq_multi, comp1_id, reinterpret_cast<void*>(0x405)) == ORBIS_OK,
          "T14: register multi Compute1RelMem");

    int idle_hits = 0;
    void* idle_uid = &idle_hits;
    IrqC::Instance()->Register(
        InterruptId::GpuIdle,
        [&](InterruptId irq) {
            if (irq == InterruptId::GpuIdle) {
                ++idle_hits;
            }
        },
        idle_uid);

    for (int cycle = 1; cycle <= 3; ++cycle) {
        {
            std::scoped_lock lock{port.port_mutex};
            port.TriggerFlipEventsLocked(cycle * 10);
        }
        {
            std::scoped_lock lock{port.vo_mutex};
            port.TriggerVblankEventsLocked(cycle * 100);
        }
        IrqC::Instance()->Signal(InterruptId::GfxEop);
        IrqC::Instance()->Signal(InterruptId::Compute1RelMem);
        IrqC::Instance()->Signal(InterruptId::GpuIdle);

        OrbisKernelEvent evs[4]{};
        int out = 0;
        OrbisKernelUseconds zero = 0;
        Check(sceKernelWaitEqueue(eq_multi, evs, 4, &out, &zero) == ORBIS_OK && out == 4,
              "T14: live multi-subscription equeue received all 4 events");

        OrbisKernelEvent other_ev{};
        int other_out = 0;
        Check(sceKernelWaitEqueue(eq_other, &other_ev, 1, &other_out, &zero) == ORBIS_OK && other_out == 1 &&
                  (other_ev.data >> 16) == static_cast<u64>(cycle * 10),
              "T14: independent live equeue received flip event");
    }

    Check(idle_hits == 3, "T14: non-expiring void IRQ handler fired all 3 cycles");
    Check(port.flip_events.size() == 2, "T14: both live flip subscriptions retained");
    Check(port.vblank_events.size() == 1, "T14: live vblank subscription retained");
    Check(IrqC::Instance()->PersistentHandlerCount(InterruptId::GfxEop) == 1 &&
              IrqC::Instance()->PersistentHandlerCount(InterruptId::Compute1RelMem) == 1 &&
              IrqC::Instance()->PersistentHandlerCount(InterruptId::GpuIdle) == 1,
          "T14: all live IRQ handlers retained");

    // Deleting eq_other must not over-clean eq_multi
    Check(sceKernelDeleteEqueue(eq_other) == ORBIS_OK, "T14: delete independent equeue");
    {
        std::scoped_lock lock{port.port_mutex};
        port.TriggerFlipEventsLocked(999);
    }
    Check(port.flip_events.size() == 1 && port.flip_events[0] == eq_multi,
          "T14: stale cleanup removed only deleted equeue, keeping eq_multi");

    IrqC::Instance()->Unregister(InterruptId::GpuIdle, idle_uid);
    port.RemoveFlipEvent(eq_multi);
    port.RemoveVblankEvent(eq_multi);
    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_multi, eop_id) == ORBIS_OK, "T14: cleanup GfxEop");
    Check(Libraries::GnmDriver::UnregisterGnmEqEvent(eq_multi, comp1_id) == ORBIS_OK, "T14: cleanup Compute1RelMem");
    Check(sceKernelDeleteEqueue(eq_multi) == ORBIS_OK, "T14: cleanup eq_multi");
    Check(EqueueTestHooks::WaitDestroyed(eq_multi), "T14: eq_multi destroyed");
}

} // namespace

int main() {
    AsioServiceGuard service;
    TestLookupVsDelete();
    TestDeleteVsTimerCallback();
    TestCallbackOwnsQueueDuringDelete();
    TestWaitVsDelete();
    TestTriggerVsDelete();
    TestConcurrentRegistryPhases();
    TestStructuralSourceGate();
    TestPeriodicAndHrTimers();
    TestMultipleWaitersNotification();
    TestVideoOutFlipStaleCleanup();
    TestVideoOutVblankStaleCleanup();
    TestIrqStaleCleanup();
    TestExplicitRemoveVsAutoCleanup();
    TestLiveSubscriptionsNoRegression();

    std::printf("equeue-lifetime: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
