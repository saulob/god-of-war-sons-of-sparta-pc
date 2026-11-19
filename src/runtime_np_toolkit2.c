/* libSceNpToolkit2 for UnityNpToolkit2.prx, offline. The PSN requests the plugin can issue fail with the signed-out
 * error; the request/response objects keep their construction semantics, and the result accessors return empty data.
 * Bindings other than the Core entry points come from np_toolkit2_table.inc (scripts/np_toolkit2_analyze.py). */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define TK2_LIBRARY "libSceNpToolkit2"
#define TK2_ERROR_ALREADY_INITIALIZED ((int32_t)0x80552e03)
#define TK2_ERROR_NOT_INITIALIZED ((int32_t)0x80552e04)
#define TK2_ERROR_INCORRECT_ARGUMENTS ((int32_t)0x80552e05)
#define NP_ERROR_SIGNED_OUT ((int32_t)0x80550006)
/* Core::Request::InitParams: the plugin builds it in a 0x98-byte slot (0x90 bytes up to its last written byte);
 * the initial user id sits at +0x84. TermParams is an empty class (one byte). */
#define INIT_PARAMS_SIZE 0x98
#define INIT_PARAMS_USER 0x84
#define TERM_PARAMS_SIZE 1
#define EMPTY_RESULT_SIZE 0x1000

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static unsigned char empty_result[EMPTY_RESULT_SIZE] __attribute__((aligned(16)));

static int32_t offline_error(void) {
    pthread_mutex_lock(&lock);
    const int ready=initialized;
    pthread_mutex_unlock(&lock);
    return ready ? NP_ERROR_SIGNED_OUT : TK2_ERROR_NOT_INITIALIZED;
}

static ABI void *init_params_ctor(void *params) {
    if (params) memset(params,0,INIT_PARAMS_SIZE);
    return params;
}
static ABI void *params_dtor(void *params) { return params; }
static ABI void *term_params_ctor(void *params) {
    if (params) memset(params,0,TERM_PARAMS_SIZE);
    return params;
}
static ABI int32_t core_init(const unsigned char *params) {
    if (!params) return TK2_ERROR_INCORRECT_ARGUMENTS;
    int32_t user;
    memcpy(&user,params+INIT_PARAMS_USER,sizeof(user));
    if (!runtime_user_player(user)) return TK2_ERROR_INCORRECT_ARGUMENTS;
    pthread_mutex_lock(&lock);
    int32_t result=0;
    if (initialized) result=TK2_ERROR_ALREADY_INITIALIZED;
    else initialized=1;
    pthread_mutex_unlock(&lock);
    if (!result) printf("Runtime: NpToolkit2 initialized offline for user %d (no PSN session)\n",user);
    return result;
}
static ABI int32_t core_term(const void *params) {
    if (!params) return TK2_ERROR_INCORRECT_ARGUMENTS;
    pthread_mutex_lock(&lock);
    int32_t result=0;
    if (!initialized) result=TK2_ERROR_NOT_INITIALIZED;
    else initialized=0;
    pthread_mutex_unlock(&lock);
    if (!result) puts("Runtime: NpToolkit2 terminated");
    return result;
}

static ABI int32_t submit_offline(const void *request, void *response) { (void)request; (void)response; return offline_error(); }
static ABI int32_t zero_int(const void *self) { (void)self; return 0; }
static ABI const void *empty_data(const void *self) { (void)self; return empty_result; }
static ABI void request_base_ctor(unsigned char *self, uint32_t service, uint32_t function) {
    if (!self) return;
    memcpy(self,&service,sizeof(service));
    memcpy(self+4,&function,sizeof(function));
}
static ABI uint32_t request_base_service(const unsigned char *self) {
    uint32_t value;
    memcpy(&value,self,sizeof(value));
    return value;
}
static ABI uint32_t request_base_function(const unsigned char *self) {
    uint32_t value;
    memcpy(&value,self+4,sizeof(value));
    return value;
}

