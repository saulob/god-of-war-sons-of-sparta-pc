#include "runtime.h"
#include "runtime_log.h"
#include "startup_phase.h"
#include "startup_profile.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define SELF_MAGIC 0x1D3D154Fu
#define MAX_MODULES 32
#define MAX_PHDRS 64
#define MAX_IDS 64
#define MAX_TRAPS 8192
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_SCE_DYNLIBDATA 0x61000000u
#define PT_SCE_PROCPARAM 0x61000001u
#define PT_SCE_RELRO 0x61000010u
#define DT_INIT 12
#define DT_FINI 13
#define DT_SCE_RELA 0x61000029ull
#define DT_SCE_RELASZ 0x6100002dull
#define DT_SCE_JMPREL 0x6100002full
#define DT_SCE_PLTRELSZ 0x61000031ull
#define DT_SCE_STRTAB 0x61000035ull
#define DT_SCE_STRSZ 0x61000037ull
#define DT_SCE_SYMTAB 0x61000039ull
#define DT_SCE_SYMTABSZ 0x6100003full
#define ERRNO(n) ((int32_t)(UINT32_C(0x80020000)|(n)))

typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint8_t *data; uint64_t size, image_size, entry; Phdr ph[MAX_PHDRS]; int phnum; } ElfFile;
typedef struct { char nid[16], library[64], module[64]; } Identity;
typedef struct { char *name; Identity identity; uintptr_t address; } GuestExport;
typedef struct PendingReloc {
    uint64_t target;
    int64_t addend;
    uint32_t type;
    Identity identity;
    int is_data, weak;
    struct PendingReloc *next;
} PendingReloc;
typedef struct {
    int used, linked, loading, exports_loaded, exports_failed, started;
    int32_t handle, start_result;
    uint64_t base, size, image_offset, tls_module, start_vaddr, fini_vaddr;
    uint64_t relro_vaddr, relro_size;
    uint32_t relro_flags;
    char path[512];
    uint64_t eh_frame_hdr, eh_frame, eh_frame_size, eh_frame_hdr_size, seg0_vaddr, seg0_size;
    int eh_searched, headers_known;
    GuestExport *exports;
    size_t export_count, export_capacity;
    PendingReloc *pending;
} LoadedModule;
typedef struct { char token[16], name[64]; } NamedId;
typedef struct { char *name; int used, weak; } ImportTrap;

static LoadedModule modules[MAX_MODULES];
static int next_handle=1;
static pthread_mutex_t module_lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static ImportTrap traps[MAX_TRAPS];
static unsigned char *trap_code;
static uint32_t trap_count;
static _Thread_local char module_operation[640];
static _Thread_local char operation_stack[8][640];
static _Thread_local unsigned operation_depth;

