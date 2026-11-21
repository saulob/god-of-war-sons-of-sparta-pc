// The production recursive lock helper: nesting takes the mutex once, release order is free, and after the first lock a
// thread never allocates again on the draw path. Built against a stub for the assert macros. No window, GPU or game.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <thread>

#include "common/recursive_lock.h"

namespace {

std::atomic<long> g_allocations{0};
thread_local bool t_counting = false;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

} // namespace

void* operator new(std::size_t size) {
    if (t_counting) {
        g_allocations.fetch_add(1);
    }
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

bool OtherThreadCanLock(std::mutex& mutex) {
    bool locked = false;
    std::thread([&] {
        locked = mutex.try_lock();
        if (locked) {
            mutex.unlock();
        }
    }).join();
    return locked;
}

void TestNesting() {
    std::mutex mutex;
    {
        Common::RecursiveScopedLock outer{mutex};
        {
            Common::RecursiveScopedLock inner{mutex};
            Check(!OtherThreadCanLock(mutex), "the mutex is held inside a nested lock");
        }
        Check(!OtherThreadCanLock(mutex), "the inner release keeps the outer lock");
    }
    Check(OtherThreadCanLock(mutex), "the outermost release unlocks");

    std::mutex a;
    std::mutex b;
    {
        Common::RecursiveScopedLock lock_a{a};
        Common::RecursiveScopedLock lock_b{b};
        Common::RecursiveScopedLock again_a{a};
        Check(!OtherThreadCanLock(a) && !OtherThreadCanLock(b), "two mutexes are tracked independently");
    }
    Check(OtherThreadCanLock(a) && OtherThreadCanLock(b), "both are released");

    std::shared_mutex shared;
    {
        Common::RecursiveSharedLock first{shared};
        Common::RecursiveSharedLock second{shared};
        bool excluded = false;
        std::thread([&] { excluded = !shared.try_lock(); }).join();
        Check(excluded, "a nested shared lock still excludes a writer");
    }
    bool free = false;
    std::thread([&] {
        free = shared.try_lock();
        if (free) {
            shared.unlock();
        }
    }).join();
    Check(free, "the shared lock is released");
}

void TestNoAllocationAfterFirstLock() {
    std::mutex mutex;
    std::mutex other;
    {
        Common::RecursiveScopedLock warm{mutex};
        Common::RecursiveScopedLock warm_other{other};
    }
    t_counting = true;
    g_allocations = 0;
    for (int i = 0; i < 1000; ++i) {
        Common::RecursiveScopedLock outer{mutex};
        Common::RecursiveScopedLock inner{mutex};
        Common::RecursiveScopedLock second{other};
    }
    t_counting = false;
    Check(g_allocations.load() == 0, "a thread that has locked before allocates nothing on later lock cycles");
}

} // namespace

int main() {
    TestNesting();
    TestNoAllocationAfterFirstLock();
    std::printf("recursive lock: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
