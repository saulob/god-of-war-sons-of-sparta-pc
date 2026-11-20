/* scePthreadCancel on the production thread layer. Cancellation is deferred: the target leaves at its next cancellation
 * point or finishes on its own, the caller never waits for it, and the process never stops. Logging, keys and exit
 * hooks are stubbed; the guest thread code (create, join, detach, cancel, exit, sleep) is the real runtime_thread.c. */
#include "runtime.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ERR_SRCH ((int32_t)0x80020003)
#define ERR_INVAL ((int32_t)0x80020016)
#define CANCELED ((void *)1)
#define SELF_LIMIT_MS 3000

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) {
    (void)caller;
    printf("  [fail] the runtime ended the process (%s, status %d)\n", api, status);
    fflush(stdout);
    _Exit(status);
}
FILE *sos_stderr_stream(void) { return stdout; }
void sos_thread_label_set(const char *name) { (void)name; }
static volatile LONG thread_cleanups;
void runtime_thread_keys_cleanup(void) { InterlockedIncrement(&thread_cleanups); }
void runtime_signal_deliver_pending(void) {}
int runtime_signal_deliverable(void) { return 0; }
void runtime_module_tls_release(void) {}

typedef void *(ABI *Entry)(void *);
typedef void *(ABI *SelfFn)(void);
typedef int32_t (ABI *CreateFn)(void **, void **, Entry, void *, const char *);
typedef int32_t (ABI *JoinFn)(void *, void **);
typedef int32_t (ABI *DetachFn)(void *);
typedef int32_t (ABI *CancelFn)(void *);
typedef int32_t (ABI *StateFn)(int, int *);

static CreateFn create;
static JoinFn join;
static DetachFn detach;
static CancelFn cancel;
static StateFn set_state;
static int failures;

static void check(const char *name, int condition) {
    if (condition) {
        printf("  [ok]   %s\n", name);
    } else {
        printf("  [fail] %s\n", name);
        ++failures;
    }
}
static void *resolve(const char *nid) { return (void *)runtime_thread_resolve(nid); }
static ULONGLONG now_ms(void) { return GetTickCount64(); }

typedef struct Job {
    volatile LONG started;
    volatile LONG release;
    volatile LONG reached_end;
    volatile LONG disabled_phase_ms;
} Job;

static int wait_flag(volatile LONG *flag, int limit_ms) {
    for (int waited = 0; !InterlockedCompareExchange(flag, 1, 1) && waited < limit_ms; ++waited) Sleep(1);
    return InterlockedCompareExchange(flag, 1, 1) != 0;
}

/* A guest thread parked in sleeps, which are cancellation points. */
static ABI void *sleeper(void *argument) {
    Job *job = argument;
    InterlockedExchange(&job->started, 1);
    const ULONGLONG begin = now_ms();
    while (!InterlockedCompareExchange(&job->release, 1, 1) && now_ms() - begin < SELF_LIMIT_MS) runtime_thread_sleep_ns(1000000);
    InterlockedExchange(&job->reached_end, 1);
    return (void *)0x5151;
}
/* Guest code that never reaches a cancellation point: only a natural finish can end it. */
static ABI void *spinner(void *argument) {
    Job *job = argument;
    InterlockedExchange(&job->started, 1);
    const ULONGLONG begin = now_ms();
    while (!InterlockedCompareExchange(&job->release, 1, 1) && now_ms() - begin < SELF_LIMIT_MS) Sleep(1);
    InterlockedExchange(&job->reached_end, 1);
    return (void *)0x7777;
}
static ABI void *quick(void *argument) {
    (void)argument;
    return (void *)0x1234;
}
/* Cancellation disabled for a while: the request waits, then takes effect at the first cancellation point. */
static ABI void *shielded(void *argument) {
    Job *job = argument;
    int old = 0;
    set_state(1, &old);
    InterlockedExchange(&job->started, 1);
    const ULONGLONG begin = now_ms();
    while (now_ms() - begin < 150) runtime_thread_sleep_ns(1000000);
    InterlockedExchange(&job->disabled_phase_ms, (LONG)(now_ms() - begin));
    set_state(0, &old);
    runtime_thread_sleep_ns(1000000);
    InterlockedExchange(&job->reached_end, 1);
    return (void *)0x6161;
}

static void start(void **thread, Entry entry, Job *job, const char *name) {
    memset(job, 0, sizeof(*job));
    *thread = NULL;
    if (create(thread, NULL, entry, job, name) != 0) *thread = NULL;
}

static void test_invalid_handles(void) {
    check("cancel of a null handle returns the guest error", cancel(NULL) == ERR_SRCH);
    check("cancel of a fake handle returns the guest error", cancel((void *)0x1234) == ERR_SRCH);
}

