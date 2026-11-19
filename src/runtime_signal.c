/* Guest signals and libkernel_unity exception handlers, after shadPS4's kernel exception.cpp and Pthread signal
 * delivery. A signal queued for a thread is delivered on that thread: Windows interrupts it with a special user APC. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
#define MAX_SIG 128
enum { SIG_HUP = 1, SIG_ILL = 4, SIG_FPE = 8, SIG_KILL = 9, SIG_BUS = 10, SIG_SEGV = 11, SIG_STOP = 17, SIG_USR1 = 30,
       SIG_THR = 32 };
enum { E_SRCH = 3, E_NOMEM = 12, E_INVAL = 22, E_AGAIN = 35 };
enum { GUEST_SIG_BLOCK = 1, GUEST_SIG_UNBLOCK = 2, GUEST_SIG_SETMASK = 3 };
enum { SA_ONSTACK = 0x1, SA_RESETHAND = 0x4, SA_SIGINFO = 0x40, SA_NODEFER = 0x10, SA_RESTART = 0x2 };
enum { SS_ONSTACK = 1, SS_DISABLE = 4, MIN_ALT_STACK = 2048 };
#define SIGHANDLER_DEFAULT 0
#define SIGHANDLER_IGNORE 1

typedef struct { uint32_t bits[4]; } Sigset;
typedef struct { uintptr_t handler; int32_t flags; Sigset mask; } Sigaction;
typedef struct { void *stack; uint64_t size; int32_t flags; int32_t pad; } AltStack;
typedef struct {
    int32_t signo, error, code, pid;
    uint32_t uid;
    int32_t status;
    void *address;
    uint64_t value;
    unsigned char reason[40];
} Siginfo;
typedef struct {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    int32_t trapno;
    uint16_t fs, gs;
    uint64_t addr;
    int32_t flags;
    uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss, len, fpformat, ownedfp, lbrfrom, lbrto, aux1, aux2;
    uint64_t fpstate[104];
    uint64_t fsbase, gsbase, spare[6];
} Mcontext;
typedef struct Ucontext {
    Sigset sigmask;
    int32_t reserved[12];
    Mcontext mcontext;
    struct Ucontext *link;
    struct { void *sp; uint64_t size; int32_t flags, pad; } stack;
    int32_t flags;
    int32_t spare[4];
    int32_t tail[3];
} Ucontext;
_Static_assert(__builtin_offsetof(Ucontext, mcontext) == 0x40, "FreeBSD ucontext layout");
_Static_assert(__builtin_offsetof(Ucontext, mcontext.rsp) == 0xf8, "guest handlers read rsp at +0xf8");

typedef void (ABI *ExceptionHandler)(int32_t, void *);
typedef void (ABI *SigactionHandler)(int32_t, Siginfo *, Ucontext *);
typedef void (ABI *SignalHandler)(int32_t);

static Sigaction actions[MAX_SIG];
static ExceptionHandler exception_callbacks[MAX_SIG];
static Sigset interrupt_set;
static unsigned apcs_queued, apc_failures, apcs_entered, apcs_in_host, dispatched, returned, undelivered;

static int valid_signal(int sig) { return sig >= 1 && sig <= MAX_SIG; }
static int in_set(const Sigset *set, int sig) { return (set->bits[(sig - 1) >> 5] >> ((sig - 1) & 31)) & 1; }
static int fail(int error) { *runtime_errno() = error; return -1; }

static int blocked(RuntimeThreadSignals *t, int sig) {
    return (__atomic_load_n(&t->mask[(sig - 1) >> 5], __ATOMIC_ACQUIRE) >> ((sig - 1) & 31)) & 1;
}
static void read_mask(RuntimeThreadSignals *t, Sigset *out) {
    for (int i = 0; i < 4; ++i) out->bits[i] = __atomic_load_n(&t->mask[i], __ATOMIC_ACQUIRE);
}
static void write_mask(RuntimeThreadSignals *t, const Sigset *mask) {
    for (int i = 0; i < 4; ++i) __atomic_store_n(&t->mask[i], mask->bits[i], __ATOMIC_RELEASE);
}
static int find_pending(RuntimeThreadSignals *t, const Sigset *only) {
    for (int sig = 1; sig <= MAX_SIG; ++sig) {
        if (only && !in_set(only, sig)) continue;
        if (__atomic_load_n(&t->pending[sig - 1], __ATOMIC_ACQUIRE)) return sig;
    }
    return 0;
}
static int find_deliverable(RuntimeThreadSignals *t) {
    const int waiting = __atomic_load_n(&t->in_sigwait, __ATOMIC_ACQUIRE);
    Sigset wait_set;
    for (int i = 0; i < 4; ++i) wait_set.bits[i] = t->wait_set[i];
    for (int sig = 1; sig <= MAX_SIG; ++sig) {
        if (blocked(t, sig) || (waiting && in_set(&wait_set, sig))) continue;
        if (__atomic_load_n(&t->pending[sig - 1], __ATOMIC_ACQUIRE)) return sig;
    }
    return 0;
}
static int consume(RuntimeThreadSignals *t, int sig) {
    uint32_t count = __atomic_load_n(&t->pending[sig - 1], __ATOMIC_ACQUIRE);
    while (count)
        if (__atomic_compare_exchange_n(&t->pending[sig - 1], &count, count - 1, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

static void import_context(Ucontext *uc, void *native) {
#ifdef _WIN32
    const CONTEXT *c = native;
    Mcontext *m = &uc->mcontext;
    m->r8 = c->R8; m->r9 = c->R9; m->r10 = c->R10; m->r11 = c->R11; m->r12 = c->R12; m->r13 = c->R13;
    m->r14 = c->R14; m->r15 = c->R15; m->rdi = c->Rdi; m->rsi = c->Rsi; m->rbp = c->Rbp; m->rbx = c->Rbx;
    m->rdx = c->Rdx; m->rax = c->Rax; m->rcx = c->Rcx; m->rsp = c->Rsp; m->rip = c->Rip; m->rflags = c->EFlags;
#else
    (void)uc; (void)native;
#endif
}
static void export_context(const Ucontext *uc, void *native) {
#ifdef _WIN32
    CONTEXT *c = native;
    const Mcontext *m = &uc->mcontext;
    c->R8 = m->r8; c->R9 = m->r9; c->R10 = m->r10; c->R11 = m->r11; c->R12 = m->r12; c->R13 = m->r13;
    c->R14 = m->r14; c->R15 = m->r15; c->Rdi = m->rdi; c->Rsi = m->rsi; c->Rbp = m->rbp; c->Rbx = m->rbx;
    c->Rdx = m->rdx; c->Rax = m->rax; c->Rcx = m->rcx; c->Rsp = m->rsp; c->Rip = m->rip;
#else
    (void)uc; (void)native;
#endif
}

static void default_action(int sig) {
    static unsigned char reported[MAX_SIG];
    if (sig == SIG_BUS || sig == SIG_SEGV || reported[sig - 1]) return;
    reported[sig - 1] = 1;
    fprintf(stderr, "Runtime: guest signal %d has no handler; ignored\n", sig);
}

static void dispatch(RuntimeThreadSignals *t, int sig, Siginfo *info, Ucontext *uc) {
    const Sigaction action = actions[sig - 1];
    const unsigned order = __atomic_add_fetch(&dispatched, 1, __ATOMIC_RELAXED);
    if (order <= 2) printf("DEBUG_SAULO: signal %d handler entered on '%s' (rsp 0x%llx)\n", sig,
                           runtime_thread_current_name() ? runtime_thread_current_name() : "?", (unsigned long long)uc->mcontext.rsp);
    if (action.handler == SIGHANDLER_IGNORE) return;
    if (action.handler == SIGHANDLER_DEFAULT) { default_action(sig); return; }
    Sigset old, now;
    read_mask(t, &old);
    now = old;
    for (int i = 0; i < 4; ++i) now.bits[i] |= action.mask.bits[i];
    if (!(action.flags & SA_NODEFER)) now.bits[(sig - 1) >> 5] |= 1u << ((sig - 1) & 31);
    write_mask(t, &now);
    if (action.flags & SA_RESETHAND) memset(&actions[sig - 1], 0, sizeof(actions[sig - 1]));
    if (action.flags & SA_SIGINFO) ((SigactionHandler)action.handler)(sig, info, uc);
    else ((SignalHandler)action.handler)(sig);
    write_mask(t, &old);
    if (__atomic_add_fetch(&returned, 1, __ATOMIC_RELAXED) <= 2)
        printf("DEBUG_SAULO: signal %d handler returned on '%s'\n", sig,
               runtime_thread_current_name() ? runtime_thread_current_name() : "?");
    if (__atomic_load_n(&t->in_sigsuspend, __ATOMIC_ACQUIRE)) __atomic_store_n(&t->suspend_interrupted, 1, __ATOMIC_RELEASE);
}

/* Runs every deliverable signal of the calling thread; `native` is the interrupted host context, if any. */
/* Runs every deliverable signal of the calling thread. `native` is its host context: interrupted (written back after
 * the handlers, which may change it) or captured at a delivery point (`resume` 0: only reported to the handlers). */
