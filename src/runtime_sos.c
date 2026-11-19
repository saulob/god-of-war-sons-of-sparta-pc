/* Imports needed by this title's boot path that no shared runtime file owns. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
#define E_SRCH 3

typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *mspace_id_mask, *mstate_table; } HeapTraceInfo;

/* libc.prx's initialiser clears *mspace_id_mask unconditionally, so both pointers must be valid. */
static ABI int32_t libc_heap_trace_info(HeapTraceInfo *info) {
    static uint64_t mask, table[64];
    if (!info) return -1;
    info->get_segment_info = 0;
    info->mspace_id_mask = &mask;
    info->mstate_table = table;
    return 0;
}

static ABI int32_t ok_noop(void) { return 0; }

static int32_t neo_mode;
static ABI int32_t is_neo_mode(void) { return neo_mode; }
void runtime_sos_set_neo_mode(int on) { neo_mode = on != 0; }

/* libkernel_cpumode_platform: a base PS4 (the Pro memory path is not selected). */
static ABI int32_t is_prospero(void) { return 0; }

/* libSceDiscMap: a digital install has no disc bitmap, nothing is pending. */
static ABI int32_t discmap_request_state(const char *path, int64_t offset, int64_t size, int32_t *flags, int32_t *first,
                                         int32_t *second) {
    (void)path; (void)offset; (void)size;
    if (flags) *flags = 0;
    if (first) *first = 0;
    if (second) *second = 0;
    return 0;
}

/* Profiler markers and the PS4 object-handle helper. */
static ABI int32_t razor_cpu_ok(void) { return 0; }
static ABI int32_t ulobjmgr_register(uint64_t object, int32_t kind, uint32_t *id) {
    if (!object || !kind || !id) return 22;
    *id = 0;
    return 0;
}
static ABI int32_t ulobjmgr_unregister(uint32_t id) { return id >= 0x4000 ? 22 : 0; }

static ABI int32_t pthread_testcancel(void) { runtime_thread_test_cancel("testcancel"); return 0; }
static ABI int32_t namedobj_create(void) { return ERR(E_SRCH); }
static ABI int32_t namedobj_delete(void) { return ERR(E_SRCH); }

/* Process CPU mode. */
static ABI int32_t get_cpumode(void) { return 0; }

/* A random (version 4) 128-bit id. */
static ABI int32_t uuid_create(void *uuid) {
    if (!uuid) return ERR(22);
    unsigned char *out = uuid;
#ifdef _WIN32
    if (compat_random(out, 16)) return ERR(14);
#else
    FILE *source = fopen("/dev/urandom", "rb");
    if (!source || fread(out, 1, 16, source) != 16) { if (source) fclose(source); return ERR(14); }
    fclose(source);
#endif
    out[6] = (unsigned char)((out[6] & 0x0f) | 0x40);
    out[8] = (unsigned char)((out[8] & 0x3f) | 0x80);
    return 0;
}

/* Console identity queries: a retail base PS4 in the sandbox. */
static ABI int32_t has_neo_mode(void) { return neo_mode; }
static ABI int32_t is_devkit(void) { return 0; }
static ABI int32_t is_cex(void) { return 1; }
static ABI int32_t is_in_sandbox(void) { return 1; }
static ABI int32_t get_main_soc_id(void) { return neo_mode ? 0x740f30 : 0x710f10; }
static ABI int32_t get_process_type(int32_t pid) { return pid == 1000 || pid == -1 || pid == 0 ? 0 : ERR(E_SRCH); }
static ABI void debug_out_text(void *unused, const char *text) {
    (void)unused;
    if (text) { fputs(text, stdout); fflush(stdout); }
}

static const RuntimeExport exports[] = {
    {"sceKernelIsNeoMode", is_neo_mode},
    {"sceKernelIsProspero", is_prospero},
    {"sceKernelGetCpumode", get_cpumode},
    {"sceGameLiveStreamingInitialize", ok_noop}, {"sceGameLiveStreamingTerminate", ok_noop},
    {"sceCoredumpRegisterCoredumpHandler", ok_noop}, {"sceCoredumpWriteUserData", ok_noop},
    {"sceLibcHeapGetTraceInfo", libc_heap_trace_info},
    {"sceDiscMap_7C980FFB0AA27E7A", discmap_request_state},
    {"sceRazorCpuJobManagerSequence", razor_cpu_ok},
    {"sceRazorCpuJobManagerJob", razor_cpu_ok},
    {"sceRazorCpuJobManagerDispatch", razor_cpu_ok},
    {"_sceUlobjmgrRegisterObject", ulobjmgr_register},
    {"_sceUlobjmgrUnregisterObject", ulobjmgr_unregister},
    {"pthread_testcancel", pthread_testcancel},
    {"__sys_namedobj_create", namedobj_create},
    {"__sys_namedobj_delete", namedobj_delete},
    {"sceKernelUuidCreate", uuid_create},
    {"sceKernelHasNeoMode", has_neo_mode}, {"sceKernelIsDevkit", is_devkit}, {"sceKernelIsCEX", is_cex},
    {"sceKernelIsInSandbox", is_in_sandbox}, {"sceKernelGetMainSocId", get_main_soc_id},
    {"sceKernelGetProcessType", get_process_type}, {"sceKernelDebugOutText", debug_out_text},
};
uintptr_t runtime_sos_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