static void test_finished_thread(void) {
    void *thread;
    void *result = NULL;
    Job job;
    start(&thread, (Entry)quick, &job, "quick");
    check("a quick guest thread is created", thread != NULL);
    int32_t answer = 0;
    for (int tries = 0; tries < 2000; ++tries) {
        answer = cancel(thread);
        if (answer == ERR_SRCH) break;
        Sleep(1);
    }
    check("cancel of a thread that finished keeps the existing error", answer == ERR_SRCH);
    check("the finished thread is still joinable", join(thread, &result) == 0);
    check("its result is intact", result == (void *)0x1234);
}

static void test_running_thread_without_cancellation_points(void) {
    void *thread;
    void *result = NULL;
    Job job;
    start(&thread, (Entry)spinner, &job, "spinner");
    check("the spinner started", thread != NULL && wait_flag(&job.started, 2000));
    const LONG cleanups = thread_cleanups;
    const ULONGLONG begin = now_ms();
    const int32_t first = cancel(thread);
    const ULONGLONG elapsed = now_ms() - begin;
    check("cancel of a running thread succeeds", first == 0);
    check("cancel returns at once instead of waiting for the thread", elapsed < 500);
    check("a second cancel of the same thread is harmless", cancel(thread) == 0);
    Sleep(150);
    check("the thread was left running", !InterlockedCompareExchange(&job.reached_end, 1, 1) && thread_cleanups == cleanups);
    InterlockedExchange(&job.release, 1);
    check("the thread finishes on its own", wait_flag(&job.reached_end, 2000));
    check("it is still joinable after the cancel", join(thread, &result) == 0);
    check("a thread that was not interrupted keeps its own result", result == (void *)0x7777);
}

static void test_running_thread_with_cancellation_points(void) {
    void *thread;
    void *result = NULL;
    Job job;
    start(&thread, (Entry)sleeper, &job, "sleeper");
    check("the sleeper started", thread != NULL && wait_flag(&job.started, 2000));
    check("cancel of the sleeper succeeds", cancel(thread) == 0);
    const ULONGLONG begin = now_ms();
    check("the canceled thread is joined", join(thread, &result) == 0);
    check("it left at its cancellation point, soon", now_ms() - begin < 1500);
    check("it never reached the end of its own code", !InterlockedCompareExchange(&job.reached_end, 1, 1));
    check("its result is the cancellation result", result == CANCELED);
    check("a joined thread cannot be joined again", join(thread, &result) != 0);
}

static void test_disabled_cancel_state(void) {
    void *thread;
    void *result = NULL;
    Job job;
    start(&thread, (Entry)shielded, &job, "shielded");
    check("the shielded thread started", thread != NULL && wait_flag(&job.started, 2000));
    check("cancel of a thread with cancellation disabled succeeds", cancel(thread) == 0);
    check("the thread is joined", join(thread, &result) == 0);
    check("it ran through its disabled phase before the request took effect", job.disabled_phase_ms >= 100);
    check("the request took effect once cancellation was enabled", result == CANCELED && !job.reached_end);
}

static void test_detached_thread(void) {
    void *thread;
    Job job;
    start(&thread, (Entry)sleeper, &job, "detached");
    check("the detached target started", thread != NULL && wait_flag(&job.started, 2000));
    const LONG cleanups = thread_cleanups;
    check("the thread is detached", detach(thread) == 0);
    check("cancel of a running detached thread succeeds", cancel(thread) == 0);
    for (int tries = 0; tries < 2000 && thread_cleanups == cleanups; ++tries) Sleep(1);
    check("the detached thread finished through its normal cleanup", thread_cleanups == cleanups + 1);
    check("it never reached the end of its own code", !job.reached_end);
    int32_t late = 0;
    for (int tries = 0; tries < 2000; ++tries) {
        late = cancel(thread);
        if (late == ERR_SRCH) break;
        Sleep(1);
    }
    check("the detached thread is reaped and a later cancel finds nothing", late == ERR_SRCH);
    check("a reaped thread cannot be joined", join(thread, NULL) != 0);
}

