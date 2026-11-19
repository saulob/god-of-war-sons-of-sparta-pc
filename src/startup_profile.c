#include "startup_profile.h"

#include "runtime.h"
#include "startup_phase.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

static int window, active;
static LARGE_INTEGER window_start;
static struct { volatile LONG64 calls, ticks, amount; } costs[SOS_COST_COUNT];

int sos_startup_profile_active(void) { return __atomic_load_n(&window, __ATOMIC_RELAXED); }

int64_t sos_cost_begin(void) {
    if (!__atomic_load_n(&active, __ATOMIC_RELAXED)) return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void sos_cost_end(SosCost kind, int64_t start, uint64_t amount) {
    if (!start) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedIncrement64(&costs[kind].calls);
    InterlockedAdd64(&costs[kind].ticks, now.QuadPart - start);
    InterlockedAdd64(&costs[kind].amount, (LONG64)amount);
}

/* Samples of the guest main thread. A guest sample is keyed by its address (256-byte buckets), a host one by its host
 * module and the nearest guest return address on the stack, which names the guest code that is waiting or calling. */
enum { KIND_GUEST = 1, KIND_HOST = 2, KIND_UNKNOWN = 3 };
enum { MAX_HOST_MODULES = 128, SLOTS = 8192 };
typedef struct { uint64_t key; uint32_t count; } Slot;

static HANDLE main_thread, sampler_thread;
static uintptr_t image_lo, image_hi;
static struct { uintptr_t lo, hi; char name[40]; } host_modules[MAX_HOST_MODULES];
static unsigned host_module_count;
static Slot slots[SLOTS];
static uint32_t samples, lost_samples, kind_samples[4];
static uint32_t host_module_samples[MAX_HOST_MODULES + 1];
static FILETIME main_cpu_start[2], process_cpu_start[2];

static uint64_t make_key(unsigned kind, unsigned module, uintptr_t address) {
    return (uint64_t)kind << 56 | (uint64_t)module << 48 | ((uint64_t)address & 0xffffffffff00ull);
}
static void count_key(uint64_t key) {
    for (uint64_t h = (key * 0x9e3779b97f4a7c15ull) >> 51, probe = 0; probe < SLOTS; ++probe, h = (h + 1) & (SLOTS - 1)) {
        if (slots[h].key == key) { ++slots[h].count; return; }
        if (!slots[h].key) { slots[h] = (Slot){key, 1}; return; }
    }
    ++lost_samples;
}
static int guest_code(uintptr_t address) {
    return (address >= image_lo && address < image_hi) || runtime_redzone_contains(address) ||
           runtime_module_in_loaded_code(address);
}
static uintptr_t guest_function(uintptr_t rip) {
    uint64_t site, begin;
    if (runtime_redzone_site(rip, &site)) rip = image_lo + (uintptr_t)site;
    return rip >= image_lo && rip < image_hi && runtime_redzone_function(rip - image_lo, &begin) ? image_lo + (uintptr_t)begin : rip;
}
static unsigned host_module_of(uintptr_t address) {
    for (unsigned i = 0; i < host_module_count; ++i)
        if (address >= host_modules[i].lo && address < host_modules[i].hi) return i;
    return MAX_HOST_MODULES;
}
/* The first guest code address in the stack above rsp: no locks and no allocation, the thread is suspended. */
static uintptr_t guest_caller(uintptr_t rsp) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)rsp, &m, sizeof(m)) || m.State != MEM_COMMIT) return 0;
    uintptr_t end = (uintptr_t)m.BaseAddress + m.RegionSize;
    if (end - rsp > 65536) end = rsp + 65536;
    for (const uintptr_t *at = (const uintptr_t *)(rsp & ~(uintptr_t)7); (uintptr_t)(at + 1) <= end; ++at)
        if (guest_code(*at)) return *at;
    return 0;
}
static void sample(void) {
    if (SuspendThread(main_thread) == (DWORD)-1) return;
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_CONTROL;
    uint64_t key = 0;
    unsigned kind = 0, module = MAX_HOST_MODULES;
    if (GetThreadContext(main_thread, &c)) {
        const uintptr_t rip = (uintptr_t)c.Rip;
        if (guest_code(rip)) {
            kind = KIND_GUEST;
            key = make_key(kind, 0, guest_function(rip));
        } else {
            module = host_module_of(rip);
            kind = module < MAX_HOST_MODULES ? KIND_HOST : KIND_UNKNOWN;
            key = make_key(kind, module < MAX_HOST_MODULES ? module : 0, guest_caller((uintptr_t)c.Rsp));
        }
    }
    ResumeThread(main_thread);
    if (!kind) return;
    ++samples;
    ++kind_samples[kind];
    if (kind == KIND_HOST) ++host_module_samples[module];
    count_key(key);
}
static DWORD WINAPI sampler(void *unused) {
    (void)unused;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -20000;
    while (__atomic_load_n(&active, __ATOMIC_ACQUIRE)) {
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        if (__atomic_load_n(&active, __ATOMIC_ACQUIRE)) sample();
    }
    if (timer) CloseHandle(timer);
    return 0;
}
static void list_host_modules(void) {
    HMODULE handles[MAX_HOST_MODULES];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &bytes)) return;
    const unsigned n = bytes / sizeof(*handles) < MAX_HOST_MODULES ? bytes / sizeof(*handles) : MAX_HOST_MODULES;
    for (unsigned i = 0; i < n; ++i) {
        MODULEINFO info;
        if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) continue;
        host_modules[host_module_count].lo = (uintptr_t)info.lpBaseOfDll;
        host_modules[host_module_count].hi = (uintptr_t)info.lpBaseOfDll + info.SizeOfImage;
        if (!GetModuleBaseNameA(GetCurrentProcess(), handles[i], host_modules[host_module_count].name,
                                sizeof(host_modules[host_module_count].name)))
            strcpy(host_modules[host_module_count].name, "?");
        ++host_module_count;
    }
}