static void dispatch_pending(void *native, int resume) {
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    if (!t) return;
    Ucontext uc;
    memset(&uc, 0, sizeof(uc));
    if (native) import_context(&uc, native);
    read_mask(t, &uc.sigmask);
    Siginfo info;
    memset(&info, 0, sizeof(info));
    info.code = 0x10001; /* SI_LWP */
    info.address = (void *)(uintptr_t)uc.mcontext.rip;
    for (int sig; (sig = find_deliverable(t));) {
        if (!consume(t, sig)) continue;
        __atomic_sub_fetch(&undelivered, 1, __ATOMIC_RELAXED);
        info.signo = sig;
        dispatch(t, sig, &info, &uc);
    }
    if (native && resume) export_context(&uc, native);
}

int runtime_signal_deliverable(void) {
    if (!__atomic_load_n(&undelivered, __ATOMIC_RELAXED)) return 0;
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    return t && find_deliverable(t) != 0;
}
void runtime_signal_deliver_pending(void) {
    if (!runtime_signal_deliverable()) return;
#ifdef _WIN32
    CONTEXT context;
    memset(&context, 0, sizeof(context));
    RtlCaptureContext(&context);
    dispatch_pending(&context, 0);
#else
    dispatch_pending(NULL, 0);
#endif
}

