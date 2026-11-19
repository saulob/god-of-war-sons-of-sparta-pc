/* FreeBSD _umtx_op and the pthread/POSIX semaphores the guest libc builds on it. Unity's IL2CPP runtime and
 * GC depend on correct wait/wake semantics here, so this is a real futex-style implementation, not a no-op. */
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>

/* FreeBSD umtx op ids (sys/umtx.h). */
enum {
    UMTX_OP_LOCK = 0, UMTX_OP_UNLOCK = 1, UMTX_OP_WAIT = 2, UMTX_OP_WAKE = 3,
    UMTX_OP_MUTEX_TRYLOCK = 4, UMTX_OP_MUTEX_LOCK = 5, UMTX_OP_MUTEX_UNLOCK = 6,
    UMTX_OP_CMP_REL = 8, UMTX_OP_WAIT_UINT = 11, UMTX_OP_WAIT_UINT_PRIVATE = 12,
    UMTX_OP_WAKE_PRIVATE = 13, UMTX_OP_MUTEX_WAIT = 14, UMTX_OP_MUTEX_WAIT_PRIVATE = 15,
    UMTX_OP_MUTEX_WAKE = 16, UMTX_OP_MUTEX_WAKE_PRIVATE = 17, UMTX_OP_SEM_WAIT = 18,
    UMTX_OP_SEM_WAKE = 19, UMTX_OP_NWAKE_PRIVATE = 20,
};

/* Waiters hash by address into a fixed set of queues; a wake is a broadcast, and every waiter rechecks its word, so a
 * shared queue only costs a spurious wakeup. */
#define SLOTS 256
#define GUEST_ETIMEDOUT 60
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; } Queue;
static Queue queues[SLOTS];
static pthread_once_t queues_ready = PTHREAD_ONCE_INIT;
static void queues_init(void) {
    for (int i = 0; i < SLOTS; ++i) {
        pthread_mutex_init(&queues[i].lock, NULL);
        pthread_cond_init(&queues[i].cond, NULL);
    }
}
static Queue *queue_for(void *addr) {
    pthread_once(&queues_ready, queues_init);
    const uintptr_t key = (uintptr_t)addr >> 2;
    return &queues[(key ^ (key >> 8) ^ (key >> 16)) % SLOTS];
}

static uint32_t load32(void *addr) { return atomic_load((_Atomic uint32_t *)addr); }
static void store32(void *addr, uint32_t value) { atomic_store((_Atomic uint32_t *)addr, value); }
static int cas32(void *addr, uint32_t *expected, uint32_t desired) {
    return atomic_compare_exchange_strong((_Atomic uint32_t *)addr, expected, desired);
}

