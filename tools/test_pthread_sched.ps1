# Headless pthread scheduling tests. The runtime thread layer is compiled with small stubs for logging, keys and labels;
# no game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_pthread_sched_' + [guid]::NewGuid().ToString('N'))
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
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) {
    (void)api; (void)caller; _Exit(status);
}
FILE *sos_stderr_stream(void) { return stdout; }
void sos_thread_label_set(const char *name) { (void)name; }
void runtime_thread_keys_cleanup(void) {}
void runtime_signal_deliver_pending(void) {}
int runtime_signal_deliverable(void) { return 0; }
void runtime_module_tls_release(void) {}
void compat_sleep_ns(uint64_t ns) { Sleep((DWORD)(ns / 1000000)); }
void compat_sleep_release(void) {}
int sos_setjmp(sos_jmp_buf buffer) { (void)buffer; return 0; }
void sos_longjmp(sos_jmp_buf buffer, int value) { (void)buffer; _Exit(value); }

typedef void *(ABI *SelfFn)(void);
typedef int32_t (ABI *CreateFn)(void **, void **, void *(ABI *)(void *), void *, const char *);
typedef int32_t (ABI *JoinFn)(void *, void **);
typedef int32_t (ABI *SchedGetFn)(void *, int *, int *);
typedef int32_t (ABI *SchedSetFn)(void *, int, const int *);
typedef int32_t (ABI *PrioGetFn)(void *, int *);
typedef int32_t (ABI *PrioSetFn)(void *, int);

static volatile LONG worker_started, worker_stop;
static int failures;

static ABI void *worker(void *argument) {
    (void)argument;
    InterlockedExchange(&worker_started, 1);
    while (InterlockedCompareExchange(&worker_stop, 1, 1) == 0) runtime_thread_sleep_ns(1000000);
    return NULL;
}
static void check(const char *name, int condition) {
    if (condition) {
        printf("  [ok]   %s\n", name);
    } else {
        printf("  [fail] %s\n", name);
        ++failures;
    }
}
static void *resolve(const char *nid) { return (void *)runtime_thread_resolve(nid); }

int main(void) {
    runtime_thread_attach_main();
    SelfFn self_fn = (SelfFn)resolve("aI+OeCz8xrQ#libkernel");
    SchedGetFn sce_get = (SchedGetFn)resolve("P41kTWUS3EI#libkernel");
    SchedSetFn sce_set = (SchedSetFn)resolve("oIRFTjoILbg#libkernel");
    SchedGetFn posix_get = (SchedGetFn)resolve("FIs3-UQT9sg#libkernel");
    SchedSetFn posix_set = (SchedSetFn)resolve("Xs9hdiD7sAA#libkernel");
    PrioGetFn sce_getprio = (PrioGetFn)resolve("1tKyG7RlMJo#libkernel");
    PrioSetFn sce_setprio = (PrioSetFn)resolve("W0Hpm2X0uPE#libkernel");
    CreateFn create = (CreateFn)resolve("6UgtwV+0zb4#libkernel");
    JoinFn join = (JoinFn)resolve("onNY9Byn-W8#libkernel");
    if (!self_fn || !sce_get || !sce_set || !posix_get || !posix_set || !sce_getprio || !sce_setprio || !create || !join) {
        puts("  [fail] scheduling exports are missing");
        return 1;
    }

    void *self = self_fn();
    int policy = 0, prio = 0;
    check("current thread Getschedparam succeeds", sce_get(self, &policy, &prio) == 0);
    check("current thread policy is FIFO", policy == 1);
    check("current thread priority is 700", prio == 700);

    void *thread = NULL;
    const int created = create(&thread, NULL, worker, NULL, "sched-test");
    check("another guest thread is created", created == 0 && thread != NULL);
    for (int waited = 0; !worker_started && waited < 2000; ++waited) Sleep(1);
    check("the guest thread started", worker_started != 0);

    policy = prio = 0;
    check("another live thread Getschedparam succeeds", sce_get(thread, &policy, &prio) == 0);
    check("another live thread keeps its policy", policy == 1);
    check("another live thread keeps its priority", prio == 700);

    int value = 900;
    check("SCE Setschedparam accepts OTHER/900", sce_set(thread, 2, &value) == 0);
    policy = prio = 0;
    check("Getschedparam reports OTHER/900", sce_get(thread, &policy, &prio) == 0 && policy == 2 && prio == 900);
    value = 0;
    check("Getprio agrees with Getschedparam", sce_getprio(thread, &value) == 0 && value == 900);
    check("Setprio updates the guest priority", sce_setprio(thread, 260) == 0);
    policy = prio = 0;
    check("Getschedparam reports the new priority",
          sce_get(thread, &policy, &prio) == 0 && policy == 2 && prio == 260);
    check("Getprio follows Setprio", sce_getprio(thread, &value) == 0 && value == 260);

    value = 500;
    check("POSIX Setschedparam accepts RR/500", posix_set(thread, 3, &value) == 0);
    policy = prio = 0;
    check("POSIX Getschedparam reports RR/500", posix_get(thread, &policy, &prio) == 0 && policy == 3 && prio == 500);

    check("SCE Getschedparam rejects a null policy", sce_get(thread, NULL, &prio) == (int32_t)0x80020016);
    check("SCE Getschedparam rejects a null param", sce_get(thread, &policy, NULL) == (int32_t)0x80020016);
    check("SCE Getschedparam rejects an invalid handle",
          sce_get((void *)0x1234, &policy, &prio) == (int32_t)0x80020003);
    check("SCE Setschedparam rejects a null param", sce_set(thread, 1, NULL) == (int32_t)0x80020016);
    check("SCE Setschedparam rejects an invalid handle",
          sce_set((void *)0x1234, 1, &value) == (int32_t)0x80020003);
    check("SCE Getprio rejects a null output", sce_getprio(thread, NULL) == (int32_t)0x80020016);
    check("SCE Getprio rejects an invalid handle", sce_getprio((void *)0x1234, &value) == (int32_t)0x80020003);
    check("POSIX Getschedparam rejects a null policy", posix_get(thread, NULL, &prio) == 22);
    check("POSIX Getschedparam rejects an invalid handle", posix_get((void *)0x1234, &policy, &prio) == 3);
    check("POSIX Setschedparam rejects a null param", posix_set(thread, 1, NULL) == 22);
    check("POSIX Setschedparam rejects an invalid handle", posix_set((void *)0x1234, 1, &value) == 3);

    InterlockedExchange(&worker_stop, 1);
    check("the guest thread joins", join(thread, NULL) == 0);
    return failures == 0 ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_thread.c') '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the pthread scheduling test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
