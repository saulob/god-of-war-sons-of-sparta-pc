/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` and `mov rax, fs:[0x10]` into `mov rax, gs:[...]`, so the
 * guest TCB and its thread handle stay reachable while the host owns FS. On
 * Windows GS is the TEB and cannot be moved. The guest TCB and thread handle live
 * in two TEB TLS slots reserved once at loader startup, and each load is rewritten
 * to its slot. */
#define _GNU_SOURCE
#include "runtime.h"
#include "guest_entry.h"
#include "startup_profile.h"
#include "startup_phase.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define CANCELED_RESULT ((void *)1)
#define WAKE_ATTEMPTS 64
#define SLEEP_SLICE_NS UINT64_C(20000000)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700
#define SCHED_POLICY_FIFO 1
#define SCHED_POLICY_OTHER 2
#define SCHED_POLICY_RR 3
#define PRIO_FIFO_MIN 256
#define PRIO_FIFO_MAX 767
#define PRIO_OTHER_DEFAULT 900

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int32_t unique_id;
    int detached, finished, joined, host_owned, dtors_ran;
    int cancel_pending;
    int cancel_state;
    void *cleanup;
    void (*wake)(void *, void *);
    void *wake_first, *wake_second;
    uint64_t guest_pthread_pad;
    /* The 1.01 cancellation helper reads the guest pthread handle at +0xd0..+0xdc. */
    uint32_t guest_cancel_enable, guest_cancel_pending, guest_cancel_point, guest_cancel_async;
    pthread_mutex_t wait_lock;
    const char *wait_point;
    uintptr_t wait_object;
    uint64_t wait_since_ms;
    RuntimeThreadSignals signals;
    uintptr_t stack_low, stack_high;
    int ready;
#ifdef _WIN32
    void *native;
    sos_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
#else
    jmp_buf exit_jump;
    void *stack;
    size_t stack_size;
    int queued;
    struct GuestThread *next_to_reap;
#endif
    struct GuestThread *next;
} GuestThread;
_Static_assert(offsetof(GuestThread, guest_cancel_enable) == 0xd0, "guest pthread cancellation fields at +0xd0");
_Static_assert(offsetof(GuestThread, guest_cancel_pending) == 0xd4, "guest pthread cancellation fields at +0xd4");
_Static_assert(offsetof(GuestThread, guest_cancel_point) == 0xd8, "guest pthread cancellation fields at +0xd8");
_Static_assert(offsetof(GuestThread, guest_cancel_async) == 0xdc, "guest pthread cancellation fields at +0xdc");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER; /* with `lock`: a thread finished or a cancellation was requested */
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;
static int track_waits;
#ifndef _WIN32
/* Detached threads whose stacks can be reused once they have exited; the host threads stay joinable for that. */
static GuestThread *to_reap;
static void queue_for_reap_locked(GuestThread *t) {
    if (!t->detached || !t->finished || t->queued) return;
    t->queued=1; t->next_to_reap=to_reap; to_reap=t;
}
static void release_stack(GuestThread *t) {
    runtime_stack_release(t->stack,t->stack_size);
    t->stack=NULL;
}
static void reap_detached(void) {
    pthread_mutex_lock(&lock);
    GuestThread *list=to_reap;
    to_reap=NULL;
    pthread_mutex_unlock(&lock);
    for (GuestThread *t=list,*next;t;t=next) {
        next=t->next_to_reap;
        pthread_join(t->host,NULL);
        release_stack(t);
    }
}
#endif

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
#ifdef _WIN32
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live elsewhere and cannot be used. */
#ifdef SOS_TLS_TEST
DWORD sos_test_tls_alloc(void);
BOOL sos_test_tls_free(DWORD);
#define TLS_ALLOC() sos_test_tls_alloc()
#define TLS_FREE(index) sos_test_tls_free(index)
#else
#define TLS_ALLOC() TlsAlloc()
#define TLS_FREE(index) TlsFree(index)
#endif
static DWORD tls_slot=TLS_OUT_OF_INDEXES;
static DWORD thread_slot=TLS_OUT_OF_INDEXES;
static INIT_ONCE tls_once=INIT_ONCE_STATIC_INIT;
static int tls_state;
static const char *tls_failed_role;
static DWORD tls_failed_index;
static BOOL CALLBACK reserve_tls_slots(PINIT_ONCE once,PVOID parameter,PVOID *context) {
    (void)once; (void)parameter; (void)context;
    const DWORD tcb=TLS_ALLOC();
    if (tcb==TLS_OUT_OF_INDEXES || tcb>=64) {
        tls_failed_role="guest TCB"; tls_failed_index=tcb; tls_state=-1;
        return TRUE;
    }
    const DWORD thread=TLS_ALLOC();
    if (thread==TLS_OUT_OF_INDEXES || thread>=64) {
        TLS_FREE(tcb);
        tls_failed_role="guest thread"; tls_failed_index=thread; tls_state=-1;
        return TRUE;
    }
    tls_slot=tcb; thread_slot=thread; tls_state=1;
    printf("Runtime: reserved guest TCB TEB TLS slots %lu and %lu\n",(unsigned long)tcb,(unsigned long)thread);
    return TRUE;
}
void runtime_thread_reserve_tls(void) {
    InitOnceExecuteOnce(&tls_once,reserve_tls_slots,NULL,NULL);
    if (tls_state==1) return;
    fprintf(stderr,"STOP: %s TEB TLS slot index %lu is not inline (<64)\n",
            tls_failed_role ? tls_failed_role : "guest",(unsigned long)tls_failed_index);
    runtime_exit(21);
}
uint32_t runtime_thread_tcb_offset(void) {
    runtime_thread_reserve_tls();
    return 0x1480+8*(uint32_t)tls_slot;
}
uint32_t runtime_thread_thread_offset(void) {
    runtime_thread_reserve_tls();
    return 0x1480+8*(uint32_t)thread_slot;
}
static void set_gs(void *base) {
    runtime_thread_reserve_tls();
    if (!TlsSetValue(tls_slot,base)) { fprintf(stderr,"STOP: guest TCB TlsSetValue failed (%lu)\n",GetLastError()); runtime_exit(21); }
}
static void set_thread_slot(void *thread) {
    runtime_thread_reserve_tls();
    if (!TlsSetValue(thread_slot,thread)) { fprintf(stderr,"STOP: guest thread TlsSetValue failed (%lu)\n",GetLastError()); runtime_exit(21); }
}
#define tls_block_alloc(size) _aligned_malloc(size,64)
#define tls_block_free(block) _aligned_free(block)
#else
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); runtime_exit(21); }
}
#define tls_block_alloc(size) aligned_alloc(64,size)
#define tls_block_free(block) free(block)
#endif
/* The reserved stack of the calling thread, whole: collectors scan from the stack pointer up to the base they are given. */
static void current_stack_bounds(uintptr_t *low,uintptr_t *high) {
#ifdef _WIN32
    const uintptr_t base=(uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    MEMORY_BASIC_INFORMATION region;
    *high=base;
    *low=VirtualQuery((LPCVOID)(base-1),&region,sizeof(region)) ? (uintptr_t)region.AllocationBase : base-DEFAULT_STACK;
#else
    pthread_attr_t attr;
    void *address=NULL;
    size_t size=0;
    if (!pthread_getattr_np(pthread_self(),&attr)) { pthread_attr_getstack(&attr,&address,&size); pthread_attr_destroy(&attr); }
    *low=(uintptr_t)address; *high=(uintptr_t)address+size;
#endif
}
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=tls_block_alloc((total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); runtime_exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    current_stack_bounds(&t->stack_low,&t->stack_high);
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&t->native,0,FALSE,DUPLICATE_SAME_ACCESS);
    set_thread_slot(t);
#endif
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
    static _Atomic int32_t next_unique_id=1;
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->unique_id=atomic_fetch_add(&next_unique_id,1);
    pthread_mutex_init(&t->wait_lock,NULL);
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); runtime_exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
}
#ifndef _WIN32
/* Collectors scan from the stack pointer to the base the guest reads back, so it must be the stack in use. */
void *runtime_thread_enter_on_stack(void *entry,void *arg0,void *arg1,void *stack,size_t size) {
    GuestThread *t=runtime_thread_current();
    const uintptr_t host_low=t->stack_low, host_high=t->stack_high;
    t->stack_low=(uintptr_t)stack; t->stack_high=(uintptr_t)stack+size;
    void *result=sos_guest_enter(entry,arg0,arg1,(unsigned char *)stack+size-64);
    t->stack_low=host_low; t->stack_high=host_high;
    return result;
}
#endif
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static GuestThread *find_live_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle && !t->finished) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static int policy_valid(int policy) { return policy>=SCHED_POLICY_FIFO && policy<=SCHED_POLICY_RR; }
static int policy_default_prio(int policy) { return policy==SCHED_POLICY_OTHER ? PRIO_OTHER_DEFAULT : DEFAULT_PRIO; }
static int policy_prio_valid(int policy,int prio) {
    return policy==SCHED_POLICY_OTHER || (prio>=PRIO_FIFO_MIN && prio<=PRIO_FIFO_MAX);
}
static GuestThread *sched_thread(void *handle) {
    if (!handle) return NULL;
    if (handle==(void *)current) return current;
    return find_live_thread(handle);
}
static int32_t sched_get_state(GuestThread *t,int *policy,int *prio) {
    if (!policy || !prio) return EINVAL;
    GuestThread *thread=sched_thread(t);
    if (!thread) return ESRCH;
    *policy=thread->attr.policy; *prio=thread->attr.prio;
    return 0;
}
static int32_t sched_set_state(GuestThread *t,int policy,int prio) {
    GuestThread *thread=sched_thread(t);
    if (!thread) return ESRCH;
    thread->attr.policy=policy; thread->attr.prio=prio;
    return 0;
}
static int32_t sched_get_priority(GuestThread *t,int *prio) {
    if (!prio) return EINVAL;
    GuestThread *thread=sched_thread(t);
    if (!thread) return ESRCH;
    *prio=thread->attr.prio;
    return 0;
}
static int32_t sched_set_priority(GuestThread *t,int prio) {
    GuestThread *thread=sched_thread(t);
    if (!thread) return ESRCH;
    thread->attr.prio=prio;
    return 0;
}
static int32_t sched_set_affinity(GuestThread *t,uint64_t mask) {
    GuestThread *thread=sched_thread(t);
    if (!thread) return ESRCH;
    thread->attr.affinity=mask;
    return 0;
}
static int32_t sched_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!mask) return EINVAL;
    GuestThread *thread=sched_thread(t);
    if (!thread) return ESRCH;
    *mask=thread->attr.affinity;
    return 0;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
const char *runtime_thread_name(void *handle) {
    GuestThread *t=find_thread(handle);
    return t ? t->name : NULL;
}
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    if (t->stack_high>t->stack_low) a->stack=t->stack_high-t->stack_low;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
/* scePthreadAttrGetstack: the host allocates guest stacks, so the address is the calling
 * thread's own stack base (callers query the attributes of the thread they run on). */
static ABI int32_t attr_get_stack_full(ThreadAttr **slot,void **address,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !address || !size) return ERR(22);
    GuestThread *t=runtime_thread_current();
    *address=(void *)t->stack_low;
    *size=t->stack_high>t->stack_low ? t->stack_high-t->stack_low : a->stack;
    return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    if (!policy_valid(policy)) return ERR(45);
    a->policy=policy;
    a->prio=policy_default_prio(policy);
    return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    if (!param) return ERR(45);
    if (!policy_prio_valid(a->policy,*param)) return ERR(45);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    sos_thread_label_set(name);
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
#ifdef _WIN32
    wchar_t wide[64]={0};
    MultiByteToWideChar(CP_UTF8,0,name,-1,wide,63);
    SetThreadDescription(GetCurrentThread(),wide);
#endif
}
#ifdef _WIN32
#define exit_setjmp(t) sos_setjmp((t)->exit_jump)
#define exit_longjmp(t) sos_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
static void wake_waiter(GuestThread *t) {
    pthread_mutex_lock(&t->wait_lock);
    if (t->wake) t->wake(t->wake_first,t->wake_second);
    pthread_mutex_unlock(&t->wait_lock);
}
static int cancel_requested(const GuestThread *t) {
    return t && !t->cancel_state && __atomic_load_n(&t->cancel_pending,__ATOMIC_ACQUIRE);
}
static __attribute__((noreturn)) void cancel_exit(GuestThread *t,const char *point) {
    printf("Runtime: thread '%s' exited by cancellation at %s\n",t->name,point);
    __atomic_store_n(&t->wait_point,NULL,__ATOMIC_RELEASE);
    t->result=CANCELED_RESULT;
    exit_longjmp(t);
}
/* Wakes a waiter blocked on `cond` with `mutex` held, so it is already waiting rather than on its way in. */
void runtime_thread_wake_condition(void *cond,void *mutex) {
    for (int attempt=0;attempt<WAKE_ATTEMPTS;++attempt) {
        if (!pthread_mutex_trylock(mutex)) {
            pthread_cond_broadcast(cond);
            pthread_mutex_unlock(mutex);
            return;
        }
        sched_yield();
    }
    pthread_cond_broadcast(cond);
}
int runtime_thread_cancel_pending(void) {
    GuestThread *t=current;
    return t && !t->host_owned && cancel_requested(t);
}
void runtime_thread_test_cancel(const char *point) {
    if (runtime_thread_cancel_pending()) cancel_exit(current,point);
}
static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
}
void runtime_thread_track_waits(int enabled) { __atomic_store_n(&track_waits,enabled,__ATOMIC_RELEASE); }
int runtime_thread_tracking_waits(void) { return __atomic_load_n(&track_waits,__ATOMIC_RELAXED); }
void runtime_thread_note_wait(const char *point,uintptr_t object) {
    GuestThread *t=current;
    if (!t || !__atomic_load_n(&track_waits,__ATOMIC_ACQUIRE)) return;
    if (point) {
        __atomic_store_n(&t->wait_object,object,__ATOMIC_RELAXED);
        __atomic_store_n(&t->wait_since_ms,monotonic_ms(),__ATOMIC_RELAXED);
    }
    __atomic_store_n(&t->wait_point,point,__ATOMIC_RELEASE);
}
/* The wake callback must make the blocked wait return. It runs with the thread's wait lock held, so the waiter's
 * storage outlives it, and it must not block. Returns nonzero when a cancellation is already pending. */
int runtime_thread_wait_enter(void (*wake)(void *,void *),void *first,void *second) {
    GuestThread *t=current;
    if (!t) return 0;
    pthread_mutex_lock(&t->wait_lock);
    t->wake=wake; t->wake_first=first; t->wake_second=second;
    pthread_mutex_unlock(&t->wait_lock);
    return !t->host_owned && cancel_requested(t);
}
/* Called right after the condition wait returns, with the waiter's mutex held. Guest signals are delivered here with that
 * mutex released, as the kernel runs a handler between an interrupted wait and its restart; callers recheck their
 * condition, so the wait restarts. */
void runtime_thread_wait_end(void) {
    GuestThread *t=current;
    if (!t) return;
    pthread_mutex_lock(&t->wait_lock);
    void (*wake)(void *,void *)=t->wake;
    pthread_mutex_t *mutex=t->wake_second;
    t->wake=NULL;
    pthread_mutex_unlock(&t->wait_lock);
    if (wake!=runtime_thread_wake_condition || !runtime_signal_deliverable()) return;
    pthread_mutex_unlock(mutex);
    runtime_signal_deliver_pending();
    pthread_mutex_lock(mutex);
}
void runtime_thread_kick(void *handle) {
    GuestThread *t=find_live_thread(handle);
    if (t) wake_waiter(t);
}
void runtime_thread_wait_begin(const char *point,void (*wake)(void *,void *),void *first,void *second) {
    if (!runtime_thread_wait_enter(wake,first,second)) return;
    runtime_thread_wait_end();
    cancel_exit(current,point);
}

static void host_sleep(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    while (nanosleep(&t,&t) && errno==EINTR) {}
#endif
}
/* Sleeps in slices so a cancellation request is seen within one slice. */
void runtime_thread_sleep_ns(uint64_t ns) {
    const int64_t cost=sos_cost_begin();
    const uint64_t asked=ns;
    runtime_thread_test_cancel("sleep");
    runtime_thread_note_wait("sleep",(uintptr_t)(ns/1000));
    while (ns>SLEEP_SLICE_NS) {
        host_sleep(SLEEP_SLICE_NS);
        ns-=SLEEP_SLICE_NS;
        runtime_thread_test_cancel("sleep");
        if (runtime_signal_deliverable()) runtime_signal_deliver_pending();
    }
    host_sleep(ns);
    runtime_thread_note_wait(NULL,0);
    sos_cost_end(SOS_COST_SLEEP,cost,asked);
    runtime_thread_test_cancel("sleep");
    runtime_signal_deliver_pending();
}
static __attribute__((noinline)) void scrub_stack(void) {
    char buffer[16*1024];
    memset(buffer,0,sizeof(buffer));
    __asm__ volatile("" :: "r"(buffer) : "memory");
}
typedef ABI void (*GuestDtors)(void);
static GuestDtors thread_dtors;
static size_t dtors_registered, dtors_run;
static void *thread_count_callback, *thread_report_callback;
static size_t atexit_open;
/* libc registers its thread-local destructor routine once; it runs as each guest thread ends, on that thread and
 * before its guest TLS is released. */
static ABI int32_t set_thread_dtors(GuestDtors dtors) {
    GuestDtors previous=__atomic_exchange_n(&thread_dtors,dtors,__ATOMIC_ACQ_REL);
    if (previous!=dtors) {
        pthread_mutex_lock(&lock); ++dtors_registered; pthread_mutex_unlock(&lock);
        printf("Runtime: thread destructor routine registered: %p\n",(void *)dtors);
    }
    return 0;
}
static ABI int32_t set_thread_atexit_count(void *callback) {
    __atomic_store_n(&thread_count_callback,callback,__ATOMIC_RELEASE);
    return 0;
}
static ABI int32_t set_thread_atexit_report(void *callback) {
    __atomic_store_n(&thread_report_callback,callback,__ATOMIC_RELEASE);
    return 0;
}
static ABI int32_t thread_atexit_increment(void) {
    __atomic_add_fetch(&atexit_open,1,__ATOMIC_RELAXED);
    return 0;
}
static ABI int32_t thread_atexit_decrement(void) {
    size_t open=__atomic_load_n(&atexit_open,__ATOMIC_RELAXED);
    while (open && !__atomic_compare_exchange_n(&atexit_open,&open,open-1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    return 0;
}
static void run_thread_dtors(GuestThread *t) {
    if (t->dtors_ran) return;
    t->dtors_ran=1;
    GuestDtors dtors=__atomic_load_n(&thread_dtors,__ATOMIC_ACQUIRE);
    if (!dtors) return;
    dtors();
    pthread_mutex_lock(&lock); ++dtors_run; pthread_mutex_unlock(&lock);
}
#ifdef _WIN32
/* A stack Windows placed above guest memory: guest code on a stack inside guest memory is then below StackLimit, and
 * the CRT's __chkstk walks from StackLimit through the guard page down to it. Such a thread's stack is committed in
 * full and its StackLimit moved to the start of guest memory. */
static void widen_stack_limit(void) {
    const uintptr_t guest_low=UINT64_C(0x1000000000);
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    const uintptr_t limit=(uintptr_t)tib->StackLimit, base=(uintptr_t)tib->StackBase;
    if (limit<=guest_low) return;
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery((LPCVOID)(base-1),&region,sizeof(region))) return;
    const uintptr_t reservation=(uintptr_t)region.AllocationBase;
    if (reservation<limit) {
        DWORD old;
        if (!VirtualAlloc((void *)reservation,limit-reservation,MEM_COMMIT,PAGE_READWRITE) ||
            !VirtualProtect((void *)reservation,limit-reservation,PAGE_READWRITE,&old)) {
            fprintf(stderr,"Runtime: thread stack at 0x%llx could not be committed (%lu)\n",(unsigned long long)base,
                    GetLastError());
            return;
        }
    }
    tib->StackLimit=(PVOID)guest_low;
}
#endif
static const char *const worker_families[] = {
    "AssetGarbageCollectorHelper", "Job.Worker", "Background Job.Worker", "SceFiosZlib",
};
static unsigned family_created[sizeof(worker_families)/sizeof(*worker_families)];
/* Worker families keep their first three full creation lines; the rest are counted at the STOP report. */
static int log_thread_creation(const char *name) {
    for (size_t i=0;i<sizeof(worker_families)/sizeof(*worker_families);++i) {
        const size_t length=strlen(worker_families[i]);
        if (strncmp(name,worker_families[i],length)) continue;
        const unsigned count=__atomic_fetch_add(&family_created[i],1,__ATOMIC_RELAXED);
        if (count<3) return 1;
        if (count==3) printf("Runtime: thread family '%s': further creations summarized\n",worker_families[i]);
        return 0;
    }
    return 1;
}
static void *host_start(void *p) {
    GuestThread *t=p;
#ifdef _WIN32
    widen_stack_limit();
#endif
    attach(t);
    set_host_name(t->name);
    scrub_stack();
    /* A creator that got the handle may signal the thread at once: it is announced only once it can take the signal. */
    pthread_mutex_lock(&lock); t->ready=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
    runtime_signal_deliver_pending();
    if (!exit_setjmp(t)) t->result=sos_guest_enter((void *)t->entry,t->argument,NULL,NULL);
    run_thread_dtors(t);
    runtime_thread_keys_cleanup();
    runtime_module_tls_release();
#ifdef _WIN32
    set_thread_slot(NULL);
#endif
    set_gs(NULL);
    tls_block_free(t->tls_block);
    t->tls_block=NULL; t->tcb=NULL;
#ifdef _WIN32
    compat_sleep_release();
#endif
    pthread_mutex_lock(&lock); t->finished=1; ++exited;
#ifndef _WIN32
    queue_for_reap_locked(t);
#endif
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!entry) {
        fprintf(stderr,"STOP: guest thread '%s' has a null entry\n",name ? name : "guest");
        runtime_exit(21);
    }
    if (!out) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    const int64_t cost=sos_cost_begin();
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
#ifdef _WIN32
    /* Windows places the stack itself (low: no high-entropy ASLR) and commits all of it:
     * guest code has no stack probes and would step over the guard page. */
    pthread_attr_setstacksize(&host,stack_bytes);
#else
    reap_detached();
    stack_bytes=(stack_bytes+16383)&~(size_t)16383;
    t->stack=runtime_stack_acquire(stack_bytes);
    if (t->stack) { t->stack_size=stack_bytes; pthread_attr_setstack(&host,t->stack,stack_bytes); }
    else pthread_attr_setstacksize(&host,stack_bytes);
#endif
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); runtime_exit(21); }
#ifdef _WIN32
    if (t->detached) pthread_detach(t->host);
#endif
    pthread_mutex_lock(&lock);
    while (!t->ready) pthread_cond_wait(&changed,&lock);
    ++created;
    pthread_mutex_unlock(&lock);
    sos_cost_end(SOS_COST_THREAD_CREATE,cost,1);
    if (log_thread_creation(t->name))
        printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d, entry=%p, arg=%p)\n",
               t->name,(unsigned long long)stack,t->attr.prio,(void *)t->entry,t->argument);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    GuestThread *self=current;
    if (t==self) return ERR(11);
    runtime_thread_test_cancel("join");
    pthread_mutex_lock(&lock);
    if (t->detached || t->joined) { pthread_mutex_unlock(&lock); return ERR(22); }
    t->joined=1;
    runtime_thread_note_wait("join",(uintptr_t)t->unique_id);
    while (!t->finished && !(self && cancel_requested(self))) pthread_cond_wait(&changed,&lock);
    if (!t->finished) {
        t->joined=0;
        pthread_mutex_unlock(&lock);
        cancel_exit(self,"join");
    }
    pthread_mutex_unlock(&lock);
    runtime_thread_note_wait(NULL,0);
    void *value=NULL;
    int e=pthread_join(t->host,&value);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); runtime_exit(21); }
    if (result) *result=t->result;
#ifndef _WIN32
    if (t->stack) release_stack(t);
#endif
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
#ifdef _WIN32
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
#else
    /* A joined host thread must not reach the reaper, which joins it again. */
    if (t->joined) return ERR(22);
    pthread_mutex_lock(&lock);
    t->detached=1;
    queue_for_reap_locked(t);
    pthread_mutex_unlock(&lock);
#endif
    return 0;
}
/* Deferred cancellation: the target exits at its next cancellation point (join, condition, semaphore or event flag
 * wait, sleep). */
static ABI int32_t thread_cancel(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->host_owned) return ERR(1);
    int32_t result=0, newly=0;
    pthread_mutex_lock(&lock);
    if (t->finished) result=ERR(3);
    else if (!t->cancel_pending) {
        __atomic_store_n(&t->cancel_pending,1,__ATOMIC_RELEASE);
        newly=1;
        pthread_cond_broadcast(&changed);
    }
    pthread_mutex_unlock(&lock);
    if (newly) {
        printf("Runtime: thread '%s' cancellation requested\n",t->name);
        wake_waiter(t);
    }
    return result;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); runtime_exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    exit_longjmp(t);
}
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    const int32_t r=sched_get_priority(t,prio);
    return r ? ERR(r) : 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    const int32_t r=sched_set_priority(t,prio);
    return r ? ERR(r) : 0;
}
static ABI int32_t thread_set_schedparam(GuestThread *t,int policy,const int *param) {
    if (!param) return 22;
    return sched_set_state(t,policy,*param);
}
static ABI int32_t sce_thread_set_schedparam(GuestThread *t,int policy,const int *param) {
    const int32_t r=thread_set_schedparam(t,policy,param);
    return r ? ERR(r) : 0;
}
static ABI int32_t thread_get_schedparam(GuestThread *t,int *policy,int *prio) {
    return sched_get_state(t,policy,prio);
}
static ABI int32_t sce_thread_get_schedparam(GuestThread *t,int *policy,int *prio) {
    const int32_t r=thread_get_schedparam(t,policy,prio);
    return r ? ERR(r) : 0;
}
/* Visits every live thread under the registry lock; a nonzero return from `visit` also wakes that thread's wait. */
void runtime_thread_signal_scan(int (*visit)(RuntimeThreadSignals *,void *native)) {
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) {
        if (t->finished) continue;
#ifdef _WIN32
        void *native=t->native;
#else
        void *native=t->ready || t->host_owned ? (void *)&t->host : NULL;
#endif
        if (visit(&t->signals,native)) wake_waiter(t);
    }
    pthread_mutex_unlock(&lock);
}
RuntimeThreadSignals *runtime_thread_signals(void *handle) {
    GuestThread *t=handle ? find_live_thread(handle) : runtime_thread_current();
    return t ? &t->signals : NULL;
}
void *runtime_thread_native_handle(void *handle) {
    GuestThread *t=handle ? find_live_thread(handle) : runtime_thread_current();
#ifdef _WIN32
    return t ? t->native : NULL;
#else
    return t && (t->ready || t->host_owned) ? (void *)&t->host : NULL;
#endif
}
static ABI int32_t thread_get_id(void) { return current ? current->unique_id : 0; }
int32_t runtime_thread_id_current(void) { return current ? current->unique_id : 0; }
const char *runtime_thread_current_name(void) { return current ? current->name : NULL; }
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    const int32_t r=sched_set_affinity(t,mask);
    return r ? ERR(r) : 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    const int32_t r=sched_get_affinity(t,mask);
    return r ? ERR(r) : 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }
static ABI int32_t set_cancel_state(int state,int *old) {
    if (state!=0 && state!=1) return 22;
    GuestThread *t=runtime_thread_current();
    if (old) *old=t->cancel_state;
    t->cancel_state=state;
    return 0;
}
/* FreeBSD's _pthread_cleanup_info is eight uintptr_t slots; the fields below fit in it. */
typedef struct GuestCleanup {
    struct GuestCleanup *prev;
    void (*routine)(void *);
    void *argument;
    int on_heap;
} GuestCleanup;
static ABI void cleanup_push(void (*routine)(void *),void *argument,GuestCleanup *info) {
    if (!info) return;
    GuestThread *t=runtime_thread_current();
    info->prev=(GuestCleanup *)t->cleanup;
    info->routine=routine;
    info->argument=argument;
    info->on_heap=0;
    t->cleanup=info;
}
static ABI void cleanup_pop(int execute) {
    GuestThread *t=runtime_thread_current();
    GuestCleanup *old=(GuestCleanup *)t->cleanup;
    if (!old) return;
    t->cleanup=old->prev;
    if (execute && old->routine) old->routine(old->argument);
}

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#libkernel",thread_self}, {"EotR8a3ASf4#libkernel",thread_self},
        {"9BcDykPmo1I#libkernel",guest_error},
        {"nsYoNRywwNg#libkernel",attr_init}, {"62KCwEMmzcM#libkernel",attr_destroy},
        {"x1X76arYMxU#libkernel",attr_get}, {"8+s5BzZjxSg#libkernel",attr_affinity},
        {"UTXzJbWhhTE#libkernel",attr_set_stack}, {"-Wreprtu0Qs#libkernel",attr_set_detach},
        {"4+h9EzwKF4I#libkernel",attr_set_policy}, {"DzES9hQF4f4#libkernel",attr_set_param},
        {"3qxgM4ezETA#libkernel",attr_set_affinity}, {"eXbUSpEaTsA#libkernel",attr_set_inherit},
        {"JaRMy+QcpeU#libkernel",attr_get_detach}, {"-fA+7ZlGDQs#libkernel",attr_get_stack},
        {"-quPa4SEJUw#libkernel",attr_get_stack_full},
        {"FXPWHNk8Of0#libkernel",attr_get_param}, {"El+cQ20DynU#libkernel",attr_set_guard},
        {"6UgtwV+0zb4#libkernel",thread_create}, {"onNY9Byn-W8#libkernel",thread_join},
        {"4qGrR6eoP9Y#libkernel",thread_detach}, {"qBDmpCyGssE#libkernel",thread_cancel}, {"3kg7rT0NQIs#libkernel",thread_exit},
        {"1xvtUVx1-Sg#libkernel",cleanup_push}, {"iWsFlYMf3Kw#libkernel",cleanup_pop},
        {"lZzFeSxPl08#libkernel",set_cancel_state},
        {"FIs3-UQT9sg#libkernel",thread_get_schedparam},
        {"P41kTWUS3EI#libkernel",sce_thread_get_schedparam},
        {"T72hz6ffq08#libkernel",thread_yield}, {"1tKyG7RlMJo#libkernel",thread_get_prio},
        {"oIRFTjoILbg#libkernel",sce_thread_set_schedparam},
        {"Xs9hdiD7sAA#libkernel",thread_set_schedparam},
        {"EI-5-jlq2dE#libkernel",thread_get_id},
        {"W0Hpm2X0uPE#libkernel",thread_set_prio}, {"bt3CTBKmGyI#libkernel",thread_set_affinity}, {"rcrVFJsQWRY#libkernel",thread_get_affinity},
        {"GBUY7ywdULE#libkernel",thread_rename}, {"3PtV6p3QNX4#libkernel",thread_equal},
        {"wtkt-teR1so#libkernel",posix_attr_init}, {"zHchY8ft5pk#libkernel",posix_attr_destroy},
        {"E+tyo3lp5Lw#libkernel",posix_attr_set_detach}, {"2Q0z6rnBrTE#libkernel",posix_attr_set_stack},
        {"euKRgm0Vn2M#libkernel",posix_attr_set_param}, {"OxhIB8LB-PQ#libkernel",posix_create},
        {"Jmi+9w9u0E4#libkernel",posix_create_name}, {"h9CcP3J0oVM#libkernel",posix_join},
        {"FJrT5LuUBAU#libkernel",thread_exit},
        {"rNhWz+lvOMU#libkernel",set_thread_dtors}, {"pB-yGZ2nQ9o#libkernel",set_thread_atexit_count},
        {"WhCc1w3EhSI#libkernel",set_thread_atexit_report}, {"Tz4RNUCBbGI#libkernel",thread_atexit_increment},
        {"8OnWXlgQlvo#libkernel",thread_atexit_decrement},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, live=%zu, exited=%zu, joined=%zu\n",
           created,created>=exited?created-exited:0,exited,joined_count);
    pthread_mutex_unlock(&lock);
    printf("Runtime: current guest thread=%s\n",current ? current->name : "none");
}
#define DIAG_THREADS 192
/* Copies under the registry lock and prints after it; gives up rather than wait on a lock a stuck thread may hold. */
void runtime_thread_diag_dump(void) {
    static struct { char name[32]; const char *point; uintptr_t object; uint64_t since; int finished, host; } rows[DIAG_THREADS];
    static pthread_mutex_t rows_lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&rows_lock);
    int locked=0;
    for (int attempt=0;attempt<50 && !(locked=!pthread_mutex_trylock(&lock));++attempt) host_sleep(2000000);
    if (!locked) { pthread_mutex_unlock(&rows_lock); puts("BootDiag: thread registry busy, no thread list"); return; }
    size_t count=0, total=0;
    for (GuestThread *t=threads;t;t=t->next,++total) {
        if (count==DIAG_THREADS) continue;
        memcpy(rows[count].name,t->name,sizeof(rows[count].name));
        rows[count].name[sizeof(rows[count].name)-1]=0;
        rows[count].point=__atomic_load_n(&t->wait_point,__ATOMIC_ACQUIRE);
        rows[count].object=__atomic_load_n(&t->wait_object,__ATOMIC_RELAXED);
        rows[count].since=__atomic_load_n(&t->wait_since_ms,__ATOMIC_RELAXED);
        rows[count].finished=t->finished; rows[count].host=t->host_owned;
        ++count;
    }
    pthread_mutex_unlock(&lock);
    const uint64_t now=monotonic_ms();
    printf("BootDiag: %zu threads (%zu listed); a wait is shown only where the runtime records it\n",total,count);
    for (size_t i=0;i<count;++i) {
        if (rows[i].finished) { printf("BootDiag:   '%s' finished\n",rows[i].name); continue; }
        if (!rows[i].point) { printf("BootDiag:   '%s'%s running or in an untracked call\n",rows[i].name,rows[i].host ? " (host)" : ""); continue; }
        printf("BootDiag:   '%s'%s in %s (0x%llx) for %llu ms\n",rows[i].name,rows[i].host ? " (host)" : "",rows[i].point,
               (unsigned long long)rows[i].object,(unsigned long long)(now>=rows[i].since ? now-rows[i].since : 0));
    }
    pthread_mutex_unlock(&rows_lock);
}