#define TK2_NAMED(nid,symbol)
#define TK2_STARTUP(nid)
#define TK2_UNBOUND(nid,symbol)
#define TK2_CTOR(index,nid,size,symbol) static ABI void *tk2_##index(void *self) { if (self) memset(self,0,size); return self; }
#define TK2_VOID(index,nid,symbol) static ABI void *tk2_##index(void *self) { return self; }
#define TK2_SUBMIT(index,nid,symbol)
#define TK2_GETP(index,nid,symbol)
#define TK2_GETI(index,nid,symbol)
#define TK2_RC(index,nid,symbol)
#define TK2_BUSY(index,nid,symbol)
#define TK2_FUNC(index,nid,symbol)
#define TK2_SVC(index,nid,symbol)
#define TK2_REQBASE(index,nid,symbol)
#include "np_toolkit2_table.inc"
#undef TK2_NAMED
#undef TK2_STARTUP
#undef TK2_UNBOUND
#undef TK2_CTOR
#undef TK2_VOID
#undef TK2_SUBMIT
#undef TK2_GETP
#undef TK2_GETI
#undef TK2_RC
#undef TK2_BUSY
#undef TK2_FUNC
#undef TK2_SVC
#undef TK2_REQBASE

typedef struct { const char *nid; void *function; const char *symbol; } Tk2Entry;
#define TK2_NAMED(nid,symbol) {nid,NULL,symbol},
#define TK2_STARTUP(nid)
#define TK2_UNBOUND(nid,symbol) {nid,NULL,symbol},
#define TK2_CTOR(index,nid,size,symbol) {nid,(void *)tk2_##index,symbol},
#define TK2_VOID(index,nid,symbol) {nid,(void *)tk2_##index,symbol},
#define TK2_SUBMIT(index,nid,symbol) {nid,(void *)submit_offline,symbol},
#define TK2_GETP(index,nid,symbol) {nid,(void *)empty_data,symbol},
#define TK2_GETI(index,nid,symbol) {nid,(void *)zero_int,symbol},
#define TK2_RC(index,nid,symbol) {nid,(void *)zero_int,symbol},
#define TK2_BUSY(index,nid,symbol) {nid,(void *)zero_int,symbol},
#define TK2_FUNC(index,nid,symbol) {nid,(void *)request_base_function,symbol},
#define TK2_SVC(index,nid,symbol) {nid,(void *)request_base_service,symbol},
#define TK2_REQBASE(index,nid,symbol) {nid,(void *)request_base_ctor,symbol},
static const Tk2Entry core_entries[]={
    {"vbh-MWtjMh4",(void *)init_params_ctor,"sce::Toolkit::NP::V2::Core::Request::InitParams::InitParams()"},
    {"-bAjEe-hdCg",(void *)params_dtor,"sce::Toolkit::NP::V2::Core::Request::InitParams::~InitParams()"},
    {"s0u30pOfi9Q",(void *)term_params_ctor,"sce::Toolkit::NP::V2::Core::Request::TermParams::TermParams()"},
    {"vycUuYWwilI",(void *)params_dtor,"sce::Toolkit::NP::V2::Core::Request::TermParams::~TermParams()"},
    {"LLdnqVnnNBQ",(void *)core_init,"sce::Toolkit::NP::V2::Core::init(const Request::InitParams&)"},
    {"IcuECtP3L0E",(void *)core_term,"sce::Toolkit::NP::V2::Core::term(const Request::TermParams&)"},
};
static const Tk2Entry table_entries[]={
#include "np_toolkit2_table.inc"
};

static const Tk2Entry *find_entry(const char *canonical) {
    const char *library=strchr(canonical,'#');
    if (!library) return NULL;
    const size_t nid_length=(size_t)(library-canonical);
    ++library;
    const char *end=strchr(library,'#');
    const size_t library_length=end ? (size_t)(end-library) : strlen(library);
    if (library_length!=sizeof(TK2_LIBRARY)-1 || strncmp(library,TK2_LIBRARY,library_length)) return NULL;
    for (size_t i=0;i<sizeof(core_entries)/sizeof(*core_entries);++i)
        if (strlen(core_entries[i].nid)==nid_length && !strncmp(core_entries[i].nid,canonical,nid_length)) return &core_entries[i];
    for (size_t i=0;i<sizeof(table_entries)/sizeof(*table_entries);++i)
        if (strlen(table_entries[i].nid)==nid_length && !strncmp(table_entries[i].nid,canonical,nid_length)) return &table_entries[i];
    return NULL;
}
uintptr_t runtime_np_toolkit2_resolve(const char *name) {
    const Tk2Entry *entry=find_entry(name);
    return entry ? (uintptr_t)entry->function : 0;
}
const char *runtime_np_toolkit2_symbol(const char *name) {
    const Tk2Entry *entry=find_entry(name);
    return entry && entry->symbol[0] ? entry->symbol : NULL;
}
void runtime_np_toolkit2_reset(void) {
    pthread_mutex_lock(&lock);
    initialized=0;
    pthread_mutex_unlock(&lock);
}
