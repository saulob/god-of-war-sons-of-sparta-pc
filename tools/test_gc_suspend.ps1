# Headless test of the IL2CPP GC stop-the-world handshake (PS4Util/Boehm): SIGUSR1 raised to guest threads, the handler
# acknowledges through a kernel semaphore and waits on an event flag until resumed. Real thread, signal, semaphore and
# event-flag layers; no game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_gc_suspend_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
@'
#include "runtime.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) { (void)api; (void)caller; _Exit(status); }
FILE *sos_stderr_stream(void) { return stdout; }
void sos_thread_label_set(const char *name) { (void)name; }
void runtime_thread_keys_cleanup(void) {}
void runtime_module_tls_release(void) {}
void compat_sleep_ns(uint64_t ns) { Sleep((DWORD)(ns / 1000000)); }
void compat_sleep_release(void) {}
int sos_setjmp(sos_jmp_buf buffer) { (void)buffer; return 0; }
void sos_longjmp(sos_jmp_buf buffer, int value) { (void)buffer; _Exit(value); }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
uintptr_t runtime_eventflag_resolve(const char *name);

/* Stand-in guest code: functions placed between these markers count as guest code for signal delivery. */
__attribute__((section(".gtext$a"), used)) static void guest_code_begin(void) {}
__attribute__((section(".gtext$c"), used)) static void guest_code_end(void) {}
int runtime_is_guest_code(uintptr_t address) {
    return address >= (uintptr_t)guest_code_begin && address < (uintptr_t)guest_code_end;
}

typedef void *(ABI *Entry)(void *);
typedef void *(ABI *SelfFn)(void);
typedef int32_t (ABI *CreateFn)(void **, void **, Entry, void *, const char *);
typedef int32_t (ABI *JoinFn)(void *, void **);
typedef int32_t (ABI *InstallFn)(int32_t, void (ABI *)(int32_t, void *));
typedef int32_t (ABI *RaiseFn)(void *, int32_t);
typedef int32_t (ABI *SemCreateFn)(uint32_t *, const char *, uint32_t, int32_t, int32_t, const void *);
typedef int32_t (ABI *SemWaitFn)(uint32_t, int32_t, uint32_t *);
typedef int32_t (ABI *SemSignalFn)(uint32_t, int32_t);
typedef int32_t (ABI *EfCreateFn)(void **, const char *, uint32_t, uint64_t, const void *);
typedef int32_t (ABI *EfWaitFn)(void *, uint64_t, uint32_t, uint64_t *, uint32_t *);
typedef int32_t (ABI *EfSetFn)(void *, uint64_t);

typedef int32_t (ABI *AttrFn)(void **);
typedef int32_t (ABI *AttrGetFn)(void *, void **);
typedef int32_t (ABI *AttrStackFn)(void **, void **, uint64_t *);
typedef int32_t (ABI *AttrSizeFn)(void **, uint64_t *);
typedef int32_t (ABI *MutexInitFn)(void **, void **, const char *);
typedef int32_t (ABI *MutexFn)(void **);

enum { TARGETS = 5, ROUNDS = 100, SIGUSR1 = 30 };
static MutexFn mutex_lock_fn, mutex_unlock_fn;
static void *gc_lock;
static SelfFn self_fn;
static SemWaitFn sem_wait_fn;
static SemSignalFn sem_signal_fn;
static EfWaitFn ef_wait_fn;
static uint32_t ack, resumed, busy;
static void *gc_flag;
static void *handles[TARGETS];
static volatile LONG64 progress[TARGETS], captured_rsp[TARGETS], captured_rip[TARGETS], started[TARGETS], wrong_thread;
static volatile LONG stop;
static volatile LONG64 stack_top[TARGETS], stack_size[TARGETS], stack_ok = 1, old_missed[TARGETS], new_covered[TARGETS];
static AttrFn attr_init_fn;
static AttrGetFn attr_get_fn;
static AttrStackFn attr_stack_fn;
static AttrSizeFn attr_size_fn;
static int failures;

