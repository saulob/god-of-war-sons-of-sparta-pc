/* The umtx semaphore operations behind the sem_timedwait of CUSA57767 1.01 and the POSIX semaphore error contract, on the
 * production source (src/runtime_umtx.c). */
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define OP_SEM_WAIT 19
#define OP_SEM_WAKE 20
#define GUEST_EAGAIN 35
#define GUEST_EINVAL 22
#define GUEST_ETIMEDOUT 60

#undef printf
int runtime_log_printf(const char *format, ...) { (void)format; return 0; }
static _Thread_local int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int runtime_guest_errno(int host) {
    return host == EAGAIN ? GUEST_EAGAIN : host == ETIMEDOUT ? GUEST_ETIMEDOUT : host;
}
static int waits;
int runtime_thread_wait_enter(void (*wake)(void *, void *), void *first, void *second) {
    (void)wake; (void)first; (void)second;
    __atomic_add_fetch(&waits, 1, __ATOMIC_SEQ_CST);
    return 0;
}
void runtime_thread_wait_end(void) {}
void runtime_thread_wake_condition(void *first, void *second) { (void)first; (void)second; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
uintptr_t runtime_umtx_resolve(const char *name);

static int32_t (ABI *umtx_op)(void *, int, uint64_t, void *, void *);
static int32_t (ABI *sem_init_)(void *, int, unsigned);
static int32_t (ABI *sem_post_)(void *);
static int32_t (ABI *sem_trywait_)(void *);
static int32_t (ABI *sem_destroy_)(void *);

static int failures;
static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "ok" : "fail", name);
    failures += !condition;
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static uint32_t sem[4];
static int32_t timed_wait(int64_t sec, int64_t nsec) {
    int64_t timeout[2] = {sec, nsec};
    guest_errno = 0;
    return umtx_op(sem + 1, OP_SEM_WAIT, 0, NULL, timeout);
}

typedef struct { int wake_only; } Post;
static void *poster(void *arg) {
    const Post *post = arg;
    while (!__atomic_load_n(&waits, __ATOMIC_SEQ_CST)) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    if (post->wake_only) {
        __atomic_store_n(&sem[2], 1, __ATOMIC_SEQ_CST);
        umtx_op(sem + 1, OP_SEM_WAKE, 0, NULL, NULL);
    } else {
        sem_post_(sem);
    }
    return NULL;
}
static void wakes(const char *name, int wake_only) {
    Post post = {wake_only};
    pthread_t thread;
    sem_init_(sem, 0, 0);
    __atomic_store_n(&waits, 0, __ATOMIC_SEQ_CST);
    if (pthread_create(&thread, NULL, poster, &post)) { check(name, 0); return; }
    const double start = now_ms();
    const int32_t result = timed_wait(10, 0);
    const double elapsed = now_ms() - start;
    pthread_join(thread, NULL);
    check(name, result == 0 && elapsed < 5000 && sem[2] == 1);
}

int main(void) {
    umtx_op = (int32_t (ABI *)(void *, int, uint64_t, void *, void *))runtime_umtx_resolve("_umtx_op");
    sem_init_ = (int32_t (ABI *)(void *, int, unsigned))runtime_umtx_resolve("sem_init");
    sem_post_ = (int32_t (ABI *)(void *))runtime_umtx_resolve("sem_post");
    sem_trywait_ = (int32_t (ABI *)(void *))runtime_umtx_resolve("sem_trywait");
    sem_destroy_ = (int32_t (ABI *)(void *))runtime_umtx_resolve("sem_destroy");
    if (!umtx_op || !sem_init_ || !sem_post_ || !sem_trywait_ || !sem_destroy_) {
        puts("  [fail] the umtx exports are missing");
        return 1;
    }
    check("a semaphore is created", sem_init_(sem, 0, 0) == 0 && sem[2] == 0);

    const double start = now_ms();
    const int32_t result = timed_wait(0, 80000000);
    const double elapsed = now_ms() - start;
    check("an empty semaphore blocks until the timeout instead of returning at once", elapsed > 60 && waits > 0);
    check("the timeout returns -1 and the guest's ETIMEDOUT", result == -1 && guest_errno == GUEST_ETIMEDOUT);
    check("a timed-out wait posts nothing and marks the waiter", sem[2] == 0 && sem[3] == 0 && sem[1] == 1);

    sem_init_(sem, 0, 2);
    check("a wait on an available count returns at once without taking it", timed_wait(5, 0) == 0 && sem[2] == 2);
    wakes("sem_post ends the wait", 0);
    wakes("the wake operation ends the wait", 1);

    sem_init_(sem, 0, 1);
    guest_errno = 0;
    check("trywait takes an available count", sem_trywait_(sem) == 0 && sem[2] == 0);
    check("trywait on an empty semaphore returns -1 and the guest's EAGAIN", sem_trywait_(sem) == -1 && guest_errno == GUEST_EAGAIN);
    guest_errno = 0;
    check("a null semaphore is refused", sem_init_(NULL, 0, 0) == -1 && guest_errno == GUEST_EINVAL && sem_destroy_(NULL) == -1);
    printf("umtx tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
