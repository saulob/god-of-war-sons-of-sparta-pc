/* Narrow, explicit PS4 libc contracts. No automatic success stubs. */
#define _CRT_RAND_S
#include "runtime.h"
#include "startup_phase.h"
#include "dcpgpu.h"
#include "runtime_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <pthread.h>

typedef struct {
    union { GuestCallback plain; void (ABI *with_arg)(void *); } callback;
    void *argument, *dso;
    int with_arg, active;
} ExitHandler;
static ExitHandler *handlers;
static size_t handler_count, handler_capacity, calls_init, calls_atexit, calls_cxa;
static uint64_t capabilities;
static _Atomic size_t guards_acquired, guards_released;
static uint64_t stack_canary;
static _Atomic size_t memory_calls;
static uintptr_t guest_image_base;
static uint64_t guest_image_size;

void runtime_set_guest_image(const void *base, uint64_t size) {
    guest_image_base=(uintptr_t)base;
    guest_image_size=size;
}
int runtime_is_guest_code(uintptr_t address) {
    if (guest_image_base && address>=guest_image_base && address-guest_image_base<guest_image_size) return 1;
    return runtime_redzone_contains(address) || runtime_module_contains(address);
}
void runtime_describe_guest_address(uintptr_t address, char *out, size_t size) {
    if (!size) return;
    if (runtime_module_describe_address(address,out,size)) return;
    if (guest_image_base && address>=guest_image_base && address-guest_image_base<guest_image_size) {
        snprintf(out,size,"guest image+0x%llx",(unsigned long long)(address-guest_image_base));
        return;
    }
    snprintf(out,size,"host address 0x%llx",(unsigned long long)address);
}
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) {
    char where[320];
    runtime_describe_guest_address(caller,where,sizeof(where));
    printf("Runtime: %s(%d) called from %s; startup phase=%s, module operation=%s\n",
           api,status,where,sos_startup_phase_current(),runtime_module_operation());
    runtime_log_flush();
}
/* Module ID 1 is the executable's initial-exec TLS; other modules use lazy per-thread blocks. */
#define TLS_MODULES 64
typedef struct { const void *data; uint64_t filesz, memsz, align; int used; } TlsModule;
typedef struct { unsigned char *data; uint64_t generation; } TlsBlock;
static TlsModule tls_modules[TLS_MODULES];
static _Thread_local TlsBlock tls_blocks[TLS_MODULES];
static pthread_mutex_t tls_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t tls_generation=1;
static void *process_param;
void runtime_set_procparam(void *param) { process_param=param; }
static ABI void *guest_procparam(void) { return process_param; }
static void **application_heap_api;
static ABI void guest_set_heap_api(void **api) {
    application_heap_api=api;
    puts("Runtime: application heap API registered");
}
void **runtime_application_heap_api(void) { return application_heap_api; }
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz) {
    if (module<2 || module>=TLS_MODULES || filesz>memsz || (data==NULL && filesz)) {
        fputs("STOP: invalid module TLS template\n",stderr); runtime_exit(21);
    }
    pthread_mutex_lock(&tls_lock);
    tls_modules[module]=(TlsModule){data,filesz,memsz,16,1};
    atomic_fetch_add_explicit(&tls_generation,1,memory_order_release);
    pthread_mutex_unlock(&tls_lock);
}
uint64_t runtime_add_module_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align) {
    if (filesz>memsz || (data==NULL && filesz) || !align || (align&(align-1))) return 0;
    pthread_mutex_lock(&tls_lock);
    uint64_t module=0;
    for (uint64_t i=2;i<TLS_MODULES;++i) if (!tls_modules[i].used) { module=i; break; }
    if (module) {
        tls_modules[module]=(TlsModule){data,filesz,memsz,align,1};
        atomic_fetch_add_explicit(&tls_generation,1,memory_order_release);
    }
    pthread_mutex_unlock(&tls_lock);
    return module;
}
void runtime_remove_module_tls(uint64_t module) {
    if (module<2 || module>=TLS_MODULES) return;
    pthread_mutex_lock(&tls_lock);
    memset(&tls_modules[module],0,sizeof(tls_modules[module]));
    atomic_fetch_add_explicit(&tls_generation,1,memory_order_release);
    pthread_mutex_unlock(&tls_lock);
}
static void *tls_block_alloc(uint64_t size,uint64_t align) {
    if (align<16) align=16;
#ifdef _WIN32
    return _aligned_malloc((size_t)size,(size_t)align);
#else
    void *block=NULL;
    if (posix_memalign(&block,(size_t)align,(size_t)size)) return NULL;
    return block;
#endif
}
static void tls_block_free(void *block) {
#ifdef _WIN32
    _aligned_free(block);
#else
    free(block);
#endif
}
void runtime_module_tls_release(void) {
    for (int module=2;module<TLS_MODULES;++module) {
        tls_block_free(tls_blocks[module].data);
        tls_blocks[module].data=NULL;
    }
}
void *runtime_module_tls_addr(uint64_t module, uint64_t offset) {
    if (module<2 || module>=TLS_MODULES) return NULL;
    pthread_mutex_lock(&tls_lock);
    if (!tls_modules[module].used || offset>=tls_modules[module].memsz) {
        pthread_mutex_unlock(&tls_lock);
        return NULL;
    }
    const uint64_t generation=atomic_load_explicit(&tls_generation,memory_order_acquire);
    TlsBlock *slot=&tls_blocks[module];
    if (slot->data && slot->generation!=generation) {
        tls_block_free(slot->data);
        slot->data=NULL;
    }
    if (!slot->data) {
        slot->data=tls_block_alloc(tls_modules[module].memsz,tls_modules[module].align);
        if (slot->data) {
            memset(slot->data,0,(size_t)tls_modules[module].memsz);
            if (tls_modules[module].filesz)
                memcpy(slot->data,tls_modules[module].data,(size_t)tls_modules[module].filesz);
            slot->generation=generation;
        }
    }
    void *result=slot->data ? slot->data+offset : NULL;
    pthread_mutex_unlock(&tls_lock);
    return result;
}
uint64_t runtime_tls_generation(void) { return atomic_load_explicit(&tls_generation,memory_order_acquire); }
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz) { runtime_set_module_tls(2,data,filesz,memsz); }
static ABI void *guest_tls_get_addr(const uint64_t *index) {
    if (!index) { fputs("STOP: null TLS descriptor\n",stderr); runtime_exit(21); }
    void *result=runtime_module_tls_addr(index[0],index[1]);
    if (!result) { fputs("STOP: unsupported TLS module/offset\n",stderr); runtime_exit(21); }
    return result;
}