static void check(const char *name, int condition) {
    printf(condition ? "  [ok]   %s\n" : "  [fail] %s\n", name);
    if (!condition) ++failures;
}
static int index_of(void *self) {
    for (int i = 0; i < TARGETS; ++i) if (handles[i] == self) return i;
    return -1;
}
/* The IL2CPP suspend handler behind PS4Util's trampoline: find the own record, save the context, ack, wait, ack resume. */
static ABI void gc_suspend_handler(int32_t sig, void *context) {
    if (sig != SIGUSR1) return;
    const int i = index_of(self_fn());
    if (i < 0) InterlockedIncrement64(&wrong_thread);
    else {
        captured_rsp[i] = *(int64_t *)((char *)context + 0xf8);
        captured_rip[i] = *(int64_t *)((char *)context + 0xe0);
    }
    sem_signal_fn(ack, 1);
    uint64_t bits = 0;
    ef_wait_fn(gc_flag, 1, 1, &bits, NULL);
    sem_signal_fn(resumed, 1);
}
__attribute__((section(".gtext$b"), noinline)) static void guest_compute(void) {
    volatile double x = 1;
    for (int k = 0; k < 20000; ++k) x *= 1.0000001;
}
/* 0 guest computation, 1 semaphore wait, 2 sleep, 3 guest code around a short runtime call, 4 the collector's lock. */
static ABI void *target(void *argument) {
    const int i = (int)(intptr_t)argument;
    {
        /* What the collector computes as the stack base: stack address + stack size of the thread's own attributes. */
        void *attr = NULL;
        void *address = NULL;
        uint64_t size = 0, high = 0;
        if (attr_init_fn(&attr) || attr_get_fn(self_fn(), &attr) || attr_stack_fn(&attr, &address, &size) ||
            attr_size_fn(&attr, &high))
            InterlockedExchange64(&stack_ok, 0);
        stack_top[i] = (LONG64)((uintptr_t)address + high);
        stack_size[i] = (LONG64)high;
        char probe;
        if ((uintptr_t)&probe < (uintptr_t)address || (uintptr_t)&probe >= (uintptr_t)address + high)
            InterlockedExchange64(&stack_ok, 0);
        /* The previous calculation: OS stack low + the creation size (1 MiB default). A live frame at `probe` is omitted
         * from a scan of [rsp, top) when probe >= top. */
        ULONG_PTR os_low = 0, os_high = 0;
        GetCurrentThreadStackLimits(&os_low, &os_high);
        old_missed[i] = (uintptr_t)&probe >= os_low + (1u << 20);
        new_covered[i] = (uintptr_t)&probe < (uintptr_t)address + high;
    }
    InterlockedExchange64(&started[i], 1);
    while (!stop) {
        if (i == 0) guest_compute();
        else if (i == 1) { uint32_t timeout = 20000; sem_wait_fn(busy, 1, &timeout); }
        else if (i == 2) runtime_thread_sleep_ns(1000000);
        else if (i == 3) { guest_compute(); self_fn(); }
        else { mutex_lock_fn(&gc_lock); guest_compute(); mutex_unlock_fn(&gc_lock); }
        InterlockedIncrement64(&progress[i]);
    }
    return NULL;
}
static int continues(int i) {
    const LONG64 since = progress[i];
    for (int waited = 0; waited < 3000; ++waited) {
        if (progress[i] != since) return 1;
        Sleep(1);
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime_thread_attach_main();
    self_fn = (SelfFn)runtime_thread_resolve("aI+OeCz8xrQ#libkernel");
    CreateFn create = (CreateFn)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    JoinFn join = (JoinFn)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    InstallFn install = (InstallFn)runtime_signal_resolve("sceKernelInstallExceptionHandler");
    RaiseFn raise_fn = (RaiseFn)runtime_signal_resolve("sceKernelRaiseException");
    SemCreateFn sem_create = (SemCreateFn)runtime_sema_resolve("188x57JYp0g#libkernel");
    sem_wait_fn = (SemWaitFn)runtime_sema_resolve("Zxa0VhQVTsk#libkernel");
    sem_signal_fn = (SemSignalFn)runtime_sema_resolve("4czppHBiriw#libkernel");
    EfCreateFn ef_create = (EfCreateFn)runtime_eventflag_resolve("sceKernelCreateEventFlag");
    ef_wait_fn = (EfWaitFn)runtime_eventflag_resolve("sceKernelWaitEventFlag");
    EfSetFn ef_set = (EfSetFn)runtime_eventflag_resolve("sceKernelSetEventFlag");
    EfSetFn ef_clear = (EfSetFn)runtime_eventflag_resolve("sceKernelClearEventFlag");
    MutexInitFn mutex_init = (MutexInitFn)runtime_mutex_resolve("cmo1RIYva9o#libkernel");
    mutex_lock_fn = (MutexFn)runtime_mutex_resolve("9UK1vLZQft4#libkernel");
    mutex_unlock_fn = (MutexFn)runtime_mutex_resolve("tn3VlD0hG60#libkernel");
    if (!self_fn || !create || !join || !install || !raise_fn || !sem_create || !sem_wait_fn || !sem_signal_fn ||
        !ef_create || !ef_wait_fn || !ef_set || !ef_clear || !mutex_init || !mutex_lock_fn || !mutex_unlock_fn) {
        puts("  [fail] exports are missing");
        return 1;
    }
    sem_create(&ack, "ack", 0, 0, 256, NULL);
    sem_create(&resumed, "resume", 0, 0, 256, NULL);
    sem_create(&busy, "busy", 0, 0, 256, NULL);
    ef_create(&gc_flag, "gc", 0x21, 0, NULL);
    mutex_init(&gc_lock, NULL, "gc lock");
    attr_init_fn = (AttrFn)runtime_thread_resolve("nsYoNRywwNg#libkernel");
    attr_get_fn = (AttrGetFn)runtime_thread_resolve("x1X76arYMxU#libkernel");
    attr_stack_fn = (AttrStackFn)runtime_thread_resolve("-quPa4SEJUw#libkernel");
    attr_size_fn = (AttrSizeFn)runtime_thread_resolve("-fA+7ZlGDQs#libkernel");
    check("SIGUSR1 handler installs", install(SIGUSR1, gc_suspend_handler) == 0);
    check("a second install is EAGAIN", install(SIGUSR1, gc_suspend_handler) == (int32_t)0x80020023);
    check("a dead handle is ESRCH", raise_fn((void *)0x1234, SIGUSR1) == (int32_t)0x80020003);

    for (int i = 0; i < TARGETS; ++i) {
        check("target thread is created", create(&handles[i], NULL, target, (void *)(intptr_t)i, "posix") == 0);
        uint32_t timeout = 5000000;
        check("a signal raised right after create is delivered",
              raise_fn(handles[i], SIGUSR1) == 0 && sem_wait_fn(ack, 1, &timeout) == 0);
        ef_set(gc_flag, 1);
        timeout = 5000000;
        check("the early-signalled thread resumes", sem_wait_fn(resumed, 1, &timeout) == 0);
        ef_clear(gc_flag, 0);
    }
    for (int i = 0; i < TARGETS; ++i) while (!started[i]) Sleep(1);

    int suspended = 1, frozen = 1, contexts = 1, resumed_all = 1, continued = 1, round = 0;
    for (; round < ROUNDS && suspended && resumed_all && continued; ++round) {
        for (int i = 0; i < TARGETS; ++i) captured_rsp[i] = captured_rip[i] = 0;
        mutex_lock_fn(&gc_lock);
        int raised = 0;
        for (int i = 0; i < TARGETS; ++i) raised += raise_fn(handles[i], SIGUSR1) == 0;
        uint32_t timeout = 5000000;
        suspended = raised == TARGETS && sem_wait_fn(ack, TARGETS, &timeout) == 0;
        if (!suspended) {
            for (int i = 0; i < TARGETS; ++i) if (!captured_rsp[i]) printf("  target %d did not acknowledge\n", i);
            runtime_signal_diag_dump();
            break;
        }
        LONG64 before[TARGETS];
        for (int i = 0; i < TARGETS; ++i) before[i] = progress[i];
        Sleep(1);
        for (int i = 0; i < TARGETS; ++i) {
            if (progress[i] != before[i]) frozen = 0;
            if (!captured_rsp[i] || !captured_rip[i]) contexts = 0;
        }
        ef_set(gc_flag, 1);
        timeout = 5000000;
        resumed_all = sem_wait_fn(resumed, TARGETS, &timeout) == 0;
        ef_clear(gc_flag, 0);
        mutex_unlock_fn(&gc_lock);
        for (int i = 0; i < TARGETS && continued; ++i) continued = continues(i);
    }
    printf("  rounds completed: %d of %d\n", round, ROUNDS);
    runtime_signal_diag_dump();
    check("every round acknowledges all targets", suspended);
    check("suspended targets make no progress", frozen);
    check("the handler runs on the raised thread", wrong_thread == 0);
    check("the handler sees rsp (+0xf8) and rip (+0xe0)", contexts);
    check("every round resumes all targets", resumed_all);
    int inside = 1;
    for (int i = 0; i < TARGETS; ++i)
        inside &= captured_rsp[i] == 0 || ((uint64_t)captured_rsp[i] < (uint64_t)stack_top[i] &&
                                           (uint64_t)captured_rsp[i] >= (uint64_t)stack_top[i] - (uint64_t)stack_size[i]);
    check("each thread's stack base from its attributes covers its stack and captured rsp", stack_ok && inside);
    int old_wrong = 1, new_right = 1;
    for (int i = 0; i < TARGETS; ++i) { old_wrong &= old_missed[i] != 0; new_right &= new_covered[i] != 0; }
    check("the previous base (OS low + creation size) omitted a live frame of every thread", old_wrong);
    check("the reported base covers that frame", new_right);
    check("every target continues after resume", continued);

    InterlockedExchange(&stop, 1);
    sem_signal_fn(busy, TARGETS);
    for (int i = 0; i < TARGETS; ++i) join(handles[i], NULL);
    return failures == 0 ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_thread.c') (Join-Path $root 'src\runtime_signal.c') `
    (Join-Path $root 'src\runtime_sema.c') (Join-Path $root 'src\runtime_eventflag.c') `
    (Join-Path $root 'src\runtime_mutex.c') '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the GC suspend test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
