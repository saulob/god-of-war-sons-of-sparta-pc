/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "startup_phase.h"
#include "runtime_log.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <x86intrin.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return 35;
#endif
#ifdef EINPROGRESS
    case EINPROGRESS: return 36;
#endif
#ifdef EALREADY
    case EALREADY: return 37;
#endif
#ifdef ENOTSOCK
    case ENOTSOCK: return 38;
#endif
#ifdef EDESTADDRREQ
    case EDESTADDRREQ: return 39;
#endif
#ifdef EMSGSIZE
    case EMSGSIZE: return 40;
#endif
#ifdef EPROTOTYPE
    case EPROTOTYPE: return 41;
#endif
#ifdef ENOPROTOOPT
    case ENOPROTOOPT: return 42;
#endif
#ifdef EPROTONOSUPPORT
    case EPROTONOSUPPORT: return 43;
#endif
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT: return 44;
#endif
#ifdef EOPNOTSUPP
    case EOPNOTSUPP: return 45;
#endif
#ifdef EPFNOSUPPORT
    case EPFNOSUPPORT: return 46;
#endif
#ifdef EAFNOSUPPORT
    case EAFNOSUPPORT: return 47;
#endif
#ifdef EADDRINUSE
    case EADDRINUSE: return 48;
#endif
#ifdef EADDRNOTAVAIL
    case EADDRNOTAVAIL: return 49;
#endif
#ifdef ENETDOWN
    case ENETDOWN: return 50;
#endif
#ifdef ENETUNREACH
    case ENETUNREACH: return 51;
#endif
#ifdef ENETRESET
    case ENETRESET: return 52;
#endif
#ifdef ECONNABORTED
    case ECONNABORTED: return 53;
#endif
#ifdef ECONNRESET
    case ECONNRESET: return 54;
#endif
#ifdef ENOBUFS
    case ENOBUFS: return 55;
#endif
#ifdef EISCONN
    case EISCONN: return 56;
#endif
#ifdef ENOTCONN
    case ENOTCONN: return 57;
#endif
#ifdef ESHUTDOWN
    case ESHUTDOWN: return 58;
#endif
#ifdef ETOOMANYREFS
    case ETOOMANYREFS: return 59;
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED: return 61;
#endif
#ifdef EHOSTDOWN
    case EHOSTDOWN: return 64;
#endif
#ifdef EHOSTUNREACH
    case EHOSTUNREACH: return 65;