/* Waits on `addr` while the 32-bit word still reads `expect`, with an optional FreeBSD timespec. */
static int wait_on(void *addr, uint32_t expect, const void *timeout, int absolute) {
    Queue *q = queue_for(addr);
    struct timespec deadline;
    int has_deadline = 0;
    if (timeout) {
        const int64_t *ts = timeout;
        if (absolute) {
            deadline.tv_sec = (time_t)ts[0];
            deadline.tv_nsec = (long)ts[1];
        } else {
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += (time_t)ts[0];
            deadline.tv_nsec += (long)ts[1];
        }
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        has_deadline = 1;
    }
    pthread_mutex_lock(&q->lock);
    while (load32(addr) == expect) {
        runtime_thread_wait_enter(runtime_thread_wake_condition, &q->cond, &q->lock);
        int rc = has_deadline ? pthread_cond_timedwait(&q->cond, &q->lock, &deadline) : pthread_cond_wait(&q->cond, &q->lock);
        runtime_thread_wait_end();
        if (rc == ETIMEDOUT) { pthread_mutex_unlock(&q->lock); return -ETIMEDOUT; }
    }
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static void wake_on(void *addr, int count) {
    (void)count;
    Queue *q = queue_for(addr);
    pthread_mutex_lock(&q->lock);
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

/* UMTX_OP_LOCK/UNLOCK/MUTEX_*: the lock word stores the owner token. */
static ABI int32_t umtx_mutex_lock(void *obj, uint32_t owner) {
    for (;;) {
        uint32_t expected = 0;
        if (cas32(obj, &expected, owner)) return 0;
        wait_on(obj, expected, NULL, 0);
    }
}
static ABI int32_t umtx_mutex_trylock(void *obj, uint32_t owner) {
    uint32_t expected = 0;
    return cas32(obj, &expected, owner) ? 0 : EBUSY;
}
static ABI int32_t umtx_mutex_unlock(void *obj) {
    store32(obj, 0);
    wake_on(obj, 1);
    return 0;
}

static ABI int32_t umtx_cmp_rel(void *obj, uint32_t current, uint32_t delta, const void *timeout) {
    uint32_t expected = current;
    if (cas32(obj, &expected, current + delta)) return 0;
    wait_on(obj, expected, timeout, 0);
    return 0;
}

/* FreeBSD sem_t: magic at +0, kernel word at +4, count at +8, pshared at +12. */
#define SEM_MAGIC 0xffff736du
static void *sem_value_addr(void *sem) { return (unsigned char *)sem + 8; }
static ABI int32_t sem_wait_op(void *sem) {
    void *value_addr = sem_value_addr(sem);
    for (;;) {
        uint32_t value = load32(value_addr);
        if ((int32_t)value > 0 && cas32(value_addr, &value, value - 1)) return 0;
        wait_on(value_addr, 0, NULL, 0);
    }
}
static ABI int32_t sem_trywait_op(void *sem) {
    void *value_addr = sem_value_addr(sem);
    uint32_t value = load32(value_addr);
    return ((int32_t)value > 0 && cas32(value_addr, &value, value - 1)) ? 0 : EAGAIN;
}
static ABI int32_t sem_post_op(void *sem) {
    void *value_addr = sem_value_addr(sem);
    uint32_t value = load32(value_addr);
    while ((int32_t)value < 0x7fffffff) {
        if (cas32(value_addr, &value, value + 1)) break;
    }
    wake_on(value_addr, 1);
    return 0;
}
static ABI int32_t sem_init_op(void *sem, int pshared, unsigned int value) {
    if (!sem) return EINVAL;
    store32(sem, SEM_MAGIC);
    store32((unsigned char *)sem + 4, 0);
    store32(sem_value_addr(sem), value);
    store32((unsigned char *)sem + 12, (uint32_t)(pshared != 0));
    return 0;
}
static ABI int32_t sem_destroy_op(void *sem) {
    if (!sem) return EINVAL;
    wake_on(sem_value_addr(sem), 0);
    store32(sem, 0);
    return 0;
}

/* ScePthreadSem is a pointer kept in the caller's slot, unlike the inline POSIX sem_t. */
#define SCE_ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct {
    int32_t value;
    pthread_mutex_t lock;
    pthread_cond_t changed;
} SceSem;

static ABI int32_t sce_sem_init(SceSem **slot, int32_t flag, unsigned int value, const char *name) {
    (void)name;
    if (!slot || flag) return SCE_ERR(22);
    SceSem *sem = calloc(1, sizeof(*sem));
    if (!sem) return SCE_ERR(12);
    sem->value = value > 0x7fffffffu ? 0x7fffffff : (int32_t)value;
    pthread_mutex_init(&sem->lock, NULL);
    pthread_cond_init(&sem->changed, NULL);
    *slot = sem;
    return 0;
}
static SceSem *sce_sem_get(SceSem **slot) { return slot ? *slot : NULL; }
static ABI int32_t sce_sem_wait(SceSem **slot) {
    SceSem *sem = sce_sem_get(slot);
    if (!sem) return SCE_ERR(22);
    pthread_mutex_lock(&sem->lock);
    while (sem->value == 0) {
        runtime_thread_wait_enter(runtime_thread_wake_condition, &sem->changed, &sem->lock);
        pthread_cond_wait(&sem->changed, &sem->lock);
        runtime_thread_wait_end();
    }
    --sem->value;
    pthread_mutex_unlock(&sem->lock);
    return 0;
}
static ABI int32_t sce_sem_trywait(SceSem **slot) {
    SceSem *sem = sce_sem_get(slot);
    if (!sem) return SCE_ERR(22);
    pthread_mutex_lock(&sem->lock);
    const int acquired = sem->value > 0;
    if (acquired) --sem->value;
    pthread_mutex_unlock(&sem->lock);
    return acquired ? 0 : SCE_ERR(11);
}
static ABI int32_t sce_sem_timedwait(SceSem **slot, uint32_t usec) {
    SceSem *sem = sce_sem_get(slot);
    if (!sem) return SCE_ERR(22);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(usec / 1000000u);
    deadline.tv_nsec += (long)(usec % 1000000u) * 1000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&sem->lock);
    while (sem->value == 0) {
        runtime_thread_wait_enter(runtime_thread_wake_condition, &sem->changed, &sem->lock);
        const int rc = pthread_cond_timedwait(&sem->changed, &sem->lock, &deadline);
        runtime_thread_wait_end();
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&sem->lock);
            return SCE_ERR(60);
        }
    }
    --sem->value;
    pthread_mutex_unlock(&sem->lock);
    return 0;
}
static ABI int32_t sce_sem_post(SceSem **slot) {
    SceSem *sem = sce_sem_get(slot);
    if (!sem) return SCE_ERR(22);
    pthread_mutex_lock(&sem->lock);
    if (sem->value < 0x7fffffff) ++sem->value;
    pthread_cond_signal(&sem->changed);
    pthread_mutex_unlock(&sem->lock);
    return 0;
}
static ABI int32_t sce_sem_destroy(SceSem **slot) {
    SceSem *sem = sce_sem_get(slot);
    if (!sem) return 0;
    pthread_cond_destroy(&sem->changed);
    pthread_mutex_destroy(&sem->lock);
    free(sem);
    *slot = NULL;
    return 0;
}