void runtime_start(uint64_t flags) {
    capabilities = flags;
    if (!(flags & 1)) return;
#ifdef _WIN32
    unsigned int halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1])) { fputs("Cannot initialize stack canary\n", stderr); runtime_exit(1); }
    memcpy(&stack_canary, halves, sizeof(stack_canary));
#else
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random || fread(&stack_canary, sizeof(stack_canary), 1, random) != 1) {
        fputs("Cannot initialize stack canary\n", stderr); runtime_exit(1);
    }
    fclose(random);
#endif
    stack_canary &= ~UINT64_C(255);
}
void runtime_report(void) {
    printf("Runtime: _init_env=%zu, atexit=%zu, __cxa_atexit=%zu, registered handlers=%zu\n",
           calls_init, calls_atexit, calls_cxa, handler_count);
    printf("Runtime: static guards acquired=%zu, released=%zu\n", (size_t)guards_acquired, (size_t)guards_released);
    runtime_thread_report();
    runtime_mutex_report();
    runtime_rwlock_report();
    runtime_sema_report();
    runtime_fiber_report();
    runtime_audio_report();
    runtime_audio3d_report();
    runtime_ajm_report();
    runtime_json_report();
    runtime_pad_report();
    runtime_move_report();
    runtime_content_report();
    runtime_memory_report();
    runtime_savedata_report();
    runtime_file_report();
    printf("Runtime: memory operations=%zu\n", (size_t)memory_calls);
}
static ABI void init_env(void) {
    /* The boot image builder verified that the supplied libc export is a bare RET. */
    ++calls_init;
    puts("Runtime: _init_env returned (verified libc implementation: RET)");
}
static pthread_mutex_t handler_lock = PTHREAD_MUTEX_INITIALIZER;
static int register_handler(ExitHandler value) {
    pthread_mutex_lock(&handler_lock);
    if (handler_count == handler_capacity) {
        size_t capacity = handler_capacity ? handler_capacity * 2 : 64;
        ExitHandler *next = capacity > 1024 * 1024 ? NULL : realloc(handlers, capacity * sizeof(*handlers));
        if (!next) { pthread_mutex_unlock(&handler_lock); return -1; }
        handlers = next; handler_capacity = capacity;
    }
    value.active = 1; handlers[handler_count++] = value;
    pthread_mutex_unlock(&handler_lock);
    return 0;
}
static ABI int guest_atexit(GuestCallback callback) {
    if (!callback) return -1;
    ++calls_atexit;
    return register_handler((ExitHandler){.callback.plain=callback});
}
static ABI int guest_cxa_atexit(void (ABI *callback)(void *), void *argument, void *dso) {
    if (!callback) return -1;
    ++calls_cxa;
    return register_handler((ExitHandler){.callback.with_arg=callback, .argument=argument, .dso=dso, .with_arg=1});
}
void runtime_finalize(void *dso) {
    /* Mark before invoking: repeated or recursive finalization cannot run twice.
       Restart at the end to include handlers registered by a destructor. */
    for (;;) {
        pthread_mutex_lock(&handler_lock);
        size_t i = handler_count;
        while (i && (!handlers[i-1].active || (dso && handlers[i-1].dso != dso))) --i;
        if (!i) { pthread_mutex_unlock(&handler_lock); return; }
        ExitHandler handler = handlers[i-1]; handlers[i-1].active = 0;
        pthread_mutex_unlock(&handler_lock);
        if (handler.with_arg) handler.callback.with_arg(handler.argument);
        else handler.callback.plain();
    }
}
static ABI void guest_finalize(void *dso) { runtime_finalize(dso); }
static _Atomic uint32_t next_guard_owner=1;
static _Thread_local uint32_t guard_owner;
static ABI int guard_acquire(uint64_t *guard) {
    _Atomic uint64_t *state = (_Atomic uint64_t *)guard;
    /* Dump libc uses the low byte for completion and the dword at +4 as lock.
       The lock word holds a per-thread owner tag so recursion is detectable. */
    if (!guard_owner) guard_owner = atomic_fetch_add(&next_guard_owner, 1);
    for (;;) {
        uint64_t expected = 0;
        if (atomic_load_explicit(state, memory_order_acquire) & 1) return 0;
        if (atomic_compare_exchange_strong_explicit(state, &expected, (uint64_t)guard_owner << 32,
                                                    memory_order_acq_rel, memory_order_acquire)) break;
        if (expected & 1) return 0;
        if ((uint32_t)(expected >> 32) == guard_owner) {
            fputs("STOP: recursive static initialization\n", stderr);
            runtime_exit(21);
        }
        sched_yield(); /* another thread is running the initializer */
    }
    atomic_fetch_add(&guards_acquired, 1);
    return 1;
}
static ABI void guard_release(uint64_t *guard) {
    atomic_store_explicit((_Atomic uint64_t *)guard, 1, memory_order_release);
    atomic_fetch_add(&guards_released, 1);
}
static ABI void guard_abort(uint64_t *guard) {
    atomic_fetch_and_explicit((_Atomic uint64_t *)guard, UINT64_C(0xffffffff), memory_order_release);
}
static ABI __attribute__((noreturn)) void stack_fail(void) {
    fputs("STOP: guest stack protector detected corruption\n", stderr);
    runtime_exit(22);
}
static ABI void *guest_memset(void *dst, int value, size_t size) {
    atomic_fetch_add_explicit(&memory_calls,1,memory_order_relaxed); return memset(dst, value, size);
}
static ABI void *guest_memcpy(void *dst, const void *src, size_t size) {
    atomic_fetch_add_explicit(&memory_calls,1,memory_order_relaxed); return memcpy(dst, src, size);
}
static ABI void *guest_memmove(void *dst, const void *src, size_t size) {
    atomic_fetch_add_explicit(&memory_calls,1,memory_order_relaxed); return memmove(dst, src, size);
}
static ABI int guest_memcmp(const void *a, const void *b, size_t size) {
    atomic_fetch_add_explicit(&memory_calls,1,memory_order_relaxed); return memcmp(a, b, size);
}
static ABI size_t guest_strlen(const char *text) { return strlen(text); }
static ABI __attribute__((noreturn)) void guest_libc_exit(int status) {
    sos_startup_phase("guest exit requested");
    runtime_log_exit_request("exit",status,(uintptr_t)__builtin_return_address(0));
    runtime_finalize(NULL);
    runtime_report();
    runtime_online_leave();
    dcpgpu_touch_release(); /* Gamescope touch mode back to each root: nothing tears down after _Exit */
    if (status == 0) runtime_log_mark_clean_exit();
    runtime_log_finalize();
    runtime_exit(status);
}
static ABI __attribute__((noreturn)) void guest_quick_exit(int status) {
    sos_startup_phase("guest exit requested");
    runtime_log_exit_request("quick_exit",status,(uintptr_t)__builtin_return_address(0));
    runtime_report();
    runtime_online_leave();
    dcpgpu_touch_release(); /* Gamescope touch mode back to each root: nothing tears down after _Exit */
    if (status==0) runtime_log_mark_clean_exit();
    runtime_log_finalize();
    runtime_exit(status);
}
static ABI __attribute__((noreturn)) void guest_abort(void) {
    sos_startup_phase("guest abort requested");
    runtime_log_exit_request("abort",134,(uintptr_t)__builtin_return_address(0));
    runtime_report();
    dcpgpu_touch_release(); /* Gamescope touch mode back to each root: nothing tears down after _Exit */
    runtime_log_mark_crash();
    runtime_log_finalize();
    runtime_exit(134);
}
/* Imports are spelled NID#library by the boot image builder, the same for every game. libScePosix
 * exports libkernel's symbols with the same contracts, so both share the libkernel key. */