void sos_startup_profile_begin(const void *image, uint64_t image_size) {
    if (main_thread || __atomic_exchange_n(&window, 1, __ATOMIC_ACQ_REL)) return;
    const char *wanted = getenv("SOS_STARTUP_PROFILE");
    if (!wanted || strcmp(wanted, "1")) return;
    image_lo = (uintptr_t)image;
    image_hi = image_lo + (uintptr_t)image_size;
    list_host_modules();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        main_thread = NULL;
        return;
    }
    FILETIME unused;
    GetThreadTimes(main_thread, &unused, &unused, &main_cpu_start[0], &main_cpu_start[1]);
    GetProcessTimes(GetCurrentProcess(), &unused, &unused, &process_cpu_start[0], &process_cpu_start[1]);
    QueryPerformanceCounter(&window_start);
    __atomic_store_n(&active, 1, __ATOMIC_RELEASE);
    sampler_thread = CreateThread(NULL, 64 << 10, sampler, NULL, 0, NULL);
    if (sampler_thread) SetThreadPriority(sampler_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

static double filetime_seconds(const FILETIME *from, const FILETIME *to) {
    const uint64_t a = (uint64_t)from->dwHighDateTime << 32 | from->dwLowDateTime;
    const uint64_t b = (uint64_t)to->dwHighDateTime << 32 | to->dwLowDateTime;
    return b > a ? (double)(b - a) / 1e7 : 0;
}
static double percent(uint32_t part) { return samples ? 100.0 * part / samples : 0; }
static void describe_key(uint64_t key, char *out, size_t size) {
    const unsigned kind = (unsigned)(key >> 56), module = (unsigned)(key >> 48) & 0xff;
    uintptr_t address = (uintptr_t)(key & 0xffffffffffffull);
    char where[200] = "no guest frame";
    if (address) {
        uint64_t begin, site;
        if (runtime_redzone_site(address, &site)) address = image_lo + (uintptr_t)site;
        if (address >= image_lo && address < image_hi && runtime_redzone_function(address - image_lo, &begin))
            snprintf(where, sizeof(where), "guest image+0x%" PRIx64 " (function image+0x%" PRIx64 ")",
                     (uint64_t)(address - image_lo), begin);
        else
            runtime_describe_guest_address(address, where, sizeof(where));
    }
    if (kind == KIND_GUEST) snprintf(out, size, "running %s", where);
    else if (kind == KIND_HOST) snprintf(out, size, "in %s, called from %s", host_modules[module].name, where);
    else snprintf(out, size, "outside known code, called from %s", where);
}
static void report(double wall) {
    FILETIME unused, main_now[2], process_now[2];
    GetThreadTimes(main_thread, &unused, &unused, &main_now[0], &main_now[1]);
    GetProcessTimes(GetCurrentProcess(), &unused, &unused, &process_now[0], &process_now[1]);
    const double main_kernel = filetime_seconds(&main_cpu_start[0], &main_now[0]);
    const double main_user = filetime_seconds(&main_cpu_start[1], &main_now[1]);
    const double process_cpu = filetime_seconds(&process_cpu_start[0], &process_now[0]) +
                               filetime_seconds(&process_cpu_start[1], &process_now[1]);
    printf("Startup profile: Unity bootstrap to AvPlayer source %.1f s; main thread CPU %.1f s (user %.1f, kernel %.1f), "
           "process CPU %.1f s; %u main-thread samples\n",
           wall, main_user + main_kernel, main_user, main_kernel, process_cpu, samples);
    char line[640];
    int at = snprintf(line, sizeof(line), "Startup profile: main thread %.0f%% guest code, %.0f%% unknown",
                      percent(kind_samples[KIND_GUEST]), percent(kind_samples[KIND_UNKNOWN]));
    for (int shown = 0; shown < 6; ++shown) {
        unsigned best = MAX_HOST_MODULES;
        for (unsigned i = 0; i < host_module_count; ++i)
            if (host_module_samples[i] && (best == MAX_HOST_MODULES || host_module_samples[i] > host_module_samples[best])) best = i;
        if (best == MAX_HOST_MODULES || at <= 0 || (size_t)at >= sizeof(line)) break;
        at += snprintf(line + at, sizeof(line) - (size_t)at, ", %.0f%% %s", percent(host_module_samples[best]),
                       host_modules[best].name);
        host_module_samples[best] = 0;
    }
    puts(line);
    for (int rank = 1; rank <= 12; ++rank) {
        Slot *best = NULL;
        for (size_t i = 0; i < SLOTS; ++i)
            if (slots[i].count && (!best || slots[i].count > best->count)) best = &slots[i];
        if (!best || percent(best->count) < 0.5) break;
        char where[300];
        describe_key(best->key, where, sizeof(where));
        printf("Startup profile: %2d. %5.1f%% %s\n", rank, percent(best->count), where);
        best->count = 0;
    }
    static const char *const names[SOS_COST_COUNT] = {
        "file opens", "file reads", "file stats", "memory maps", "memory unmaps and protections", "thread creations",
        "module loads", "dlsym", "sleeps", "GPU page faults", "emulated instructions",
    };
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    at = snprintf(line, sizeof(line), "Startup profile: host services, wall time summed over threads:");
    for (int k = 0; k < SOS_COST_COUNT && at > 0 && (size_t)at < sizeof(line); ++k) {
        const double seconds = (double)costs[k].ticks / (double)hz.QuadPart;
        at += snprintf(line + at, sizeof(line) - (size_t)at, "%s %s %lld (%.2f s", k ? "," : "", names[k],
                       (long long)costs[k].calls, seconds);
        if (k == SOS_COST_FILE_READ) at += snprintf(line + at, sizeof(line) - (size_t)at, ", %.0f MiB", costs[k].amount / 1048576.0);
        if (k == SOS_COST_SLEEP) at += snprintf(line + at, sizeof(line) - (size_t)at, ", %.2f s asked", costs[k].amount / 1e9);
        if (at > 0 && (size_t)at < sizeof(line)) at += snprintf(line + at, sizeof(line) - (size_t)at, ")");
    }
    puts(line);
}

void sos_startup_profile_end(void) {
    if (!__atomic_exchange_n(&window, 0, __ATOMIC_ACQ_REL) || !__atomic_exchange_n(&active, 0, __ATOMIC_ACQ_REL)) return;
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    if (sampler_thread) {
        WaitForSingleObject(sampler_thread, 1000);
        CloseHandle(sampler_thread);
        sampler_thread = NULL;
    }
    report((double)(now.QuadPart - window_start.QuadPart) / (double)hz.QuadPart);
}
#else
int sos_startup_profile_active(void) { return 0; }
int64_t sos_cost_begin(void) { return 0; }
void sos_cost_end(SosCost kind, int64_t start, uint64_t amount) { (void)kind; (void)start; (void)amount; }
void sos_startup_profile_begin(const void *image, uint64_t image_size) { (void)image; (void)image_size; }
void sos_startup_profile_end(void) {}
#endif
