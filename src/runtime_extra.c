/* libc start-up contracts the shared runtime files do not own. */
#include "runtime.h"
#include <stdio.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))

static ABI int32_t ok_noop(void) { return 0; }
static ABI void *null_pointer(void) { return NULL; }
static ABI int32_t not_found(void) { return ERR(3); }

static ABI int32_t thread_name(void *thread, char *name) {
    if (!name) return ERR(22);
    const char *n=runtime_thread_name(thread);
    if (!n) return ERR(3);
    snprintf(name,32,"%s",n);
    return 0;
}

/* POSIX pthread APIs return errno values, not SCE codes. */
typedef int32_t (ABI *RwOne)(void **);
static int32_t posix_code(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
#define POSIX_RW(name,nid) static ABI int32_t posix_rw_##name(void **rw) { \
    return posix_code(((RwOne)runtime_rwlock_resolve(nid))(rw)); }
POSIX_RW(init,"6ULAa0fq4jA#libkernel")
POSIX_RW(destroy,"BB+kb08Tl9A#libkernel")
POSIX_RW(rdlock,"Ox9i0c7L5w0#libkernel")
POSIX_RW(wrlock,"mqdNorrB+gI#libkernel")
POSIX_RW(tryrdlock,"XD3mDeybCnk#libkernel")
POSIX_RW(trywrlock,"bIHoZCTomsI#libkernel")
POSIX_RW(unlock,"+L98PIbGttk#libkernel")

typedef int32_t (ABI *EqualOne)(void *, void *);
static ABI int32_t posix_thread_equal(void *first, void *second) {
    return ((EqualOne)runtime_thread_resolve("3PtV6p3QNX4#libkernel"))(first, second);
}
static ABI int32_t posix_thread_detach(void *thread) {
    return posix_code(((int32_t (ABI *)(void *))runtime_thread_resolve("4qGrR6eoP9Y#libkernel"))(thread));
}

typedef int32_t (ABI *AttrSetStack)(void **, uint64_t);
typedef int32_t (ABI *AttrGetStackFull)(void **, void **, uint64_t *);
static ABI int32_t attr_set_stack(void **attr, uint64_t size) {
    return ((AttrSetStack)runtime_thread_resolve("UTXzJbWhhTE#libkernel"))(attr, size);
}
static ABI int32_t attr_get_stackaddr(void **attr, void **address) {
    uint64_t size = 0;
    return ((AttrGetStackFull)runtime_thread_resolve("-quPa4SEJUw#libkernel"))(attr, address, &size);
}

typedef int32_t (ABI *CancelOne)(void *);
static ABI int32_t posix_thread_cancel(void *thread) {
    return posix_code(((CancelOne)runtime_thread_resolve("qBDmpCyGssE#libkernel"))(thread));
}
typedef int32_t (ABI *MutexAttrProtocol)(void **, int);
static ABI int32_t mutexattr_setprotocol(void **attr, int protocol) {
    return posix_code(((MutexAttrProtocol)runtime_mutex_resolve("1FGvU0i9saQ#libkernel"))(attr, protocol));
}
static ABI int32_t sched_priority_max(int policy) {
    return (policy == 1 || policy == 3) ? 767 : 22;
}
static ABI int32_t sched_priority_min(int policy) {
    return (policy == 1 || policy == 3) ? 256 : 22;
}

static const RuntimeExport exports[]={
    {"sceKernelIsAddressSanitizerEnabled",ok_noop},
    {"sceKernelGetSanitizerMallocReplaceExternal",null_pointer},
    {"sceKernelGetSanitizerNewReplaceExternal",null_pointer},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",not_found},
    {"scePthreadGetname",thread_name},
    {"scePthreadAttrSetstack",attr_set_stack}, {"scePthreadAttrGetstackaddr",attr_get_stackaddr},
    {"pthread_rwlock_init",posix_rw_init}, {"pthread_rwlock_destroy",posix_rw_destroy},
    {"pthread_rwlock_rdlock",posix_rw_rdlock}, {"pthread_rwlock_wrlock",posix_rw_wrlock},
    {"pthread_rwlock_tryrdlock",posix_rw_tryrdlock}, {"pthread_rwlock_trywrlock",posix_rw_trywrlock},
    {"pthread_rwlock_unlock",posix_rw_unlock},
    {"pthread_equal",posix_thread_equal}, {"pthread_detach",posix_thread_detach},
    {"pthread_cancel",posix_thread_cancel}, {"pthread_mutexattr_setprotocol",mutexattr_setprotocol},
    {"sched_get_priority_max",sched_priority_max}, {"sched_get_priority_min",sched_priority_min},
};
uintptr_t runtime_extra_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