#ifdef _WIN32
typedef LONG (NTAPI *QueueApcEx)(HANDLE, HANDLE, PVOID, PVOID, PVOID, PVOID);
/* A handler may take runtime locks, so it runs only where a kernel would deliver: the thread was interrupted in guest
 * code. Otherwise the thread is in a runtime call: blocking waits deliver after they wake (runtime_thread_wait_end), and
 * the pump interrupts the thread again until it is back in guest code. */
static void NTAPI apc_entry(PVOID a, PVOID b, PVOID c, PCONTEXT context) {
    (void)a; (void)b; (void)c;
    if (!context || !runtime_is_guest_code(context->Rip)) {
        __atomic_add_fetch(&apcs_in_host, 1, __ATOMIC_RELAXED);
        return;
    }
    if (__atomic_add_fetch(&apcs_entered, 1, __ATOMIC_RELAXED) <= 2)
        printf("DEBUG_SAULO: signal APC in guest code on '%s' (rip %p)\n",
               runtime_thread_current_name() ? runtime_thread_current_name() : "?", (void *)(uintptr_t)context->Rip);
    dispatch_pending(context, 1);
}
static LONG queue_apc(void *native) {
    static QueueApcEx queue;
    if (!queue) queue = (QueueApcEx)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueueApcThreadEx");
    const LONG status = queue ? queue(native, (HANDLE)(uintptr_t)1 /* special user APC */, (PVOID)apc_entry, NULL, NULL, NULL)
                              : (LONG)0xC0000002;
    __atomic_add_fetch(status ? &apc_failures : &apcs_queued, 1, __ATOMIC_RELAXED);
    return status;
}
#endif

static pthread_mutex_t pump_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pump_wake = PTHREAD_COND_INITIALIZER;
static int pump_started;