static void test_cancel_races_with_completion(void) {
    int bad_join = 0, bad_result = 0, bad_cancel = 0, canceled = 0, natural = 0;
    for (int i = 0; i < 200; ++i) {
        void *thread;
        Job job;
        start(&thread, (Entry)((i & 1) ? sleeper : quick), &job, "race");
        if (!thread) { ++bad_join; continue; }
        if (i & 1) wait_flag(&job.started, 2000);
        if (i % 3 == 0) Sleep(0);
        const int32_t answer = cancel(thread);
        if (answer != 0 && answer != ERR_SRCH) ++bad_cancel;
        void *result = NULL;
        if (join(thread, &result) != 0) ++bad_join;
        if (result == CANCELED) ++canceled;
        else if (result == (void *)0x1234) ++natural;
        else ++bad_result;
        if (join(thread, &result) == 0) ++bad_join;
    }
    check("cancel racing a finish only ever answers success or the finished error", bad_cancel == 0);
    check("every raced thread is joined exactly once", bad_join == 0);
    check("every raced thread has either its own result or the cancellation result", bad_result == 0);
    check("both outcomes of the race happened", canceled > 0 && natural > 0);
}

#define MANY 16
typedef struct Canceller {
    void **threads;
    volatile LONG *go;
    volatile LONG bad;
} Canceller;
static DWORD WINAPI canceller_main(void *argument) {
    Canceller *c = argument;
    wait_flag(c->go, 2000);
    for (int i = 0; i < MANY; ++i) {
        const int32_t answer = cancel(c->threads[i]);
        if (answer != 0 && answer != ERR_SRCH) InterlockedIncrement(&c->bad);
    }
    return 0;
}
static void test_many_threads_finish_while_cancelling(void) {
    void *threads[MANY];
    Job jobs[MANY];
    for (int i = 0; i < MANY; ++i) start(&threads[i], (Entry)((i & 1) ? quick : sleeper), &jobs[i], "many");
    int created = 1;
    for (int i = 0; i < MANY; ++i) if (!threads[i]) created = 0;
    check("sixteen guest threads are created", created);
    if (!created) return;
    volatile LONG go = 0;
    Canceller canceller = {threads, &go, 0};
    HANDLE helpers[3];
    for (int i = 0; i < 3; ++i) helpers[i] = CreateThread(NULL, 0, canceller_main, &canceller, 0, NULL);
    InterlockedExchange(&go, 1);
    WaitForMultipleObjects(3, helpers, TRUE, 10000);
    for (int i = 0; i < 3; ++i) CloseHandle(helpers[i]);
    check("concurrent cancels of threads that are finishing only answer success or the finished error", canceller.bad == 0);
    int joined = 0, expected = 0;
    for (int i = 0; i < MANY; ++i) {
        void *result = NULL;
        if (join(threads[i], &result) != 0) continue;
        ++joined;
        if ((i & 1) ? result == (void *)0x1234 : result == CANCELED) ++expected;
    }
    check("every thread is joined once", joined == MANY);
    check("each result is its own or the cancellation result", expected == MANY);
}

typedef struct Joiner {
    void *thread;
    volatile LONG answer;
} Joiner;
static DWORD WINAPI joiner_main(void *argument) {
    Joiner *joiner = argument;
    InterlockedExchange(&joiner->answer, (LONG)join(joiner->thread, NULL) + 1000000);
    return 0;
}
static void test_concurrent_join_has_one_owner(void) {
    void *thread;
    Job job;
    start(&thread, (Entry)sleeper, &job, "joined");
    check("the joined target started", thread != NULL && wait_flag(&job.started, 2000));
    Joiner joiner = {thread, 0};
    HANDLE helper = CreateThread(NULL, 0, joiner_main, &joiner, 0, NULL);
    Sleep(80);
    const int32_t second = join(thread, NULL);
    check("a second join while one is waiting is refused", second != 0);
    InterlockedExchange(&job.release, 1);
    WaitForSingleObject(helper, 5000);
    CloseHandle(helper);
    check("the first join completes", joiner.answer == 1000000);
}

int main(void) {
    runtime_thread_attach_main();
    create = (CreateFn)resolve("6UgtwV+0zb4#libkernel");
    join = (JoinFn)resolve("onNY9Byn-W8#libkernel");
    detach = (DetachFn)resolve("4qGrR6eoP9Y#libkernel");
    cancel = (CancelFn)resolve("qBDmpCyGssE#libkernel");
    set_state = (StateFn)resolve("lZzFeSxPl08#libkernel");
    if (!create || !join || !detach || !cancel || !set_state) {
        puts("  [fail] thread exports are missing");
        return 1;
    }
    test_invalid_handles();
    test_finished_thread();
    test_running_thread_without_cancellation_points();
    test_running_thread_with_cancellation_points();
    test_disabled_cancel_state();
    test_detached_thread();
    test_cancel_races_with_completion();
    test_many_threads_finish_while_cancelling();
    test_concurrent_join_has_one_owner();
    printf("thread runtime tests: %d failures\n", failures);
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
