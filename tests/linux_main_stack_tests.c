/* Stack bounds of the guest main thread (issue #5) on the production thread, memory and guest entry layers. The IL2CPP
 * collector reads its stack base through scePthreadAttrGet, scePthreadAttrGetstackaddr and scePthreadAttrGetstacksize
 * and scans from the stack pointer up to it; on POSIX the guest main thread runs on its own runtime_low_map stack. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define PAGE ((uintptr_t)4096)
#define MAIN_STACK ((size_t)8 * 1024 * 1024)
#define ENTRY_RESULT ((void *)0x5157)

#ifdef _WIN32
int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
FILE *sos_stderr_stream(void) { return stdout; }
#else
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset) {
    (void)fd; (void)buffer; (void)size; (void)offset;
    return -1;
}
#endif
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) {
    (void)caller;
    printf("  [fail] the runtime ended the process (%s, status %d)\n", api, status);
    fflush(stdout);
    _Exit(status ? status : 1);
}
void sos_thread_label_set(const char *name) { (void)name; }
void runtime_thread_keys_cleanup(void) {}
void runtime_signal_deliver_pending(void) {}
int runtime_signal_deliverable(void) { return 0; }
void runtime_module_tls_release(void) {}
uintptr_t runtime_mutex_resolve(const char *name) { (void)name; return 0; }
uintptr_t runtime_rwlock_resolve(const char *name) { (void)name; return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef void *(ABI *Entry)(void *);
typedef void *(ABI *SelfFn)(void);
typedef int32_t (ABI *AttrFn)(void **);
typedef int32_t (ABI *AttrGetFn)(void *, void **);
typedef int32_t (ABI *AttrAddrFn)(void **, void **);
typedef int32_t (ABI *AttrSizeFn)(void **, uint64_t *);
typedef int32_t (ABI *CreateFn)(void **, void **, Entry, void *, const char *);
typedef int32_t (ABI *JoinFn)(void *, void **);

static SelfFn self_fn;
static AttrFn attr_init_fn, attr_destroy_fn;
static AttrGetFn attr_get_fn;
static AttrAddrFn attr_addr_fn;
static AttrSizeFn attr_size_fn;
static CreateFn create_fn;
static JoinFn join_fn;
static int failures, checks;

static void check(const char *name, int ok) {
    ++checks;
    printf("  [%s] %s\n", ok ? "ok" : "fail", name);
    if (!ok) ++failures;
    fflush(stdout);
}

typedef struct { uintptr_t low, high; } Bounds;

/* The calls the collector makes for the thread it runs on. */
static Bounds guest_bounds(void) {
    void *attr = NULL;
    void *address = NULL;
    uint64_t size = 0;
    if (attr_init_fn(&attr) || attr_get_fn(self_fn(), &attr) || attr_addr_fn(&attr, &address) || attr_size_fn(&attr, &size))
        return (Bounds){0, 0};
    attr_destroy_fn(&attr);
    return (Bounds){(uintptr_t)address, (uintptr_t)address + size};
}
static __attribute__((noinline)) uintptr_t current_rsp(void) {
    uintptr_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    return sp;
}
static int contains(Bounds b, uintptr_t address) { return b.low <= address && address < b.high; }
static int same(Bounds a, Bounds b) { return a.low == b.low && a.high == b.high; }

static Bounds native_bounds(void) {
#ifdef _WIN32
    const uintptr_t base = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery((LPCVOID)(base - 1), &region, sizeof(region))) return (Bounds){0, 0};
    return (Bounds){(uintptr_t)region.AllocationBase, base};
#else
    pthread_attr_t attr;
    void *address = NULL;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr)) return (Bounds){0, 0};
    pthread_attr_getstack(&attr, &address, &size);
    pthread_attr_destroy(&attr);
    return (Bounds){(uintptr_t)address, (uintptr_t)address + size};
#endif
}

typedef struct {
    uintptr_t rsp;
    Bounds seen, native;
} Observed;

static ABI void *worker_entry(void *argument) {
    Observed *o = argument;
    o->rsp = current_rsp();
    o->seen = guest_bounds();
    o->native = native_bounds();
    return NULL;
}
static int run_worker(Observed *o) {
    void *handle = NULL;
    memset(o, 0, sizeof(*o));
    return create_fn(&handle, NULL, worker_entry, o, "worker") == 0 && join_fn(handle, NULL) == 0;
}

static int resolve(void) {
    runtime_thread_attach_main();
    self_fn = (SelfFn)runtime_thread_resolve("aI+OeCz8xrQ#libkernel");
    attr_init_fn = (AttrFn)runtime_thread_resolve("nsYoNRywwNg#libkernel");
    attr_destroy_fn = (AttrFn)runtime_thread_resolve("62KCwEMmzcM#libkernel");
    attr_get_fn = (AttrGetFn)runtime_thread_resolve("x1X76arYMxU#libkernel");
    attr_addr_fn = (AttrAddrFn)runtime_extra_resolve("scePthreadAttrGetstackaddr");
    attr_size_fn = (AttrSizeFn)runtime_thread_resolve("-fA+7ZlGDQs#libkernel");
    create_fn = (CreateFn)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    join_fn = (JoinFn)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    return self_fn && attr_init_fn && attr_destroy_fn && attr_get_fn && attr_addr_fn && attr_size_fn && create_fn && join_fn;
}

static void worker_checks(const char *where, Bounds main_bounds) {
    Observed worker;
    char name[160];
    const int ran = run_worker(&worker);
    snprintf(name, sizeof(name), "a guest thread created %s reports its own stack, which holds its rsp", where);
    check(name, ran && contains(worker.seen, worker.rsp) && same(worker.seen, worker.native));
    snprintf(name, sizeof(name), "a guest thread created %s does not report the main thread's stack", where);
    check(name, ran && !same(worker.seen, main_bounds) && !contains(main_bounds, worker.rsp));
}