static const char *canonical_name(const char *name, char out[128]) {
    const char *posix=strstr(name,"#libScePosix");
    if (!posix || posix[12] || posix-name>100) return name;
    snprintf(out,128,"%.*s#libkernel",(int)(posix-name),name);
    return out;
}
uintptr_t runtime_resolve(const char *name, int is_data) {
    if (!(capabilities & 1)) return 0;
    char canonical[128];
    name=canonical_name(name,canonical);
    if (is_data) {
        static int32_t need_libc_internal = 1; /* SDK marker variable referenced by Fios2 */
        if (!strcmp(name, "f7uOxY9mM1U#libkernel")) return (uintptr_t)&stack_canary;
        if (!strcmp(name, "ZT4ODD2Ts9o#libSceLibcInternal")) return (uintptr_t)&need_libc_internal;
        static const char *progname = "eboot.bin";
        if (!strcmp(name, "djxxOmW6-aw#libkernel")) return (uintptr_t)&progname;
        return 0;
    }
    /* Exact imports; the suffix is the exporting library. */
    if (!strcmp(name, "bzQExy189ZI#libc")) return (uintptr_t)init_env;
    if (!strcmp(name, "8G2LB+A3rzg#libc")) return (uintptr_t)guest_atexit;
    if (!strcmp(name, "tsvEmnenz48#libc")) return (uintptr_t)guest_cxa_atexit;
    if (!strcmp(name, "uMei1W9uyNo#libc")) return (uintptr_t)guest_libc_exit;
    if (!strcmp(name, "qdGFBoLVNKI#libc")) return (uintptr_t)guest_quick_exit;
    if (!strcmp(name, "L1SBTkC+Cvw#libc")) return (uintptr_t)guest_abort;
    /* Not imported by the current eboot, exposed for ABI tests/future use. */
    if (!strcmp(name, "H2e8t5ScQGc#libc")) return (uintptr_t)guest_finalize;
    if (!strcmp(name, "3GPpjQdAMTw#libc")) return (uintptr_t)guard_acquire;
    if (!strcmp(name, "9rAeANT2tyE#libc")) return (uintptr_t)guard_release;
    if (!strcmp(name, "2emaaluWzUw#libc")) return (uintptr_t)guard_abort;
    if (!strcmp(name, "Ou3iL1abvng#libkernel")) return (uintptr_t)stack_fail;
    if (!strcmp(name, "8zTFvBIAIN8#libc")) return (uintptr_t)guest_memset;
    if (!strcmp(name, "Q3VBxCXhUHs#libc")) return (uintptr_t)guest_memcpy;
    if (!strcmp(name, "+P6FRGH4LfA#libc")) return (uintptr_t)guest_memmove;
    if (!strcmp(name, "DfivPArhucg#libc")) return (uintptr_t)guest_memcmp;
    if (!strcmp(name, "j4ViWNHEgww#libc")) return (uintptr_t)guest_strlen;
    if (!strcmp(name, "vNe1w4diLCs#libkernel")) return (uintptr_t)guest_tls_get_addr;
    if (!strcmp(name, "959qrazPIrg#libkernel")) return (uintptr_t)guest_procparam;
    if (!strcmp(name, "p5EcQeEeJAE#libkernel")) return (uintptr_t)guest_set_heap_api;
    uintptr_t mutex = runtime_mutex_resolve(name);
    if (mutex) return mutex;
    uintptr_t thread = runtime_thread_resolve(name);
    if (thread) return thread;
    uintptr_t content=runtime_content_resolve(name);
    if (content) return content;
    uintptr_t timer = runtime_time_resolve(name);
    if (timer) return timer;
    uintptr_t sema = runtime_sema_resolve(name);
    if (sema) return sema;
    uintptr_t rwlock = runtime_rwlock_resolve(name);
    if (rwlock) return rwlock;
    uintptr_t memory = runtime_memory_resolve(name);
    if (memory) return memory;
    uintptr_t signal = runtime_signal_resolve(name);
    if (signal) return signal;
    uintptr_t kernel = runtime_kernel_resolve(name);
    if (kernel) return kernel;
    uintptr_t socket = runtime_posix_socket_resolve(name);
    if (socket) return socket;
    uintptr_t extra = runtime_extra_resolve(name);
    if (extra) return extra;
    uintptr_t np = runtime_np_resolve(name);
    if (np) return np;
    uintptr_t toolkit = runtime_np_toolkit2_resolve(name);
    if (toolkit) return toolkit;
    uintptr_t online = runtime_online_resolve(name);
    if (online) return online;
    uintptr_t users = runtime_users_resolve(name);
    if (users) return users;
    uintptr_t services = runtime_services_resolve(name);
    if (services) return services;
    uintptr_t rtc = runtime_rtc_resolve(name);
    if (rtc) return rtc;
    uintptr_t save = runtime_savedata_resolve(name);
    if (save) return save;
    uintptr_t file = runtime_file_resolve(name);
    if (file) return file;
    uintptr_t fiber = runtime_fiber_resolve(name);
    if (fiber) return fiber;
    uintptr_t audio = runtime_audio_resolve(name);
    if (audio) return audio;
    uintptr_t audio3d = runtime_audio3d_resolve(name);
    if (audio3d) return audio3d;
    uintptr_t ajm = runtime_ajm_resolve(name);
    if (ajm) return ajm;
    uintptr_t json = runtime_json_resolve(name);
    if (json) return json;
    uintptr_t pad = runtime_pad_resolve(name);
    if (pad) return pad;
    uintptr_t move = runtime_move_resolve(name);
    if (move) return move;
    uintptr_t font = runtime_font_resolve(name);
    if (font) return font;
    uintptr_t eventflag = runtime_eventflag_resolve(name);
    if (eventflag) return eventflag;
    uintptr_t umtx = runtime_umtx_resolve(name);
    if (umtx) return umtx;
    uintptr_t module = runtime_module_resolve(name);
    if (module) return module;
    uintptr_t title = runtime_sos_resolve(name);
    if (title) return title;
    return dcpgpu_resolve(name);
}
static const struct { const char *nid, *symbol; } import_names[]={
#include "import_names.inc"
};
typedef struct { uint32_t h[5]; uint64_t length; unsigned char block[64]; size_t used; } Sha1;
static uint32_t rotate_left(uint32_t value,unsigned bits) { return (value<<bits)|(value>>(32-bits)); }
static void sha1_block(Sha1 *ctx,const unsigned char *block) {
    uint32_t w[80];
    for (int i=0;i<16;++i)
        w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|block[i*4+3];
    for (int i=16;i<80;++i) w[i]=rotate_left(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=ctx->h[0],b=ctx->h[1],c=ctx->h[2],d=ctx->h[3],e=ctx->h[4];
    for (int i=0;i<80;++i) {
        uint32_t f,k;
        if (i<20) { f=(b&c)|(~b&d); k=0x5A827999u; }
        else if (i<40) { f=b^c^d; k=0x6ED9EBA1u; }
        else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCu; }
        else { f=b^c^d; k=0xCA62C1D6u; }
        const uint32_t next=rotate_left(a,5)+f+e+k+w[i];
        e=d; d=c; c=rotate_left(b,30); b=a; a=next;
    }
    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d; ctx->h[4]+=e;
}
static void sha1_update(Sha1 *ctx,const void *data,size_t size) {
    const unsigned char *bytes=data;
    ctx->length+=size;
    while (size) {
        const size_t take=size<64-ctx->used ? size : 64-ctx->used;
        memcpy(ctx->block+ctx->used,bytes,take);
        ctx->used+=take; bytes+=take; size-=take;
        if (ctx->used==64) { sha1_block(ctx,ctx->block); ctx->used=0; }
    }
}
static void sha1_digest(Sha1 *ctx,unsigned char out[20]) {
    const uint64_t bits=ctx->length*8;
    unsigned char pad=0x80;
    sha1_update(ctx,&pad,1);
    unsigned char zero=0;
    while (ctx->used!=56) sha1_update(ctx,&zero,1);
    unsigned char length[8];
    for (int i=0;i<8;++i) length[i]=(unsigned char)(bits>>(56-i*8));
    sha1_update(ctx,length,8);
    for (int i=0;i<5;++i) {
        out[i*4]=(unsigned char)(ctx->h[i]>>24); out[i*4+1]=(unsigned char)(ctx->h[i]>>16);
        out[i*4+2]=(unsigned char)(ctx->h[i]>>8); out[i*4+3]=(unsigned char)ctx->h[i];
    }
}
void runtime_symbol_to_nid(const char *symbol,char out[12]) {
    static const unsigned char salt[16]={0x51,0x8d,0x64,0xa6,0x35,0xde,0xd8,0xc1,
                                          0xe6,0xb0,0x39,0xb1,0xc3,0xe5,0x52,0x30};
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    Sha1 ctx={{0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u},0,{0},0};
    unsigned char digest[20];
    if (!symbol) { out[0]=0; return; }
    sha1_update(&ctx,symbol,strlen(symbol));
    sha1_update(&ctx,salt,sizeof(salt));
    sha1_digest(&ctx,digest);
    unsigned char reversed[8];
    for (int i=0;i<8;++i) reversed[i]=digest[7-i];
    size_t at=0;
    for (int i=0;i<8;i+=3) {
        const uint32_t chunk=((uint32_t)reversed[i]<<16)|(i+1<8 ? (uint32_t)reversed[i+1]<<8 : 0)|
                             (i+2<8 ? reversed[i+2] : 0);
        const int count=i+2<8 ? 4 : i+1<8 ? 3 : 2;
        for (int j=0;j<count;++j) out[at++]=alphabet[(chunk>>(18-j*6))&63];
    }
    out[at]=0;
}
/* Symbol name for a scoped NID, or NULL when this eboot/libc never imports it. */
const char *runtime_symbol(const char *nid) {
    for (size_t i=0;i<sizeof(import_names)/sizeof(*import_names);++i)
        if (!strcmp(nid,import_names[i].nid)) return import_names[i].symbol;
    return NULL;
}
/* Same symbol name means same contract whether imported from libkernel or
 * libScePosix, so newer modules resolve by name through runtime_symbol. */
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) {
    const char *symbol=runtime_symbol(nid);
    if (symbol) {
        for (size_t i=0;i<count;++i) if (!strcmp(symbol,table[i].name)) return (uintptr_t)table[i].function;
        return 0;
    }
    const char *hash=strchr(nid,'#');
    const size_t length=hash ? (size_t)(hash-nid) : strlen(nid);
    if (!length || length>=16) return 0;
    char wanted[16],generated[12];
    memcpy(wanted,nid,length); wanted[length]=0;
    for (size_t i=0;i<count;++i) {
        runtime_symbol_to_nid(table[i].name,generated);
        if (!strcmp(wanted,generated)) return (uintptr_t)table[i].function;
    }
    return 0;
}
const char *runtime_import_name(const char *name) {
    char canonical[128];
    const char *symbol=runtime_symbol(canonical_name(name,canonical));
    if (!symbol) symbol=runtime_np_toolkit2_symbol(name);
    return symbol ? symbol : "name not resolved; see src/import_names.inc";
}
