/* Explicit guest timeval layout; host pointers never escape this boundary. */
#define _DEFAULT_SOURCE
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int guest_gettimeofday(GuestTimeval *value,GuestTimezone *zone) {
    struct timeval t;
    if (gettimeofday(&t,NULL)) { *runtime_errno()=runtime_guest_errno(errno); return -1; }
    if (zone) {
#ifdef _WIN32
        zone->minuteswest=(int32_t)(-compat_utc_offset((time_t)t.tv_sec)/60);
#else
        struct tm local;
        const time_t now=(time_t)t.tv_sec;
        localtime_r(&now,&local);
        zone->minuteswest=(int32_t)(-local.tm_gmtoff/60);
#endif
        zone->dsttime=0;
    }
    if (value) { value->seconds=t.tv_sec; value->microseconds=t.tv_usec; }
    return 0;
}
uintptr_t runtime_time_resolve(const char *name) {
    if (!strcmp(name,"n88vx3C5nW8#libkernel")) return (uintptr_t)guest_gettimeofday;
    return 0;
}