static int pump_visit(RuntimeThreadSignals *t, void *native) {
    if (!find_deliverable(t)) return 0;
#ifdef _WIN32
    if (native) queue_apc(native);
#else
    (void)native;
#endif
    return 1;
}
/* Interrupts threads with undelivered signals until each reaches a delivery point. */
static void *pump_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&pump_lock);
        while (!__atomic_load_n(&undelivered, __ATOMIC_RELAXED)) pthread_cond_wait(&pump_wake, &pump_lock);
        pthread_mutex_unlock(&pump_lock);
        const struct timespec period = {0, 1000000};
        nanosleep(&period, NULL);
        runtime_thread_signal_scan(pump_visit);
    }
    return NULL;
}
static void pump_notify(void) {
    pthread_mutex_lock(&pump_lock);
    if (!pump_started) {
        pthread_t thread;
        pump_started = !pthread_create(&thread, NULL, pump_main, NULL);
        if (pump_started) pthread_detach(thread);
    }
    pthread_cond_signal(&pump_wake);
    pthread_mutex_unlock(&pump_lock);
}

/* 0: delivered, queued, or the thread has not started (it delivers on start); nonzero: it cannot be interrupted. */
static int wake_thread(void *handle) {
    if (!handle || runtime_thread_signals(handle) == runtime_thread_signals(NULL)) {
        runtime_signal_deliver_pending();
        return 0;
    }
    runtime_thread_kick(handle);
#ifdef _WIN32
    HANDLE native = runtime_thread_native_handle(handle);
    if (!native) return 0;
    const LONG status = queue_apc(native);
    /* DEBUG_SAULO: first cross-thread signal requests of the GC suspend handshake. */
    static int traced;
    if (__atomic_fetch_add(&traced, 1, __ATOMIC_RELAXED) < 4)
        printf("DEBUG_SAULO: signal from '%s' to thread %p (%s), native %p, APC status 0x%08lx\n",
               runtime_thread_current_name() ? runtime_thread_current_name() : "?", handle,
               runtime_thread_name(handle) ? runtime_thread_name(handle) : "?", (void *)native, (unsigned long)status);
    return status ? E_SRCH : 0;
#else
    return 0;
#endif
}

static int queue_signal(void *thread, int sig) {
    RuntimeThreadSignals *t = runtime_thread_signals(thread);
    if (!t) return E_SRCH;
    if (!valid_signal(sig)) return E_INVAL;
    __atomic_add_fetch(&undelivered, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->pending[sig - 1], 1, __ATOMIC_RELEASE);
    if (blocked(t, sig)) return 0;
    if (wake_thread(thread)) {
        if (consume(t, sig)) __atomic_sub_fetch(&undelivered, 1, __ATOMIC_RELAXED);
        return E_SRCH;
    }
    pump_notify();
    return 0;
}

static ABI int32_t sig_emptyset(Sigset *set) { if (!set) return fail(E_INVAL); memset(set, 0, sizeof(*set)); return 0; }
static ABI int32_t sig_fillset(Sigset *set) { if (!set) return fail(E_INVAL); memset(set, 0xff, sizeof(*set)); return 0; }
static ABI int32_t sig_addset(Sigset *set, int sig) {
    if (!set || !valid_signal(sig)) return fail(E_INVAL);
    set->bits[(sig - 1) >> 5] |= 1u << ((sig - 1) & 31);
    return 0;
}
static ABI int32_t sig_delset(Sigset *set, int sig) {
    if (!set || !valid_signal(sig)) return fail(E_INVAL);
    set->bits[(sig - 1) >> 5] &= ~(1u << ((sig - 1) & 31));
    return 0;
}
static ABI int32_t sig_ismember(const Sigset *set, int sig) {
    if (!set || !valid_signal(sig)) return fail(E_INVAL);
    return in_set(set, sig);
}