const char *runtime_module_operation(void) { return module_operation[0] ? module_operation : "none"; }
static void operation_set(const char *kind,const char *path) {
    if (path) snprintf(module_operation,sizeof(module_operation),"%s %s",kind,path);
    else snprintf(module_operation,sizeof(module_operation),"%s",kind);
}
static void operation_push(const char *kind,const char *path) {
    if (operation_depth<sizeof(operation_stack)/sizeof(operation_stack[0]))
        snprintf(operation_stack[operation_depth],sizeof(operation_stack[0]),"%s",module_operation);
    ++operation_depth;
    operation_set(kind,path);
}
static void operation_pop(void) {
    if (!operation_depth) { module_operation[0]=0; return; }
    --operation_depth;
    if (operation_depth<sizeof(operation_stack)/sizeof(operation_stack[0]))
        snprintf(module_operation,sizeof(module_operation),"%s",operation_stack[operation_depth]);
    else module_operation[0]=0;
}
static uint16_t rd16(const void *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t rd64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int range_ok(uint64_t offset,uint64_t size,uint64_t total) { return offset<=total && size<=total-offset; }
static void normalize_path(const char *path,char out[512]) {
    while (!strncmp(path,"/app0/",6) || !strncmp(path,"/hostapp/",9))
        path+=path[1]=='a' ? 6 : 9;
    snprintf(out,512,"%s",path);
    for (char *p=out;*p;++p) if (*p=='\\') *p='/';
    while (out[0]=='/' && out[1]) memmove(out,out+1,strlen(out));
}
/* The eboot requests Il2CppUserAssemblies with different capitalization from the linked file. */
static int path_equal(const char *a,const char *b) {
    for (;*a && *b;++a,++b) {
        unsigned char ca=(unsigned char)*a,cb=(unsigned char)*b;
        if (ca>='A' && ca<='Z') ca=(unsigned char)(ca+'a'-'A');
        if (cb>='A' && cb<='Z') cb=(unsigned char)(cb+'a'-'A');
        if (ca!=cb) return 0;
    }
    return *a==*b;
}
static const Phdr *find_ph(const ElfFile *elf,uint32_t type) {
    for (int i=0;i<elf->phnum;++i) if (elf->ph[i].type==type) return &elf->ph[i];
    return NULL;
}
static int parse_self(const uint8_t *raw,uint64_t raw_size,ElfFile *out) {
    if (raw_size<32 || rd32(raw)!=SELF_MAGIC) return 0;
    const uint16_t count=rd16(raw+24);
    const uint64_t base=32+(uint64_t)count*32;
    if (!range_ok(base,64,raw_size) || memcmp(raw+base,"\x7f""ELF\x02\x01\x01",7) || rd16(raw+base+18)!=62) return 0;
    memset(out,0,sizeof(*out));
    out->entry=rd64(raw+base+24);
    const uint64_t phoff=rd64(raw+base+32);
    const uint16_t nph=rd16(raw+base+56);
    if (!nph || nph>MAX_PHDRS || rd16(raw+base+54)!=56) return 0;
    uint64_t end=phoff+(uint64_t)nph*56;
    if (end>512ull*1024*1024 || !range_ok(base+phoff,(uint64_t)nph*56,raw_size)) return 0;
    for (int i=0;i<nph;++i) {
        const uint8_t *p=raw+base+phoff+(uint64_t)i*56;
        Phdr *h=&out->ph[i];
        h->type=rd32(p); h->flags=rd32(p+4); h->offset=rd64(p+8); h->vaddr=rd64(p+16);
        h->paddr=rd64(p+24); h->filesz=rd64(p+32); h->memsz=rd64(p+40); h->align=rd64(p+48);
        if (h->filesz>h->memsz && (h->type==PT_LOAD || h->type==PT_SCE_RELRO)) return 0;
        if (!range_ok(h->offset,h->filesz,512ull*1024*1024)) return 0;
        const uint64_t file_end=h->offset+h->filesz;
        if (file_end>end) end=file_end;
        if ((h->type==PT_LOAD || h->type==PT_SCE_RELRO) && h->vaddr+h->memsz>out->image_size)
            out->image_size=h->vaddr+h->memsz;
    }
    if (!out->image_size || out->image_size>512ull*1024*1024) return 0;
    uint8_t *elf=calloc(1,(size_t)end);
    if (!elf) return 0;
    memcpy(elf,raw+base,(size_t)(phoff+(uint64_t)nph*56));
    for (int i=0;i<count;++i) {
        const uint8_t *s=raw+32+(uint64_t)i*32;
        if (!range_ok((uint64_t)(s-raw),32,raw_size)) { free(elf); return 0; }
        const uint64_t flags=rd64(s),off=rd64(s+8),size=rd64(s+16),mem=rd64(s+24);
        if (!(flags&0x800) || !size) continue;
        if (flags&10) { free(elf); return 0; }
        const unsigned index=(unsigned)((flags>>20)&4095);
        if (index>=nph || !range_ok(off,size,raw_size)) { free(elf); return 0; }
        const Phdr *p=&out->ph[index];
        if (size!=p->filesz || size!=mem) { free(elf); return 0; }
        memcpy(elf+p->offset,raw+off,(size_t)size);
    }
    out->data=elf;
    out->size=end;
    out->phnum=nph;
    return 1;
}
static uintptr_t read_file(const char *path,uint64_t *size) {
    int64_t fd=runtime_file_open(path,0,0);
    if (fd<0) return 0;
    /* Sized up front (one spare byte sees the end): small realloc steps copy a 136 MB module quadratically. */
    const int64_t length=runtime_file_lseek(fd,0,2);
    size_t capacity=length>0 && runtime_file_lseek(fd,0,0)==0 && (uint64_t)length<SIZE_MAX ? (size_t)length+1 : 0;
    uint8_t *data=capacity ? malloc(capacity) : NULL;
    if (capacity && !data) { runtime_file_close(fd); return 0; }
    size_t total=0;
    for (;;) {
        if (total==capacity) {
            const size_t grown_capacity=capacity ? capacity*2 : (size_t)1<<20;
            uint8_t *grown=grown_capacity>capacity ? realloc(data,grown_capacity) : NULL;
            if (!grown) { runtime_file_close(fd); free(data); return 0; }
            data=grown;
            capacity=grown_capacity;
        }
        const int64_t n=runtime_file_read(fd,data+total,capacity-total);
        if (n<0) { runtime_file_close(fd); free(data); return 0; }
        if (!n) break;
        total+=(size_t)n;
    }
    runtime_file_close(fd);
    *size=total;
    return (uintptr_t)data;
}
static int load_elf(const char *path,ElfFile *elf) {
    uint64_t raw_size=0;
    const uintptr_t raw=read_file(path,&raw_size);
    if (!raw) return 0;
    const int ok=parse_self((const uint8_t *)raw,raw_size,elf);
    free((void *)raw);
    return ok;
}
static int dynamic_entries(const ElfFile *elf,uint64_t *offset,uint64_t *size) {
    const Phdr *p=find_ph(elf,PT_DYNAMIC);
    if (!p || !range_ok(p->offset,p->filesz,elf->size)) return 0;
    *offset=p->offset; *size=p->filesz;
    return 1;
}
static uint64_t dynamic_tag(const ElfFile *elf,uint64_t wanted,uint64_t fallback) {
    uint64_t off,size;
    if (!dynamic_entries(elf,&off,&size)) return fallback;
    for (uint64_t at=off;at+16<=off+size;at+=16) {
        const uint64_t tag=rd64(elf->data+at);
        if (!tag) break;
        if (tag==wanted) return rd64(elf->data+at+8);
    }
    return fallback;
}
static int dynlib_tables(const ElfFile *elf,uint64_t *strings,uint64_t *string_size,uint64_t *symbols,uint64_t *symbol_size) {
    const Phdr *data=find_ph(elf,PT_SCE_DYNLIBDATA);
    if (!data) return 0;
    const uint64_t so=dynamic_tag(elf,DT_SCE_STRTAB,0),ss=dynamic_tag(elf,DT_SCE_STRSZ,0);
    const uint64_t yo=dynamic_tag(elf,DT_SCE_SYMTAB,0),ys=dynamic_tag(elf,DT_SCE_SYMTABSZ,0);
    if (!range_ok(so,ss,data->filesz) || !range_ok(yo,ys,data->filesz) || ys%24) return 0;
    const uint64_t str=data->offset+so,sym=data->offset+yo;
    if (!range_ok(str,ss,elf->size) || !range_ok(sym,ys,elf->size)) return 0;
    *strings=str; *string_size=ss; *symbols=sym; *symbol_size=ys;
    return 1;
}
static const char *table_string(const ElfFile *elf,uint64_t strings,uint64_t size,uint32_t offset) {
    if (offset>=size) return NULL;
    const char *s=(const char *)elf->data+strings+offset;
    return memchr(s,0,size-offset) ? s : NULL;
}
static void encode_id(unsigned value,char out[16]) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    char reversed[16];
    int count=0;
    do { reversed[count++]=alphabet[value&63]; value>>=6; } while (value && count<15);
    for (int i=0;i<count;++i) out[i]=reversed[count-1-i];
    out[count]=0;
}
static int named_ids(const ElfFile *elf,int library,NamedId *ids) {
    const uint64_t first=library ? 0x61000013ull : 0x6100000dull;
    const uint64_t second=library ? 0x61000015ull : 0x6100000full;
    uint64_t strings,string_size,symbols,symbol_size;
    if (!dynlib_tables(elf,&strings,&string_size,&symbols,&symbol_size)) return 0;
    uint64_t off,size;
    dynamic_entries(elf,&off,&size);
    int count=0;
    for (uint64_t at=off;at+16<=off+size;at+=16) {
        const uint64_t tag=rd64(elf->data+at),value=rd64(elf->data+at+8);
        if (!tag) break;
        if (tag!=first && tag!=second) continue;
        if (count>=MAX_IDS) return 0;
        const char *name=table_string(elf,strings,string_size,(uint32_t)value);
        if (!name || strlen(name)>=sizeof(ids[count].name)) return 0;
        encode_id((unsigned)(value>>48),ids[count].token);
        snprintf(ids[count].name,sizeof(ids[count].name),"%s",name);
        ++count;
    }
    return count;
}
static int decode_identity(const ElfFile *elf,const char *name,Identity *out) {
    memset(out,0,sizeof(*out));
    const char *first=strchr(name,'#');
    if (!first || first-name>11) return 0;
    const char *second=strchr(first+1,'#');
    if (!second || strchr(second+1,'#')) return 0;
    memcpy(out->nid,name,(size_t)(first-name));
    out->nid[first-name]=0;
    const char *lib_token=first+1,*mod_token=second+1;
    if (!*lib_token || !*mod_token) return 0;
    NamedId libraries[MAX_IDS],module_ids[MAX_IDS];
    char wanted[16];
    const size_t lib_length=(size_t)(second-lib_token);
    if (lib_length>=sizeof(wanted) || strlen(mod_token)>=sizeof(wanted)) return 0;
    memcpy(wanted,lib_token,lib_length); wanted[lib_length]=0;
    const int nl=named_ids(elf,1,libraries),nm=named_ids(elf,0,module_ids);
    int found=0;
    for (int i=0;i<nl;++i) if (!strcmp(libraries[i].token,wanted)) {
        snprintf(out->library,sizeof(out->library),"%s",libraries[i].name); found=1; break;
    }
    if (!found) return 0;
    found=0;
    for (int i=0;i<nm;++i) if (!strcmp(module_ids[i].token,mod_token)) {
        snprintf(out->module,sizeof(out->module),"%s",module_ids[i].name); found=1; break;
    }
    return found;
}
static LoadedModule *module_by_handle(int32_t handle) {
    for (int i=0;i<MAX_MODULES;++i) if (modules[i].used && modules[i].handle==handle) return &modules[i];
    return NULL;
}
static LoadedModule *module_by_path(const char *path) {
    char normalized[512];
    normalize_path(path,normalized);
    for (int i=0;i<MAX_MODULES;++i) if (modules[i].used && path_equal(modules[i].path,normalized)) return &modules[i];
    return NULL;
}
static int identity_same(const Identity *a,const Identity *b,int include_module) {
    return !strcmp(a->nid,b->nid) && !strcmp(a->library,b->library) && (!include_module || !strcmp(a->module,b->module));
}
static int export_add(LoadedModule *m,const char *name,const Identity *identity,int has_identity,uintptr_t address) {
    if (m->export_count==m->export_capacity) {
        const size_t capacity=m->export_capacity ? m->export_capacity*2 : 64;
        GuestExport *grown=realloc(m->exports,capacity*sizeof(*grown));
        if (!grown) return 0;
        m->exports=grown;
        m->export_capacity=capacity;
    }
    GuestExport *e=&m->exports[m->export_count];
    e->name=strdup(name);
    if (!e->name) return 0;
    e->identity=has_identity ? *identity : (Identity){0};
    e->address=address;
    ++m->export_count;
    return 1;
}
static int load_exports(LoadedModule *m,const ElfFile *elf) {
    uint64_t strings,string_size,symbols,symbol_size;
    if (!dynlib_tables(elf,&strings,&string_size,&symbols,&symbol_size)) return 0;
    for (uint64_t off=0;off<symbol_size;off+=24) {
        const uint8_t *s=elf->data+symbols+off;
        const uint32_t name_offset=rd32(s);
        const uint8_t info=s[4],type=info&15,binding=info>>4;
        const uint16_t section=rd16(s+6);
        const uint64_t value=rd64(s+8);
        if (!section || (binding!=1 && binding!=2) || (type!=1 && type!=2)) continue;
        if (value>m->size && section!=0xfff1) return 0;
        const char *name=table_string(elf,strings,string_size,name_offset);
        if (!name) return 0;
        Identity identity;
        const int has_identity=decode_identity(elf,name,&identity);
        const uintptr_t address=section==0xfff1 ? (uintptr_t)value : (uintptr_t)(m->base+value);
        if (!export_add(m,name,&identity,has_identity,address)) return 0;
    }
    return 1;
}
static int ensure_exports(LoadedModule *m) {
    if (m->exports_loaded) return 1;
    ElfFile elf;
    if (m->exports_failed) return 0;
    elf.data=NULL;
    if (!load_elf(m->path,&elf) || !load_exports(m,&elf)) {
        free(elf.data);
        for (size_t i=0;i<m->export_count;++i) free(m->exports[i].name);
        m->export_count=0;
        m->exports_failed=1;
        fprintf(stderr,"Runtime: cannot parse exports of %s\n",m->path);
        return 0;
    }
    free(elf.data);
    m->exports_loaded=1;
    return 1;
}
static GuestExport *find_guest_export(const Identity *identity) {
    for (int i=0;i<MAX_MODULES;++i) {
        LoadedModule *m=&modules[i];
        if (!m->used || m->loading || !ensure_exports(m)) continue;
        for (size_t e=0;e<m->export_count;++e)
            if (m->exports[e].identity.nid[0] && identity_same(&m->exports[e].identity,identity,1)) return &m->exports[e];
    }
    return NULL;
}
static GuestExport *find_module_export(LoadedModule *m,const char *name,const char *nid) {
    if (!ensure_exports(m)) return NULL;
    GuestExport *fallback=NULL;
    for (size_t i=0;i<m->export_count;++i) {
        GuestExport *e=&m->exports[i];
        if (!strcmp(e->name,name)) return e;
        if (!e->identity.nid[0]) continue;
        const char *hash=strchr(name,'#');
        if (!hash && (!strcmp(e->identity.nid,name) || (nid && !strcmp(e->identity.nid,nid)))) fallback=e;
        else if (hash) {
            Identity requested;
            snprintf(requested.nid,sizeof(requested.nid),"%.*s",(int)(hash-name),name);
            if (!strcmp(e->identity.nid,requested.nid) && !strcmp(e->identity.library,hash+1)) fallback=e;
        }
    }
    return fallback;
}
static ABI __attribute__((noreturn)) void unresolved_import(uint32_t id) {
    const char *name=id<trap_count && traps[id].used ? traps[id].name : "unknown";
    char caller[320];
    runtime_describe_guest_address((uintptr_t)__builtin_return_address(0),caller,sizeof(caller));
    fprintf(stderr,"STOP: unresolved dynamic import %s (%s) called from %s; phase=%s, module operation=%s\n",
            name,runtime_import_name(name),caller,runtime_module_operation(),module_operation);
    runtime_log_flush();
    runtime_exit(20);
}
static uintptr_t new_trap(const char *name) {
    if (trap_count>=MAX_TRAPS) return (uintptr_t)unresolved_import;
    const uint32_t id=trap_count++;
    if (!trap_code) {
#ifdef _WIN32
        trap_code=VirtualAlloc(NULL,(SIZE_T)MAX_TRAPS*32,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
        trap_code=mmap(NULL,(size_t)MAX_TRAPS*32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (trap_code==MAP_FAILED) trap_code=NULL;
#endif
        if (!trap_code) { --trap_count; return (uintptr_t)unresolved_import; }
    }
#ifdef _WIN32
    DWORD old;
    VirtualProtect(trap_code,(SIZE_T)MAX_TRAPS*32,PAGE_EXECUTE_READWRITE,&old);
#else
    mprotect(trap_code,(size_t)MAX_TRAPS*32,PROT_READ|PROT_WRITE|PROT_EXEC);
#endif
    traps[id].name=strdup(name);
    traps[id].used=1;
    unsigned char *at=trap_code+(size_t)id*32;
    const uintptr_t handler=(uintptr_t)unresolved_import;
    at[0]=0xbf; memcpy(at+1,&id,4);
    at[5]=0x48; at[6]=0xb8; memcpy(at+7,&handler,8);
    at[15]=0xff; at[16]=0xe0;
    return (uintptr_t)at;
}
static uint64_t symbol_value(LoadedModule *m,const uint8_t *symbol,int *weak,int *found) {
    const uint16_t section=rd16(symbol+6);
    const uint64_t value=rd64(symbol+8);
    *weak=(symbol[4]>>4)==2;
    *found=section!=0;
    if (section==0xfff1) return value;
    return section ? m->base+value : 0;
}
static void pending_add(LoadedModule *m,uint64_t target,const Identity *identity,uint32_t type,int64_t addend,
                        int is_data,int weak) {
    PendingReloc *p=calloc(1,sizeof(*p));
    if (!p) { runtime_exit(1); }
    p->target=target; p->addend=addend; p->type=type; p->identity=*identity; p->is_data=is_data; p->weak=weak;
    p->next=m->pending;
    m->pending=p;
    char canonical[160];
    snprintf(canonical,sizeof(canonical),"%s#%s#%s",identity->nid,identity->library,identity->module);
    const uint64_t value=weak ? 0 : new_trap(canonical);
    memcpy((void *)(uintptr_t)(m->base+target),&value,8);
}
static int resolve_pending(const Identity *identity,int is_data,int weak,uint64_t *value) {
    char canonical[160];
    snprintf(canonical,sizeof(canonical),"%s#%s",identity->nid,identity->library);
    uintptr_t resolved=runtime_resolve(canonical,is_data);
    if (resolved) { *value=resolved; return 1; }
    GuestExport *guest=find_guest_export(identity);
    if (guest) { *value=guest->address; return 1; }
    (void)weak;
    return 0;
}
static int apply_table(LoadedModule *m,const ElfFile *elf,uint64_t relative,uint64_t bytes) {
    const Phdr *data=find_ph(elf,PT_SCE_DYNLIBDATA);
    if (!data || !range_ok(relative,bytes,data->filesz)) return 0;
    const uint64_t table=data->offset+relative;
    uint64_t strings,string_size,symbols,symbol_size;
    if (!range_ok(table,bytes,elf->size) || bytes%24 || !dynlib_tables(elf,&strings,&string_size,&symbols,&symbol_size)) return 0;
    for (uint64_t at=0;at<bytes;at+=24) {
        const uint8_t *r=elf->data+table+at;
        const uint64_t target=rd64(r),info=rd64(r+8);
        int64_t addend=(int64_t)rd64(r+16);
        const uint32_t type=(uint32_t)info,symbol_index=(uint32_t)(info>>32);
        uint64_t value=0;
        if (target>m->size || m->size-target<8) return 0;
        if (type==8) value=m->base+(uint64_t)addend;
        else if (type==16) {
            if (!m->tls_module) return 0;
            value=m->tls_module;
        } else if (type==1 || type==6 || type==7) {
            if ((uint64_t)symbol_index*24>=symbol_size) return 0;
            const uint8_t *symbol=elf->data+symbols+(uint64_t)symbol_index*24;
            if (type==6 || type==7) addend=0;
            int weak=0,found=0;
            value=symbol_value(m,symbol,&weak,&found);
            if (!found) {
                const char *name=table_string(elf,strings,string_size,rd32(symbol));
                Identity identity;
                if (!name || !decode_identity(elf,name,&identity)) {
                    if (weak) value=0;
                    else return 0;
                } else {
                    const int is_data=(symbol[4]&15)==1;
                    if (!resolve_pending(&identity,is_data,weak,&value)) {
                        pending_add(m,target,&identity,type,addend,is_data,weak);
                        continue;
                    }
                }
            }
            value+=(uint64_t)addend;
        } else return 0;
        memcpy((void *)(uintptr_t)(m->base+target),&value,8);
    }
    return 1;
}
static size_t host_page_size(void);
static void protect_relro(LoadedModule *m,int writable) {
    if (!m->relro_size) return;
    const size_t page=host_page_size();
    const uint64_t start=m->relro_vaddr&~(uint64_t)(page-1);
    const uint64_t end=(m->relro_vaddr+m->relro_size+page-1)&~(uint64_t)(page-1);
#ifdef _WIN32
    DWORD old;
    VirtualProtect((void *)(uintptr_t)(m->base+start),(SIZE_T)(end-start),writable ? PAGE_READWRITE : PAGE_READONLY,&old);
#else
    mprotect((void *)(uintptr_t)(m->base+start),(size_t)(end-start),(writable ? PROT_READ|PROT_WRITE : PROT_READ));
#endif
}
static void repair_pending(void) {
    for (int i=0;i<MAX_MODULES;++i) {
        LoadedModule *m=&modules[i];
        if (!m->used || !m->pending) continue;
        protect_relro(m,1);
        PendingReloc **link=&m->pending;
        while (*link) {
            PendingReloc *p=*link;
            uint64_t value;
            if (resolve_pending(&p->identity,p->is_data,p->weak,&value)) {
                value+=(uint64_t)p->addend;
                memcpy((void *)(uintptr_t)(m->base+p->target),&value,8);
                *link=p->next;
                free(p);
            } else link=&p->next;
        }
        protect_relro(m,0);
    }
}
static uint64_t patch_import_stubs(LoadedModule *m) {
    uint64_t patched=0;
    const uint64_t limit=m->size<0x20000 ? m->size : 0x20000;
    uint64_t previous_offset=0,previous_got=0;
    int have_previous=0;
    for (uint64_t off=8;off+32<=limit;off+=16) {
        unsigned char *at=(unsigned char *)(uintptr_t)(m->base+off);
        if (at[0]==0xff && at[1]==0x25) {
            int32_t disp;
            memcpy(&disp,at+2,4);
            previous_offset=off;
            previous_got=off+6+(int64_t)disp;
            have_previous=1;
            continue;
        }
        if (at[0]!=0xe9 || at[5]!=0x07 || !have_previous || off!=previous_offset+16) continue;
        unsigned char *next=at+16;
        if (next[0]!=0xff || next[1]!=0x25) continue;
        int32_t next_disp;
        memcpy(&next_disp,next+2,4);
        const uint64_t slot=previous_got+8;
        if (off+16+6+(int64_t)next_disp!=slot+8 || slot+8>m->size) continue;
        const int32_t disp=(int32_t)(slot-(off+6));
        at[0]=0xff; at[1]=0x25; memcpy(at+2,&disp,4);
        ++patched;
    }
    return patched;
}
static uint64_t patch_tcb_loads(LoadedModule *m,const ElfFile *elf) {
    uint64_t patched=0;
    static const unsigned char suffix[]={0x48,0x8b,0x04,0x25,0,0,0,0};
    for (int i=0;i<elf->phnum;++i) {
        const Phdr *p=&elf->ph[i];
        if (p->type!=PT_LOAD || !(p->flags&1)) continue;
        for (uint64_t at=p->vaddr;at+9<=p->vaddr+p->memsz;++at) {
            unsigned char *code=(unsigned char *)(uintptr_t)(m->base+at);
            if ((code[0]!=0x64 && code[0]!=0x65) || memcmp(code+1,suffix,sizeof(suffix))) continue;
#ifdef _WIN32
            const uint32_t displacement=runtime_thread_tcb_offset();
            code[0]=0x65;
            memcpy(code+5,&displacement,4);
#else
            code[0]=0x65;
#endif
            ++patched;
            at+=8;
        }
    }
    return patched;
}
static size_t host_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}
static void *module_alloc(uint64_t size) {
    const size_t page=host_page_size();
    const size_t rounded=(size_t)((size+page-1)&~(uint64_t)(page-1));
#ifdef _WIN32
    return VirtualAlloc(NULL,rounded,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *p=mmap(NULL,rounded,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED ? NULL : p;
#endif
}
static void abandon_module(LoadedModule *m) {
    if (!m->used || m->linked) return;
    if (m->tls_module) runtime_remove_module_tls(m->tls_module);
    while (m->pending) {
        PendingReloc *next=m->pending->next;
        free(m->pending);
        m->pending=next;
    }
    for (size_t i=0;i<m->export_count;++i) free(m->exports[i].name);
    free(m->exports);
    if (m->base) {
#ifdef _WIN32
        VirtualFree((void *)(uintptr_t)m->base,0,MEM_RELEASE);
#else
        munmap((void *)(uintptr_t)m->base,(size_t)((m->size+host_page_size()-1)&~(uint64_t)(host_page_size()-1)));
#endif
    }
    memset(m,0,sizeof(*m));
}
static int protect_region(uint64_t base,const Phdr *p) {
    const size_t page=host_page_size();
    const uint64_t start=p->vaddr&~(uint64_t)(page-1);
    const uint64_t end=(p->vaddr+p->memsz+page-1)&~(uint64_t)(page-1);
    if (end==start) return 1;
#ifdef _WIN32
    DWORD old,mode=PAGE_NOACCESS;
    if (p->flags&1) mode=(p->flags&2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (p->flags&2) mode=PAGE_READWRITE;
    else if (p->flags&4) mode=PAGE_READONLY;
    if (!VirtualProtect((void *)(uintptr_t)(base+start),(SIZE_T)(end-start),mode,&old)) return 0;
    FlushInstructionCache(GetCurrentProcess(),(void *)(uintptr_t)(base+start),(SIZE_T)(end-start));
#else
    const int prot=((p->flags&4) ? PROT_READ : 0)|((p->flags&2) ? PROT_WRITE : 0)|((p->flags&1) ? PROT_EXEC : 0);
    if (mprotect((void *)(uintptr_t)(base+start),(size_t)(end-start),prot)) return 0;
#endif
    return 1;
}
static int finalize_mapping(LoadedModule *m,const ElfFile *elf) {
    for (int i=0;i<elf->phnum;++i)
        if (elf->ph[i].type==PT_LOAD && !protect_region(m->base,&elf->ph[i])) return 0;
    for (int i=0;i<elf->phnum;++i)
        if (elf->ph[i].type==PT_SCE_RELRO && !protect_region(m->base,&elf->ph[i])) return 0;
#ifdef _WIN32
    if (trap_code) {
        DWORD old;
        VirtualProtect(trap_code,(SIZE_T)MAX_TRAPS*32,PAGE_EXECUTE_READ,&old);
    }
#else
    if (trap_code) mprotect(trap_code,(size_t)MAX_TRAPS*32,PROT_READ|PROT_EXEC);
#endif
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(),(void *)(uintptr_t)m->base,(SIZE_T)m->size);
#endif
    return 1;
}
static int32_t register_linked(const char *path,const void *base,uint64_t size,uint64_t image_offset,int is_main) {
    pthread_mutex_lock(&module_lock);
    LoadedModule *existing=module_by_path(path);
    if (existing) { pthread_mutex_unlock(&module_lock); return existing->handle; }
    if (is_main && module_by_handle(0)) { pthread_mutex_unlock(&module_lock); return ERRNO(17); }
    LoadedModule *m=NULL;
    for (int i=0;i<MAX_MODULES;++i) if (!modules[i].used) { m=&modules[i]; break; }
    if (!m) { pthread_mutex_unlock(&module_lock); return ERRNO(12); }
    memset(m,0,sizeof(*m));
    m->used=1; m->linked=1; m->handle=is_main ? 0 : next_handle++;
    m->base=(uintptr_t)base; m->size=size; m->image_offset=image_offset;
    if (is_main) m->started=1;
    normalize_path(path,m->path);
    pthread_mutex_unlock(&module_lock);
    return m->handle;
}
int32_t runtime_module_register_linked(const char *path,const void *base,uint64_t size,uint64_t image_offset) {
    return register_linked(path,base,size,image_offset,0);
}
/* The main executable is module handle 0: Unity resolves its own exports with sceKernelDlsym(0, ...). */
int32_t runtime_module_register_main(const char *path,const void *base,uint64_t size) {
    return register_linked(path,base,size,0,1);
}
void runtime_module_linked_started(uint64_t image_offset,int32_t result) {
    pthread_mutex_lock(&module_lock);
    for (int i=0;i<MAX_MODULES;++i) if (modules[i].used && modules[i].linked && modules[i].image_offset==image_offset) {
        modules[i].started=1;
        modules[i].start_result=result;
    }
    pthread_mutex_unlock(&module_lock);
}
static void record_unwind_headers(LoadedModule *m,const Phdr *ph,int count);
/* Read without the module lock while a thread is suspended (the startup profiler); ranges are never removed. */
static uintptr_t code_ranges[MAX_MODULES][2];
static int code_range_count;
static void publish_code_range(uintptr_t base,uint64_t size) {
    const int n=__atomic_load_n(&code_range_count,__ATOMIC_RELAXED);
    if (n==MAX_MODULES) return;
    code_ranges[n][0]=base; code_ranges[n][1]=base+(uintptr_t)size;
    __atomic_store_n(&code_range_count,n+1,__ATOMIC_RELEASE);
}
int runtime_module_in_loaded_code(uintptr_t address) {
    const int n=__atomic_load_n(&code_range_count,__ATOMIC_ACQUIRE);
    for (int i=0;i<n;++i) if (address>=code_ranges[i][0] && address<code_ranges[i][1]) return 1;
    return 0;
}
static const char *file_name(const char *path) {
    const char *slash=strrchr(path,'/');
    return slash ? slash+1 : path;
}
static void note_first_dlsym(const char *name) {
    static const struct { const char *symbol, *milestone; } marks[]={
        {"FMOD_Studio_System_Initialize","FMOD Studio initialization"},
        {"FMOD_Studio_System_FlushSampleLoading","FMOD Studio initialized, sample loading flush"},
        {"FMOD_Studio_System_LoadBankFile","first FMOD bank load"},
        {"PrxSaveDataInitialize","SaveData initialization"},
    };
    static int seen[sizeof(marks)/sizeof(*marks)];
    if (!sos_startup_profile_active()) return;
    for (size_t i=0;i<sizeof(marks)/sizeof(*marks);++i)
        if (!strcmp(name,marks[i].symbol) && !__atomic_exchange_n(&seen[i],1,__ATOMIC_RELAXED))
            sos_startup_milestone("%s",marks[i].milestone);
}
static int32_t load_new_module(const char *path,uint64_t args,void *argp,int32_t *result) {
    ElfFile elf;
    printf("Runtime: loading module %s\n",path);
    if (!load_elf(path,&elf)) { printf("Runtime: cannot load module %s\n",path); return ERRNO(2); }
    LoadedModule *m=NULL;
    for (int i=0;i<MAX_MODULES;++i) if (!modules[i].used) { m=&modules[i]; break; }
    if (!m) { free(elf.data); return ERRNO(12); }
    memset(m,0,sizeof(*m));
    m->used=1; m->loading=1; m->handle=next_handle++; m->size=elf.image_size;
    normalize_path(path,m->path);
    m->start_vaddr=dynamic_tag(&elf,DT_INIT,0);
    m->fini_vaddr=dynamic_tag(&elf,DT_FINI,0);
    record_unwind_headers(m,elf.ph,elf.phnum);
    const Phdr *relro=find_ph(&elf,PT_SCE_RELRO);
    if (relro) {
        m->relro_vaddr=relro->vaddr;
        m->relro_size=relro->memsz;
        m->relro_flags=relro->flags;
    }
    if (m->start_vaddr>=m->size || m->fini_vaddr>=m->size) {
        free(elf.data); abandon_module(m); return ERRNO(8);
    }
    m->base=(uintptr_t)module_alloc(m->size);
    if (!m->base) { free(elf.data); abandon_module(m); return ERRNO(12); }
    for (int i=0;i<elf.phnum;++i) {
        const Phdr *p=&elf.ph[i];
        if ((p->type==PT_LOAD || p->type==PT_SCE_RELRO) && p->filesz)
            memcpy((void *)(uintptr_t)(m->base+p->vaddr),elf.data+p->offset,(size_t)p->filesz);
    }
    const Phdr *tls=find_ph(&elf,PT_TLS);
    if (tls && tls->memsz) {
        m->tls_module=runtime_add_module_tls((const void *)(uintptr_t)(m->base+tls->vaddr),tls->filesz,tls->memsz,tls->align);
        if (!m->tls_module) { free(elf.data); abandon_module(m); return ERRNO(12); }
    }
    const uint64_t rela=dynamic_tag(&elf,DT_SCE_RELA,0),relasz=dynamic_tag(&elf,DT_SCE_RELASZ,0);
    const uint64_t jmprel=dynamic_tag(&elf,DT_SCE_JMPREL,0),pltrelsz=dynamic_tag(&elf,DT_SCE_PLTRELSZ,0);
    if (!apply_table(m,&elf,rela,relasz) || !apply_table(m,&elf,jmprel,pltrelsz) || !load_exports(m,&elf)) {
        free(elf.data);
        abandon_module(m);
        return ERRNO(8);
    }
    m->exports_loaded=1;
    const uint64_t stubs=patch_import_stubs(m);
    const uint64_t tcb=patch_tcb_loads(m,&elf);
    const Phdr *procparam=find_ph(&elf,PT_SCE_PROCPARAM);
    void *parameter=NULL;
    if (procparam) {
        const uint64_t size=procparam->memsz>=8 ? rd64((const void *)(uintptr_t)(m->base+procparam->vaddr)) : 0;
        if (size<16 || size>procparam->memsz) { free(elf.data); abandon_module(m); return ERRNO(22); }
        parameter=(void *)(uintptr_t)(m->base+procparam->vaddr);
    }
    if (!finalize_mapping(m,&elf)) { free(elf.data); abandon_module(m); return ERRNO(13); }
    publish_code_range(m->base,m->size);
    repair_pending();
    m->loading=0;
    printf("Runtime: module %s relocated (%" PRIu64 " import stubs, %" PRIu64 " TCB loads)\n",path,stubs,tcb);
    free(elf.data);
    pthread_mutex_unlock(&module_lock);
    operation_set("starting module",path);
    typedef int32_t (ABI *ModuleStart)(uint64_t,void *,void *);
    printf("Runtime: starting module %s at 0x%" PRIx64 "\n",path,m->base+m->start_vaddr);
    const int32_t started=((ModuleStart)(uintptr_t)(m->base+m->start_vaddr))(args,argp,parameter);
    pthread_mutex_lock(&module_lock);
    m->started=1;
    m->start_result=started;
    if (result) *result=started;
    printf("Runtime: module %s start returned %d\nRuntime: module %s handle %d\n",path,started,path,m->handle);
    if (sos_startup_profile_active()) sos_startup_milestone("%s started",file_name(path));
    return m->handle;
}
ABI int32_t runtime_module_load_start(const char *path,uint64_t args,void *argp,uint32_t flags,void *opt,int32_t *result) {
    (void)opt;
    operation_push("sceKernelLoadStartModule",path);
    if (!path || flags) { operation_pop(); return ERRNO(22); }
    char event[96];
    pthread_mutex_lock(&module_lock);
    LoadedModule *existing=module_by_path(path);
    if (existing) {
        snprintf(event,sizeof(event),"reused PRX %s",path);
        runtime_history_note(event,"loader",(uintptr_t)__builtin_return_address(0));
        printf("Runtime: module %s already loaded, handle %d (start returned %d)\n",
               path,existing->handle,existing->start_result);
        const int32_t handle=existing->handle;
        pthread_mutex_unlock(&module_lock);
        operation_pop();
        return handle;
    }
    snprintf(event,sizeof(event),"loaded and started PRX %s",path);
    runtime_history_note(event,"loader",(uintptr_t)__builtin_return_address(0));
    const int64_t cost=sos_cost_begin();
    const int32_t handle=load_new_module(path,args,argp,result);
    sos_cost_end(SOS_COST_MODULE_LOAD,cost,1);
    pthread_mutex_unlock(&module_lock);
    operation_pop();
    return handle;
}
ABI int32_t runtime_module_dlsym(int32_t handle,const char *name,void **out) {
    operation_push("sceKernelDlsym",name);
    /* A failed lookup leaves *out untouched: callers preload a fallback there. */
    if (!name || !*name || !out) { operation_pop(); return ERRNO(22); }
    note_first_dlsym(name);
    const int64_t cost=sos_cost_begin();
    pthread_mutex_lock(&module_lock);
    LoadedModule *m=module_by_handle(handle);
    if (!m) { pthread_mutex_unlock(&module_lock); operation_pop(); return ERRNO(3); }
    char generated[12]={0};
    if (!strchr(name,'#')) runtime_symbol_to_nid(name,generated);
    GuestExport *e=find_module_export(m,name,generated[0] ? generated : NULL);
    if (!e || !e->address) { pthread_mutex_unlock(&module_lock); operation_pop(); return ERRNO(3); }
    *out=(void *)e->address;
    printf("Runtime: dlsym handle %d %s -> 0x%" PRIxPTR "\n",handle,name,e->address);
    pthread_mutex_unlock(&module_lock);
    sos_cost_end(SOS_COST_DLSYM,cost,1);
    operation_pop();
    return 0;
}
static ABI int32_t stop_unload_module(int32_t handle,uint64_t args,void *argp,uint32_t flags,void *opt,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)opt;
    operation_push("sceKernelStopUnloadModule",NULL);
    if (result) *result=0;
    pthread_mutex_lock(&module_lock);
    LoadedModule *m=module_by_handle(handle);
    pthread_mutex_unlock(&module_lock);
    if (!m) { operation_pop(); return ERRNO(3); }
    printf("Runtime: module handle %d remains resident after stop/unload request\n",handle);
    operation_pop();
    return 0;
}
/* Without the module lock: signal delivery asks from an interrupted thread that may hold it. */
int runtime_module_contains(uintptr_t address) {
    for (int i=0;i<MAX_MODULES;++i)
        if (__atomic_load_n(&modules[i].used,__ATOMIC_ACQUIRE) && address>=modules[i].base &&
            address-modules[i].base<modules[i].size) return 1;
    return 0;
}
int runtime_module_describe_address(uintptr_t address,char *out,size_t size) {
    pthread_mutex_lock(&module_lock);
    for (int i=0;i<MAX_MODULES;++i) if (modules[i].used && address>=modules[i].base && address-modules[i].base<modules[i].size) {
        const uint64_t offset=address-modules[i].base;
        if (modules[i].linked)
            snprintf(out,size,"guest image+0x%" PRIx64 " (%s+0x%" PRIx64 ")",modules[i].image_offset+offset,modules[i].path,offset);
        else snprintf(out,size,"%s+0x%" PRIx64,modules[i].path,offset);
        pthread_mutex_unlock(&module_lock);
        return 1;
    }
    pthread_mutex_unlock(&module_lock);
    return 0;
}
#define PT_GNU_EH_FRAME 0x6474e550u
/* The module's unwind-table location and its first executable segment, from its program headers. */
static void record_unwind_headers(LoadedModule *m,const Phdr *ph,int count) {
    int have_text=0;
    for (int i=0;i<count;++i) {
        if (ph[i].type==PT_GNU_EH_FRAME) { m->eh_frame_hdr=ph[i].vaddr; m->eh_frame_hdr_size=ph[i].memsz; m->headers_known=1; }
        else if (ph[i].type==PT_LOAD && (ph[i].flags&1) && !have_text) { m->seg0_vaddr=ph[i].vaddr; m->seg0_size=ph[i].memsz; have_text=1; }
    }
    if (!have_text) m->headers_known=0;
}
/* A linked module's headers: its SELF file starts with them, so only the first 64 KiB are read. */
static int read_linked_headers(LoadedModule *m) {
    const int64_t fd=runtime_file_open(m->path,0,0);
    if (fd<0) return 0;
    uint8_t raw[65536];
    const int64_t n=runtime_file_read((int)fd,raw,sizeof(raw));
    runtime_file_close((int)fd);
    if (n<64 || rd32(raw)!=SELF_MAGIC) return 0;
    const uint16_t count=rd16(raw+24);
    const uint64_t base=32+(uint64_t)count*32;
    if (!range_ok(base,64,(uint64_t)n) || memcmp(raw+base,"""ELF",7)) return 0;
    const uint64_t phoff=rd64(raw+base+32);
    const uint16_t nph=rd16(raw+base+56);
    if (!nph || nph>MAX_PHDRS || rd16(raw+base+54)!=56 || !range_ok(base+phoff,(uint64_t)nph*56,(uint64_t)n)) return 0;
    Phdr ph[MAX_PHDRS];
    for (int i=0;i<nph;++i) {
        const uint8_t *p=raw+base+phoff+(uint64_t)i*56;
        ph[i]=(Phdr){rd32(p),rd32(p+4),rd64(p+8),rd64(p+16),rd64(p+24),rd64(p+32),rd64(p+40),rd64(p+48)};
    }
    record_unwind_headers(m,ph,nph);
    return m->headers_known;
}
/* sceKernelGetModuleInfoForUnwind: where a module's unwind tables are, for the guest C++ unwinder. */
typedef struct { uint64_t size; char name[256]; uint64_t eh_frame_hdr, eh_frame, eh_frame_size, seg0, seg0_size; } UnwindInfo;
#ifdef _WIN32
static int readable_run(uintptr_t at,uintptr_t end,uintptr_t *run_end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)at,&info,sizeof(info)) || info.State!=MEM_COMMIT) return 0;
    if (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) return 0;
    *run_end=(uintptr_t)info.BaseAddress+info.RegionSize;
    if (*run_end>end) *run_end=end;
    return 1;
}
#endif
static int32_t rd_s32(const unsigned char *p) { int32_t v; memcpy(&v,p,4); return v; }
static int eh_table_valid(uintptr_t hdr,uintptr_t lo,uintptr_t hi) {
    const unsigned char *h=(const unsigned char *)hdr;
    if (h[0]!=1 || h[1]!=0x1b || h[2]!=0x03 || h[3]!=0x3b) return 0;
    uint32_t count; memcpy(&count,h+8,4);
    if (count<16 || count>(1u<<22) || hdr+12+(uint64_t)count*8>hi) return 0;
    const int64_t frames=(int64_t)hdr+4+rd_s32(h+4);
    if (frames<(int64_t)lo || frames>=(int64_t)hi) return 0;
    int64_t previous=-1;
    for (uint32_t i=0;i<count && (i<64 || i+64>=count);++i) {
        const int64_t pc=(int64_t)hdr+rd_s32(h+12+8*(uint64_t)i), fde=(int64_t)hdr+rd_s32(h+16+8*(uint64_t)i);
        if (pc<=previous || pc<(int64_t)lo || pc>=(int64_t)hi || fde<(int64_t)lo || fde>=(int64_t)hi) return 0;
        previous=pc;
    }
    return 1;
}
/* The tables are found by their .eh_frame_hdr signature inside the module's readable pages. */
static void find_unwind_tables(LoadedModule *m) {
#ifdef _WIN32
    const uintptr_t lo=(uintptr_t)m->base, hi=lo+(uintptr_t)m->size;
    for (uintptr_t at=lo,run_end;at<hi;at=run_end) {
        if (!readable_run(at,hi,&run_end)) { run_end=(at|0xfff)+1; continue; }
        for (uintptr_t p=(at+3)&~(uintptr_t)3;p+12<=run_end;p+=4) {
            const unsigned char *h=(const unsigned char *)p;
            if (h[0]!=1 || h[1]!=0x1b || h[2]!=0x03 || h[3]!=0x3b || p+12+8*16>run_end) continue;
            if (!eh_table_valid(p,lo,hi)) continue;
            uint32_t count; memcpy(&count,h+8,4);
            const uint64_t frames=(uint64_t)((int64_t)p+4+rd_s32(h+4));
            m->eh_frame_hdr=p;
            m->eh_frame=frames;
            m->eh_frame_size=p>frames ? p-frames : hi-p;
            return;
        }
    }
#else
    (void)m;
#endif
}
static int find_module_snapshot(uint64_t address,LoadedModule *snapshot,int *index) {
    int found=0;
    pthread_mutex_lock(&module_lock);
    for (int i=0;i<MAX_MODULES && !found;++i)
        if (modules[i].used && address>=modules[i].base && address-modules[i].base<modules[i].size) {
            *snapshot=modules[i]; snapshot->exports=NULL; snapshot->pending=NULL; found=1; *index=i;
        }
    pthread_mutex_unlock(&module_lock);
    return found;
}
static void ensure_unwind_tables(LoadedModule *snapshot,int index) {
    if (snapshot->eh_searched) return;
    const uintptr_t module_base=(uintptr_t)snapshot->base;
    if (snapshot->linked) read_linked_headers(snapshot);
    if (snapshot->headers_known && snapshot->eh_frame_hdr_size>=12 &&
        snapshot->eh_frame_hdr+snapshot->eh_frame_hdr_size<=snapshot->size) {
        const uintptr_t hdr=module_base+(uintptr_t)snapshot->eh_frame_hdr;
        const uint8_t *h=(const uint8_t *)hdr;
        if (h[0]==1 && h[1]==0x1b) {
            const uint64_t frames=(uint64_t)((int64_t)hdr+4+rd_s32(h+4));
            snapshot->eh_frame_hdr=hdr;
            snapshot->eh_frame=frames;
            snapshot->eh_frame_size=hdr>frames ? hdr-frames : module_base+snapshot->size-hdr;
        } else snapshot->headers_known=0;
    }
    if (!snapshot->headers_known) find_unwind_tables(snapshot);
    pthread_mutex_lock(&module_lock);
    modules[index].eh_frame_hdr=snapshot->eh_frame_hdr; modules[index].eh_frame=snapshot->eh_frame;
    modules[index].eh_frame_size=snapshot->eh_frame_size; modules[index].eh_searched=1;
    modules[index].headers_known=snapshot->headers_known; modules[index].seg0_vaddr=snapshot->seg0_vaddr;
    modules[index].seg0_size=snapshot->seg0_size;
    pthread_mutex_unlock(&module_lock);
}
static const char *module_file_name(const LoadedModule *m) {
    const char *name=strrchr(m->path,'/');
    return name ? name+1 : m->path;
}
int32_t runtime_module_unwind_info(uint64_t address,int32_t flags,void *out) {
    UnwindInfo *info=out;
    if (flags>=3) { if (info) memset(info,0,sizeof(*info)); return ERRNO(22); }
    if (!info) return ERRNO(14);
    if (info->size<sizeof(*info)) return ERRNO(22);
    LoadedModule snapshot;
    int index=-1;
    if (!find_module_snapshot(address,&snapshot,&index)) return ERRNO(3);
    ensure_unwind_tables(&snapshot,index);
    memset(info,0,sizeof(*info));
    info->size=sizeof(*info);
    snprintf(info->name,sizeof(info->name),"%s",module_file_name(&snapshot));
    info->eh_frame_hdr=snapshot.eh_frame_hdr; info->eh_frame=snapshot.eh_frame; info->eh_frame_size=snapshot.eh_frame_size;
    info->seg0=snapshot.headers_known ? snapshot.base+snapshot.seg0_vaddr : snapshot.base;
    info->seg0_size=snapshot.headers_known ? snapshot.seg0_size : snapshot.size;
    return 0;
}
/* sceKernelGetModuleInfoFromAddr: OrbisKernelModuleInfoEx as shadPS4 lays it out. */
typedef struct { uint64_t address; uint32_t size; int32_t prot; } SegmentInfo;
typedef struct {
    uint64_t size;
    char name[256];
    int32_t id;
    uint32_t tls_index;
    uint64_t tls_init_addr;
    uint32_t tls_init_size, tls_size, tls_offset, tls_align;
    uint64_t init_proc, fini_proc, reserved1, reserved2, eh_frame_hdr, eh_frame;
    uint32_t eh_frame_hdr_size, eh_frame_size;
    SegmentInfo segments[4];
    uint32_t segment_count;
} ModuleInfoEx;
int32_t runtime_module_info_from_address(uint64_t address,int32_t flags,void *out) {
    ModuleInfoEx *info=out;
    if (flags>=3) { if (info) memset(info,0,sizeof(*info)); return ERRNO(22); }
    if (!info) return ERRNO(14);
    LoadedModule snapshot;
    int index=-1;
    if (!find_module_snapshot(address,&snapshot,&index)) return ERRNO(3);
    ensure_unwind_tables(&snapshot,index);
    memset(info,0,sizeof(*info));
    info->size=sizeof(*info);
    snprintf(info->name,sizeof(info->name),"%s",module_file_name(&snapshot));
    info->id=snapshot.handle;
    info->tls_index=(uint32_t)snapshot.tls_module;
    info->init_proc=snapshot.start_vaddr ? snapshot.base+snapshot.start_vaddr : 0;
    info->fini_proc=snapshot.fini_vaddr ? snapshot.base+snapshot.fini_vaddr : 0;
    info->eh_frame_hdr=snapshot.eh_frame_hdr; info->eh_frame=snapshot.eh_frame;
    info->eh_frame_hdr_size=(uint32_t)snapshot.eh_frame_hdr_size; info->eh_frame_size=(uint32_t)snapshot.eh_frame_size;
    info->segments[0]=(SegmentInfo){snapshot.headers_known ? snapshot.base+snapshot.seg0_vaddr : snapshot.base,
                                    (uint32_t)(snapshot.headers_known ? snapshot.seg0_size : snapshot.size),5};
    info->segment_count=1;
    return 0;
}
static ABI int32_t info_from_address_export(uint64_t address,int32_t flags,void *info) {
    return runtime_module_info_from_address(address,flags,info);
}
static ABI int32_t unwind_info_export(uint64_t address,int32_t flags,void *info) { return runtime_module_unwind_info(address,flags,info); }
static const RuntimeExport exports_table[]={
    {"sceKernelGetModuleInfoForUnwind",unwind_info_export}, {"sceSysmoduleGetModuleInfoForUnwind",unwind_info_export},
    {"sceKernelGetModuleInfoFromAddr",info_from_address_export},
    {"sceKernelLoadStartModule",runtime_module_load_start},
    {"sceKernelDlsym",runtime_module_dlsym},
    {"sceKernelStopUnloadModule",stop_unload_module},
};
uintptr_t runtime_module_resolve(const char *name) { return RUNTIME_LOOKUP(exports_table,name); }