#ifdef _WIN32
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    check("the guest pthread exports resolve", resolve());
    const Bounds host = guest_bounds();
    const Bounds teb = native_bounds();
    check("Windows: the main thread reports its TEB stack (AllocationBase to StackBase)", host.high && same(host, teb));
    check("Windows: that range holds the main thread's rsp", contains(host, current_rsp()));
    worker_checks("on Windows", host);
    printf("linux-main-stack Windows tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
#else
static int mapped(uintptr_t address) {
    return msync((void *)(address & ~(PAGE - 1)), PAGE, MS_ASYNC) == 0 || errno != ENOMEM;
}
/* The collector's scan of [sp, high), stopped at the first unmapped page instead of faulting there.
 * Returns that page, or 0 when every word of the range was read. */
static uintptr_t scan(uintptr_t sp, uintptr_t high) {
    if (sp >= high) return sp;
    for (uintptr_t page = sp & ~(PAGE - 1); page < high; page += PAGE)
        if (!mapped(page)) return page < sp ? sp : page;
    volatile uint64_t sum = 0;
    for (uintptr_t word = sp; word + sizeof(uint64_t) <= high; word += sizeof(uint64_t)) sum += *(volatile uint64_t *)word;
    (void)sum;
    return 0;
}

typedef struct {
    uintptr_t rsp, scan_stop;
    Bounds seen;
    Observed worker;
    int worker_ran;
} GuestMain;

static ABI void *guest_main(void *arg0, void *arg1) {
    GuestMain *g = arg0;
    g->rsp = current_rsp();
    g->seen = guest_bounds();
    g->scan_stop = scan(g->rsp, g->seen.high);
    g->worker_ran = run_worker(&g->worker);
    return arg1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    check("the guest pthread exports resolve", resolve());
    const Bounds host = guest_bounds();
    check("before the guest entry the main thread reports its host pthread stack", host.high && same(host, native_bounds()));
    check("that host stack holds the host rsp", contains(host, current_rsp()));

    if (!runtime_low_map(PAGE, PROT_READ | PROT_WRITE)) { check("a low mapping precedes the guest stack", 0); return 1; }
    unsigned char *stack = runtime_low_map(MAIN_STACK, PROT_READ | PROT_WRITE);
    const Bounds guest = {(uintptr_t)stack, (uintptr_t)stack + MAIN_STACK};
    check("the guest main stack is a page-aligned low mapping below 1 TiB",
          stack && !((uintptr_t)stack & (PAGE - 1)) && guest.high <= ((uintptr_t)1 << 40));
    if (!stack) return 1;
    check("the guest stack is mapped readable and writable from its first to its last page",
          mapped(guest.low) && mapped(guest.high - PAGE) && scan(guest.low, guest.high) == 0);
    check("the page above the guest stack (its exclusive top) is unmapped", !mapped(guest.high));
    check("the page below the guest stack is unmapped (no guard page inside the mapping)", !mapped(guest.low - PAGE));
    check("allocating the guest stack does not change the main thread's bounds", same(guest_bounds(), host));

    const uintptr_t entry_sp = guest.high - 64;
    const uintptr_t old_stop = scan(entry_sp & ~(uintptr_t)15, host.high);
    printf("  host bounds 0x%lx-0x%lx, guest stack 0x%lx-0x%lx\n", (unsigned long)host.low, (unsigned long)host.high,
           (unsigned long)guest.low, (unsigned long)guest.high);
    check("issue #5 reproduced: a scan from the guest rsp to the host stack base stops at the guest stack top",
          !contains(host, entry_sp) && old_stop == guest.high);

    GuestMain g;
    memset(&g, 0, sizeof(g));
    void *result = runtime_thread_enter_on_stack((void *)guest_main, &g, ENTRY_RESULT, stack, MAIN_STACK);
    printf("  guest rsp 0x%lx, reported 0x%lx-0x%lx\n", (unsigned long)g.rsp, (unsigned long)g.seen.low, (unsigned long)g.seen.high);
    check("the guest entry runs and its result is returned", result == ENTRY_RESULT);
    check("the guest entry runs on the guest stack, not the host stack", contains(guest, g.rsp) && !contains(host, g.rsp));
    check("on the guest stack the collector's low bound is the first byte of the mapping", g.seen.low == guest.low);
    check("on the guest stack the collector's high bound is the exclusive top of the mapping", g.seen.high == guest.high);
    check("the reported size is the whole guest stack (8 MiB)", g.seen.high - g.seen.low == MAIN_STACK);
    check("low <= rsp < high on the guest stack", contains(g.seen, g.rsp));
    check("the host stack is not reported on the guest stack", !same(g.seen, host));
    check("the collector's scan from rsp to the reported high reads every word and never leaves the mapping",
          g.scan_stop == 0 && g.seen.high <= guest.high);
    check("the main thread reports its host stack again after the guest entry returns", same(guest_bounds(), host));

    check("a guest thread created on the guest main stack reports its own stack, which holds its rsp",
          g.worker_ran && contains(g.worker.seen, g.worker.rsp) && same(g.worker.seen, g.worker.native));
    check("a guest thread created on the guest main stack does not report the guest main stack",
          g.worker_ran && !same(g.worker.seen, guest) && !contains(guest, g.worker.rsp));
    worker_checks("from the host main stack", host);
    printf("linux-main-stack POSIX tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
#endif