static ABI int32_t umtx_op(void *obj, int op, uint64_t val, void *uaddr1, void *uaddr2) {
    if (!obj) return EINVAL;
    switch (op) {
    case UMTX_OP_LOCK: case UMTX_OP_MUTEX_LOCK:
        return umtx_mutex_lock(obj, (uint32_t)(val ? val : 1));
    case UMTX_OP_MUTEX_TRYLOCK:
        return umtx_mutex_trylock(obj, (uint32_t)(val ? val : 1));
    case UMTX_OP_UNLOCK: case UMTX_OP_MUTEX_UNLOCK:
        return umtx_mutex_unlock(obj);
    case UMTX_OP_WAIT: case UMTX_OP_WAIT_UINT: case UMTX_OP_WAIT_UINT_PRIVATE:
    case UMTX_OP_MUTEX_WAIT: case UMTX_OP_MUTEX_WAIT_PRIVATE:
    {
        /* uaddr1 is the size of a struct _umtx_time at uaddr2 (timespec, flags, clock id) or zero for a plain timespec. */
        const uint32_t *flags = uaddr1 && uaddr2 ? (const uint32_t *)((const unsigned char *)uaddr2 + 16) : NULL;
        return wait_on(obj, (uint32_t)(val & 0xffffffffu), uaddr2, flags && (*flags & 1)) ? GUEST_ETIMEDOUT : 0;
    }
    case UMTX_OP_WAKE: case UMTX_OP_WAKE_PRIVATE: case UMTX_OP_MUTEX_WAKE: case UMTX_OP_MUTEX_WAKE_PRIVATE:
        wake_on(obj, (int)val);
        return 0;
    case UMTX_OP_NWAKE_PRIVATE: {
        void *const *addresses = obj;
        for (unsigned long i = 0; i < val && addresses; ++i) wake_on(addresses[i], 0);
        return 0;
    }
    case UMTX_OP_CMP_REL:
        return umtx_cmp_rel(obj, (uint32_t)(val & 0xffffffffu), (uint32_t)((val >> 32) & 0xffffffffu), uaddr1);
    case UMTX_OP_SEM_WAIT:
        return sem_wait_op(obj);
    case UMTX_OP_SEM_WAKE:
        return sem_post_op(obj);
    default:
        return EINVAL;
    }
}

static ABI int32_t guest_sem_post(void *sem) { return sem_post_op(sem); }
static ABI int32_t guest_sem_wait(void *sem) { return sem_wait_op(sem); }
static ABI int32_t guest_sem_trywait(void *sem) { return sem_trywait_op(sem); }
static ABI int32_t guest_sem_destroy(void *sem) { return sem_destroy_op(sem); }
static ABI int32_t guest_sem_init(void *sem, int pshared, unsigned int value) { return sem_init_op(sem, pshared, value); }

static const RuntimeExport exports[] = {
    {"_umtx_op", umtx_op},
    {"sem_post", guest_sem_post}, {"sem_wait", guest_sem_wait},
    {"sem_trywait", guest_sem_trywait}, {"sem_destroy", guest_sem_destroy}, {"sem_init", guest_sem_init},
    {"scePthreadSemInit", sce_sem_init}, {"scePthreadSemWait", sce_sem_wait},
    {"scePthreadSemTrywait", sce_sem_trywait}, {"scePthreadSemDestroy", sce_sem_destroy},
    {"scePthreadSemPost", sce_sem_post}, {"scePthreadSemTimedwait", sce_sem_timedwait},
};
uintptr_t runtime_umtx_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