#endif
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static struct timespec process_start;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
/* The launcher shares this process: guest process time starts when the guest does. */
void runtime_reset_process_clock(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
static int host_clock(uint32_t id,clockid_t *out) {
    switch (id) {
    case 0: case 9: case 10: *out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: *out=CLOCK_PROCESS_CPUTIME_ID; return 1;                  /* PROF */
    case 15: *out=CLOCK_MONOTONIC; return 1;                          /* PROCTIME: since the guest started */
    case 16: case 17: case 18: case 19: *out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    clockid_t host;
    if (!ts) return EFAULT;
    if (!host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    if (id==15) {
        t.tv_sec-=process_start.tv_sec; t.tv_nsec-=process_start.tv_nsec;
        if (t.tv_nsec<0) { --t.tv_sec; t.tv_nsec+=1000000000; }
    }
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    clockid_t host; struct timespec t;
    if (!ts) return fail_posix(EFAULT);
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
    return 0;
}
static ABI int32_t kernel_clock_getres(uint32_t id,GuestTimespec *ts) {
    return posix_clock_getres(id,ts) ? ERR(*runtime_errno()) : 0;
}
static ABI uint64_t process_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000+(uint64_t)((t.tv_nsec-process_start.tv_nsec)/1000);
}
static ABI uint64_t process_time_counter(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000000+(uint64_t)(t.tv_nsec-process_start.tv_nsec);
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
static ABI uint64_t read_tsc(void) { return __rdtsc(); }
static uint64_t tsc_hz;
static ABI uint64_t tsc_frequency(void) {
    if (!tsc_hz) {
        struct timespec a,b,nap={0,20000000};
        clock_gettime(CLOCK_MONOTONIC,&a); uint64_t t0=__rdtsc();
#ifdef _WIN32
        (void)nap; compat_sleep_ns(20000000);
#else
        nanosleep(&nap,NULL);
#endif
        clock_gettime(CLOCK_MONOTONIC,&b); uint64_t t1=__rdtsc();
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000+(uint64_t)(b.tv_nsec-a.tv_nsec);
        tsc_hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return tsc_hz;
}
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
static int sleep_ns(uint64_t ns) {
    runtime_thread_sleep_ns(ns);
    return 0;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t kernel_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    return posix_nanosleep(rq,rem) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now=time(NULL);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-compat_utc_offset(now)/60); tz->dsttime=0;
#else
    struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
/* Seconds east of UTC at t, daylight saving included; *dst tells whether daylight saving is in effect. */
static long zone_offset(time_t t, int *dst) {
    struct tm local;
    if (!localtime_r(&t,&local)) { *dst=0; return 0; }
    *dst=local.tm_isdst>0;
#ifdef _WIN32
    return compat_utc_offset(t);
#else
    return local.tm_gmtoff;
#endif
}
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } GuestTimesec;
/* west_sec carries the standard-time offset east of UTC, as the console reports it. */
static ABI int32_t utc_to_localtime(int64_t utc,int64_t *local,GuestTimesec *st,uint64_t *dst_sec) {
    int dst;
    const long offset=zone_offset((time_t)utc,&dst);
    const uint32_t dst_part=dst ? 3600 : 0;
    if (local) *local=utc+offset;
    if (st) { st->t=utc; st->west_sec=(uint32_t)(offset-(long)dst_part); st->dst_sec=dst_part; }
    if (dst_sec) *dst_sec=dst_part;
    return 0;
}
static ABI int32_t localtime_to_utc(int64_t local,int64_t type,int64_t *utc,GuestTimezone *tz,int32_t *dst_sec) {
    (void)type;
    if (!tz) return ERR(22);
    kernel_gettimezone(tz);
    if (utc) *utc=local+(int64_t)tz->minuteswest*60;
    if (dst_sec) *dst_sec=tz->dsttime*60;
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (tv) { tv->sec=t.tv_sec; tv->usec=t.tv_usec; }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
/* The guest cannot change the host clock; setting it is accepted and ignored. */
static ABI int32_t posix_settimeofday(const GuestTimeval *tv,const GuestTimezone *tz) { (void)tv; (void)tz; return 0; }
static ABI int32_t posix_clock_settime(int32_t id,const GuestTimespec *ts) { (void)id; (void)ts; return 0; }
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }
static ABI int32_t set_gpo(int32_t value) { (void)value; return 0; }
static ABI int32_t get_gpi(void) { return 0; }
static ABI uint32_t get_uid(void) { return 1; }
static ABI int32_t get_current_cpu(void) { return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    sos_startup_phase("guest exit requested");
    runtime_log_exit_request("_exit",status,(uintptr_t)__builtin_return_address(0));
    runtime_report();
    runtime_online_leave();
    fflush(stdout);
    if (status == 0) runtime_log_mark_clean_exit();
    runtime_log_finalize();
    _exit(status);
}
static ABI __attribute__((noreturn)) void kernel_exit_process(int32_t status) {
    sos_startup_phase("guest exit requested");
    runtime_log_exit_request("sceKernelExitProcess",status,(uintptr_t)__builtin_return_address(0));
    runtime_online_leave();
    if (status==0) runtime_log_mark_clean_exit();
    runtime_log_finalize();
    runtime_exit(status);
}
static ABI __attribute__((noreturn)) void hard__Exit(int32_t status) {
    sos_startup_phase("guest exit requested");
    runtime_log_exit_request("_Exit",status,(uintptr_t)__builtin_return_address(0));
    runtime_online_leave();
    if (status==0) runtime_log_mark_clean_exit();
    runtime_log_finalize();
    runtime_exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    char where[320];
    runtime_describe_guest_address((uintptr_t)__builtin_return_address(0),where,sizeof(where));
    fprintf(stderr,"STOP: guest raised debug exception %s code=0x%x argument=0x%llx from %s\n",runtime_debug_stop_name(code),code,
            (unsigned long long)argument,where);
    runtime_report();
    runtime_log_flush();
    _exit(23); /* GPU and guest threads are running: exit() would run destructors under them */
}
/* The process was started without arguments other than its own name. */
static const char *guest_argv[]={"eboot.bin",NULL};
static ABI int32_t get_argc(void) { return 1; }
static ABI const char **get_argv(void) { return guest_argv; }
static ABI int32_t kernel_mlock(const void *address,uint64_t size) { (void)address; (void)size; return 0; }
/* libc's C++ exception unwinder. The game does not throw (no __cxa_throw import): unwinding
 * finds no frame data, and resuming a context is a stop, never a silent success. */
/* FreeBSD's __elf_phdr_match_addr over a dl_phdr_info: 1 when `address` lies in a PT_LOAD segment. */
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } ElfPhdr;
typedef struct { uint64_t base; const char *name; const ElfPhdr *phdr; uint16_t count; } PhdrInfo;
static ABI int32_t elf_phdr_match_addr(const PhdrInfo *info,uint64_t address) {
    if (!info || !info->phdr) return 0;
    for (uint16_t i=0;i<info->count;++i) {
        const ElfPhdr *p=&info->phdr[i];
        if (p->type==1 && address>=info->base+p->vaddr && address-info->base-p->vaddr<p->memsz) return 1;
    }
    return 0;
}
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
static ABI __attribute__((noreturn)) void unwind_resume(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn/__Ux86_64_setcontext) is not supported\n",stderr);
    runtime_report();
    runtime_log_flush();
    _exit(23); /* GPU and guest threads are running: exit() would run destructors under them */
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(who,&user,&kernel);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){kernel/1000000,kernel%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (compat_random(old,(size_t)*oldlen)) return fail_posix(EIO);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    static int reported;
    if (__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<16) {
        fprintf(stderr,"Runtime: unsupported sysctl mib");
        for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
        fputc('\n',stderr);
    }
    return fail_posix(ENOENT);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; uint32_t generation; KeyDestructor destructor; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static _Thread_local uint32_t key_value_generation[KEYS];
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor; ++keys[i].generation;
        pthread_mutex_unlock(&key_lock);
        *key=i; return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].destructor=NULL;
    pthread_mutex_unlock(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *value) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    pthread_mutex_lock(&key_lock);
    key_value_generation[key]=keys[key].generation;
    pthread_mutex_unlock(&key_lock);
    key_values[key]=value; return 0;
}
static ABI void *key_get(uint32_t key) {
    if (!key || key>=KEYS) return NULL;
    pthread_mutex_lock(&key_lock);
    const int live=keys[key].used && key_value_generation[key]==keys[key].generation;
    pthread_mutex_unlock(&key_lock);
    return live ? key_values[key] : NULL;
}
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *value=key_values[i];
            if (!value) continue;
            pthread_mutex_lock(&key_lock);
            const int live=keys[i].used && key_value_generation[i]==keys[i].generation;
            const KeyDestructor destructor=live ? keys[i].destructor : NULL;
            pthread_mutex_unlock(&key_lock);
            key_values[i]=NULL;
            if (!destructor) continue;
            any=1;
            destructor(value);
        }
        if (!any) break;
    }
}
static ABI int32_t posix_key_create(uint32_t *k,KeyDestructor d) { int32_t r=key_create(k,d); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *v) { int32_t r=key_set(k,v); return r ? r&0xffff : 0; }

static const RuntimeExport exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep}, {"sceKernelSleep",kernel_sleep},
    {"nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep}, {"_nanosleep",kernel_nanosleep},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"sceKernelSetGPO",set_gpo}, {"sceKernelGetGPI",get_gpi}, {"getuid",get_uid},
    {"sceKernelGetCurrentCpu",get_current_cpu},
    {"sceKernelClockGetres",kernel_clock_getres},
    {"settimeofday",posix_settimeofday}, {"sceKernelSettimeofday",posix_settimeofday},
    {"clock_settime",posix_clock_settime},
    {"_exit",hard_exit}, {"_Exit",hard__Exit}, {"sceKernelExitProcess",kernel_exit_process},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"getargc",get_argc}, {"getargv",get_argv},
    {"sceKernelMlock",kernel_mlock},
    {"sceKernelConvertUtcToLocaltime",utc_to_localtime}, {"sceKernelConvertLocaltimeToUtc",localtime_to_utc},
    {"__elf_phdr_match_addr",elf_phdr_match_addr}, {"_is_signal_return",is_signal_return},
    {"__pthread_cxa_finalize",pthread_cxa_finalize}, {"sigreturn",unwind_resume},
    {"__Ux86_64_setcontext",unwind_resume},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
};
uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
