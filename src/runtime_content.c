/* Offline AppContent provider for the configured base-game profile.
 * No package/DLC mounting, downloads, entitlement or license emulation: add-on requests are refused. */
#include "runtime.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define NOT_INITIALIZED ((int32_t)0x80d90001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_FOUND ((int32_t)0x80d90005)
#define NO_ENTITLEMENT ((int32_t)0x80d90007)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); runtime_exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent is not configured\n",stderr); runtime_exit(21); }
}
static int32_t provider_error(void) { return configured && refs ? 0 : NOT_INITIALIZED; }
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static const char *system_module_name(unsigned id) {
    switch (id) {
    case 0x8f: return "libSceMove";
    case 0x95: return "libSceIme";
    case 0x102: return "libSceNpToolkit2";
    case 0xa5: return "libSceAvPlayer";
    case 0xa9: return "libSceMouse";
    case 0xcf: return "libSceVideodec2";
    case 0xb4: return "AppContent";
    default: return NULL;
    }
}
static void note_module(const char *prefix, unsigned id, uintptr_t caller) {
    char event[64];
    const char *name=system_module_name(id);
    if (name) snprintf(event,sizeof(event),"%s 0x%x (%s)",prefix,id,name);
    else snprintf(event,sizeof(event),"%s 0x%x",prefix,id);
    runtime_history_note(event,"libSceSysmodule",caller);
}
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_exit(21); }
    const int first=module_refs[id]==0;
    ++module_refs[id];
    note_module(first ? "loaded module" : "reused module",id,(uintptr_t)__builtin_return_address(0));
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
/* Internal modules (libSceFios2 asks for its helpers): nothing to load on the host. */
static ABI int32_t module_load_internal_arg(uint32_t id, uint64_t argc, const void *argv, uint32_t flags, int32_t *result) {
    (void)argc; (void)argv;
    if (!id || flags) return INVALID_ID;
    if (result) *result=0;
    note_module("internal module",id,(uintptr_t)__builtin_return_address(0));
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) refs=module_refs[id];
    return 0;
}
static int32_t require_loaded(void) {
    return provider_error();
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    const int32_t state=require_loaded();
    if (state) return state;
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static int32_t require_initialized(void) {
    const int32_t state=require_loaded();
    return state ? state : initialized ? 0 : NOT_INITIALIZED;
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (service>7) return PARAMETER;
    if (!capacity || !list) {
        if (!hits) return PARAMETER;
        *hits=0;
        ++lists;
        return 0;
    }
    /* No add-ons are installed by this profile. List slots stay untouched. */
    if (hits) *hits=0;
    ++lists; return 0;
}
/* /download0 lives in the user directory: report the host's free space there. */
static ABI int32_t download_space_kb(const char *mount_point,uint64_t *kb) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (!mount_point || !kb) return PARAMETER;
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExA(runtime_file_user_dir(),&free_bytes,NULL,NULL)) free_bytes.QuadPart=0;
    *kb=free_bytes.QuadPart/1024;
#else
    struct statvfs space;
    *kb=statvfs(runtime_file_user_dir(),&space) ? 0 : (uint64_t)space.f_bavail*space.f_frsize/1024;
#endif
    return 0;
}
static ABI int32_t addon_mount(uint32_t service,const void *label,void *mount_point) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (service>7 || !label || !mount_point) return PARAMETER;
    return NOT_FOUND;
}
static ABI int32_t addon_enqueue_download(uint32_t service,const void *label) {
    const int32_t state=require_initialized();
    if (state) return state;
    (void)service;
    return label ? NO_ENTITLEMENT : PARAMETER;
}
static ABI int32_t addon_info(uint32_t service,const void *label,void *info) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (service>7 || !label || !info) return PARAMETER;
    return NO_ENTITLEMENT;
}
static void clear_directory(const char *path) {
    DIR *dir=opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[1024];
        if (snprintf(child,sizeof(child),"%s/%s",path,entry->d_name)>=(int)sizeof(child)) continue;
        struct stat info;
        if (!stat(child,&info) && S_ISDIR(info.st_mode)) {
            clear_directory(child);
            rmdir(child);
        } else remove(child);
    }
    closedir(dir);
}
static int temporary_data_path(char *out,size_t size) {
    const int length=snprintf(out,size,"%s/temp0",runtime_file_user_dir());
    return length<0 || length>=(int)size ? PARAMETER : 0;
}
static ABI int32_t temporary_data_format(void) {
    const int32_t state=require_initialized();
    if (state) return state;
    char path[1024];
    int32_t result=temporary_data_path(path,sizeof(path));
    if (result) return result;
    clear_directory(path);
    return 0;
}
static ABI int32_t temporary_data_mount2(uint32_t option,void *mount_point) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (!mount_point) return PARAMETER;
    char path[1024];
    int32_t result=temporary_data_path(path,sizeof(path));
    if (result) return result;
    if (option & 1) clear_directory(path);
    mkdir(path,0755);
    runtime_file_mount("/temp0",path);
    memset(mount_point,0,16);
    memcpy(mount_point,"/temp0",7);
    return 0;
}
static ABI int32_t temporary_data_unmount(const void *mount_point) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (!mount_point) return PARAMETER;
    char mount[17];
    memcpy(mount,mount_point,16);
    mount[16]=0;
    runtime_file_unmount(mount);
    return 0;
}
static ABI int32_t temporary_space_kb(const void *mount_point,uint64_t *kb) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (!mount_point || !kb) return PARAMETER;
    char path[1024];
    int32_t result=temporary_data_path(path,sizeof(path));
    if (result) return result;
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExA(path,&free_bytes,NULL,NULL)) free_bytes.QuadPart=0;
    *kb=free_bytes.QuadPart/1024;
#else
    struct statvfs space;
    *kb=statvfs(path,&space) ? 0 : (uint64_t)space.f_bavail*space.f_frsize/1024;
#endif
    return 0;
}
static ABI int32_t addon_unmount(const void *mount_point) {
    const int32_t state=require_initialized();
    if (state) return state;
    if (!mount_point) return PARAMETER;
    char mount[17];
    memcpy(mount,mount_point,16);
    mount[16]=0;
    runtime_file_unmount(mount);
    return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal_arg;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"Gl6w5i0JokY#libSceAppContent")) return (uintptr_t)download_space_kb;
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"7gxh+5QubhY#libSceAppContent")) return (uintptr_t)addon_enqueue_download;
    if (!strcmp(name,"m47juOmH0VE#libSceAppContent")) return (uintptr_t)addon_info;
    if (!strcmp(name,"a5N7lAG0y2Q#libSceAppContent")) return (uintptr_t)temporary_data_format;
    if (!strcmp(name,"SaKib2Ug0yI#libSceAppContent")) return (uintptr_t)temporary_space_kb;
    if (!strcmp(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addon_unmount;
    if (!strcmp(name,"buYbeLOGWmA#libSceAppContent")) return (uintptr_t)temporary_data_mount2;
    if (!strcmp(name,"bcolXMmp6qQ#libSceAppContent")) return (uintptr_t)temporary_data_unmount;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