static int32_t change_mask(int how, const Sigset *set, Sigset *old) {
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    if (!t) return E_INVAL;
    Sigset current;
    read_mask(t, &current);
    if (old) *old = current;
    if (!set) return 0;
    Sigset next = current;
    for (int i = 0; i < 4; ++i) {
        if (how == GUEST_SIG_BLOCK) next.bits[i] |= set->bits[i];
        else if (how == GUEST_SIG_UNBLOCK) next.bits[i] &= ~set->bits[i];
        else if (how == GUEST_SIG_SETMASK) next.bits[i] = set->bits[i];
        else return E_INVAL;
    }
    write_mask(t, &next);
    runtime_signal_deliver_pending();
    return 0;
}
static ABI int32_t sig_pthread_sigmask(int how, const Sigset *set, Sigset *old) { return change_mask(how, set, old); }
static ABI int32_t sig_procmask(int how, const Sigset *set, Sigset *old) {
    const int32_t error = change_mask(how, set, old);
    return error ? fail(error) : 0;
}

static ABI int32_t sig_action(int sig, const Sigaction *act, Sigaction *old) {
    if (!valid_signal(sig) || sig == SIG_THR || sig == SIG_KILL || sig == SIG_STOP) return fail(E_INVAL);
    if (old) *old = actions[sig - 1];
    if (act) actions[sig - 1] = *act;
    return 0;
}
static ABI uintptr_t sig_signal(int sig, uintptr_t handler) {
    if (!valid_signal(sig)) { fail(E_INVAL); return (uintptr_t)-1; }
    Sigaction act = {handler, in_set(&interrupt_set, sig) == 1 ? 0 : SA_RESTART, {{0}}}, old;
    if (sig_action(sig, &act, &old)) return (uintptr_t)-1;
    return old.handler;
}
static ABI int32_t sig_altstack(const AltStack *stack, AltStack *old) {
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    if (!t) return fail(E_INVAL);
    if (old) *old = (AltStack){t->alt_stack, t->alt_size, (int32_t)t->alt_flags, 0};
    if (!stack) return 0;
    if (stack->flags & ~(SS_ONSTACK | SS_DISABLE)) return fail(E_INVAL);
    if ((stack->flags & SS_ONSTACK) && !(t->alt_flags & SS_ONSTACK)) return fail(E_INVAL);
    if (!(stack->flags & SS_DISABLE) && stack->size < MIN_ALT_STACK) return fail(E_NOMEM);
    t->alt_stack = stack->stack;
    t->alt_size = stack->size;
    t->alt_flags = (uint32_t)(stack->flags & SS_DISABLE ? stack->flags : stack->flags & ~SS_ONSTACK);
    return 0;
}
static ABI int32_t sig_pending(Sigset *set) {
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    if (!set || !t) return fail(E_INVAL);
    memset(set, 0, sizeof(*set));
    for (int sig = 1; sig <= MAX_SIG; ++sig)
        if (__atomic_load_n(&t->pending[sig - 1], __ATOMIC_ACQUIRE)) set->bits[(sig - 1) >> 5] |= 1u << ((sig - 1) & 31);
    return 0;
}
static ABI int32_t sig_suspend(const Sigset *mask) {
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    if (!mask || !t) return fail(E_INVAL);
    Sigset old;
    read_mask(t, &old);
    __atomic_store_n(&t->suspend_interrupted, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&t->in_sigsuspend, 1, __ATOMIC_RELEASE);
    write_mask(t, mask);
    runtime_signal_deliver_pending();
    while (!__atomic_load_n(&t->suspend_interrupted, __ATOMIC_ACQUIRE)) runtime_thread_sleep_ns(1000000);
    __atomic_store_n(&t->in_sigsuspend, 0, __ATOMIC_RELEASE);
    write_mask(t, &old);
    return fail(4); /* EINTR */
}
static ABI int32_t sig_wait(const Sigset *set, int32_t *sig) {
    RuntimeThreadSignals *t = runtime_thread_signals(NULL);
    if (!set || !sig || !t) return E_INVAL;
    for (int i = 0; i < 4; ++i) t->wait_set[i] = set->bits[i];
    __atomic_store_n(&t->in_sigwait, 1, __ATOMIC_RELEASE);
    for (;;) {
        const int pending = find_pending(t, set);
        if (pending && consume(t, pending)) {
            *sig = pending;
            break;
        }
        runtime_thread_sleep_ns(1000000);
    }
    __atomic_store_n(&t->in_sigwait, 0, __ATOMIC_RELEASE);
    memset(t->wait_set, 0, sizeof(t->wait_set));
    return 0;
}
static ABI int32_t sig_thread_kill(void *thread, int sig) {
    if (!valid_signal(sig)) return E_INVAL;
    return queue_signal(thread, sig);
}
static ABI int32_t sig_raise(int sig) {
    const int32_t error = valid_signal(sig) ? queue_signal(NULL, sig) : E_INVAL;
    return error ? fail(error) : 0;
}

static ABI void exception_trampoline(int32_t sig, Siginfo *info, Ucontext *context) {
    (void)info;
    ExceptionHandler callback = exception_callbacks[sig - 1];
    if (callback) callback(sig, context);
}
static int exception_signal_allowed(int32_t sig) {
    return sig == SIG_HUP || sig == SIG_ILL || sig == SIG_FPE || sig == SIG_BUS || sig == SIG_SEGV || sig == SIG_USR1;
}
static ABI int32_t install_exception_handler(int32_t sig, ExceptionHandler handler) {
    if (!exception_signal_allowed(sig)) return ERR(E_INVAL);
    if (exception_callbacks[sig - 1]) return ERR(E_AGAIN);
    exception_callbacks[sig - 1] = handler;
    const Sigaction act = {(uintptr_t)exception_trampoline, SA_SIGINFO | SA_RESTART, {{0}}};
    actions[sig - 1] = act;
    printf("DEBUG_SAULO: exception handler for signal %d installed by '%s' (%p)\n", sig,
           runtime_thread_current_name() ? runtime_thread_current_name() : "?", (void *)handler);
    return 0;
}
static ABI int32_t remove_exception_handler(int32_t sig) {
    if (!exception_signal_allowed(sig)) return ERR(E_INVAL);
    exception_callbacks[sig - 1] = NULL;
    memset(&actions[sig - 1], 0, sizeof(actions[sig - 1]));
    return 0;
}
static ABI int32_t raise_exception(void *thread, int32_t sig) {
    if (sig != SIG_USR1) return ERR(E_INVAL);
    const int32_t error = queue_signal(thread, sig);
    return error ? ERR(error) : 0;
}

static const RuntimeExport exports[] = {
    {"sigaction", sig_action}, {"signal", sig_signal}, {"sigemptyset", sig_emptyset}, {"sigfillset", sig_fillset},
    {"sigaddset", sig_addset}, {"sigdelset", sig_delset}, {"sigismember", sig_ismember},
    {"sigprocmask", sig_procmask}, {"_sigprocmask", sig_procmask}, {"pthread_sigmask", sig_pthread_sigmask},
    {"sigaltstack", sig_altstack}, {"sigpending", sig_pending}, {"sigsuspend", sig_suspend}, {"sigwait", sig_wait},
    {"pthread_kill", sig_thread_kill}, {"raise", sig_raise},
    {"sceKernelInstallExceptionHandler", install_exception_handler},
    {"sceKernelRemoveExceptionHandler", remove_exception_handler},
    {"sceKernelRaiseException", raise_exception},
};
/* DEBUG_SAULO: stall diagnostics. */
void runtime_signal_diag_dump(void) {
    printf("BootDiag: signals: undelivered %u; APCs queued %u, failed %u, in guest code %u, in runtime code %u; "
           "handlers entered %u, returned %u\n",
           __atomic_load_n(&undelivered, __ATOMIC_RELAXED), __atomic_load_n(&apcs_queued, __ATOMIC_RELAXED),
           __atomic_load_n(&apc_failures, __ATOMIC_RELAXED), __atomic_load_n(&apcs_entered, __ATOMIC_RELAXED),
           __atomic_load_n(&apcs_in_host, __ATOMIC_RELAXED), __atomic_load_n(&dispatched, __ATOMIC_RELAXED),
           __atomic_load_n(&returned, __ATOMIC_RELAXED));
}
uintptr_t runtime_signal_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
