/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include "runtime.h"
#include "startup_phase.h"
#include "startup_profile.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef _WIN32
#include <windows.h>
/* 64-bit sizes and times; directories are never opened as CRT descriptors. */
typedef struct _stat64 HostStat;
/* The UCRT of Windows 11 24H2 and later stats a file symbolic link itself (size 0): such a file is stat'ed through
 * an open descriptor, which follows the link. */
static int host_stat(const char *path,HostStat *s) {
    if (_stat64(path,s)) return -1;
    const DWORD attributes=GetFileAttributesA(path);
    if (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        !(attributes&FILE_ATTRIBUTE_DIRECTORY)) {
        const int fd=_open(path,_O_RDONLY|_O_BINARY);
        if (fd>=0) {
            HostStat target;
            if (!_fstat64(fd,&target)) *s=target;
            _close(fd);
        }
    }
    return 0;
}
#define host_fstat(fd,s) _fstat64(fd,s)
#define host_lseek _lseeki64
#define O_DIRECTORY 0x40000000 /* sos marker, removed before _open */
#else
typedef struct stat HostStat;
#define host_stat(path,s) stat(path,s)
#define host_fstat(fd,s) fstat(fd,s)
#define host_lseek lseek
#endif
#ifdef _WIN32
#define IS_BACKSLASH_C(c) ((c)==0x5c)
#else
#define IS_BACKSLASH_C(c) 0
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 16

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;
typedef struct { void *base; uint64_t length; } GuestIovec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *names; size_t count, *offsets; unsigned char *types; } Listing;
/* A game file the guest reads with byte ranges replaced and possibly past its end; the host file is never written. */
typedef struct { uint64_t offset, size; const unsigned char *data; } OverlayPatch;
typedef struct { char guest[512]; uint64_t length; OverlayPatch *patches; size_t count; } Overlay;
typedef struct { int used, host; Listing *dir; size_t position; char path[512]; const Overlay *overlay; int device; int flags, fdflags; } File;
typedef struct { char guest[64]; char host[512]; int read_only; } Mount;
#define MAX_OVERLAYS 4
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static Overlay overlays[MAX_OVERLAYS];
static size_t overlay_count;
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

/* Operations that fail for a reason other than "not found" are listed, a bounded number of times. */
static void note_failure(const char *operation,const char *guest,int error) {
    static int reported;
    if (error==ENOENT || __atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)>=32) return;
    printf("Runtime: %s(%s) -> %s\n",operation,guest ? guest : "",strerror(error));
}
static int mount_as(const char *guest,const char *host,int read_only) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        mounts[i].read_only=read_only;
        pthread_mutex_unlock(&lock); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=64 || strlen(host)>=512) { pthread_mutex_unlock(&lock); return -1; }
    snprintf(mounts[mount_count].guest,64,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    mounts[mount_count].read_only=read_only;
    ++mount_count;
    pthread_mutex_unlock(&lock);
    return 0;
}
int runtime_file_mount(const char *guest,const char *host) { return mount_as(guest,host,0); }
int runtime_file_mount_read_only(const char *guest,const char *host) { return mount_as(guest,host,1); }
void runtime_file_unmount(const char *guest) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        mounts[i]=mounts[--mount_count]; break;
    }
    pthread_mutex_unlock(&lock);
}
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }
void runtime_file_configure(const char *app0,const char *user) {
    char path[600];
    snprintf(user_root,sizeof(user_root),"%s",user);
    mount_as("/app0",app0,1);
    mount_as("/hostapp",app0,1);
    const char *writable[]={"temp0","download0","data"};
    mkdir(user,0755);
    for (int i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",user,writable[i]);
        mkdir(path,0755);
        char guest[32]; snprintf(guest,sizeof(guest),"/%s",writable[i]);
        runtime_file_mount(guest,path);
    }
}
#ifndef _WIN32
/* The PS4 file system ignores case, Linux's does not: a path component that does not exist is replaced by the
 * entry of its folder that differs only in case. The mount's own folder (the first `root` bytes) is exact. */
static void match_case(char *path,size_t root) {
    if (root && path[root-1]=='/') --root;
    for (size_t at=root;path[at]=='/';) {
        size_t start=at+1,end=start;
        while (path[end] && path[end]!='/') ++end;
        if (end==start) { at=end; continue; }
        char saved=path[end];
        path[end]=0;
        struct stat info;
        if (lstat(path,&info)) {
            path[at]=0;
            DIR *folder=opendir(path);
            path[at]='/';
            int found=0;
            for (struct dirent *e; folder && !found && (e=readdir(folder));)
                if (strlen(e->d_name)==end-start && !strcasecmp(e->d_name,path+start)) { memcpy(path+start,e->d_name,end-start); found=1; }
            if (folder) closedir(folder);
            if (!found) { path[end]=saved; return; }
        }
        path[end]=saved;
        at=end;
    }
}
#endif
static void absolute_guest_path(const char *guest,char *out,size_t size) {
    if (guest[0]!='/') snprintf(out,size,"/app0/%s",guest);
    else snprintf(out,size,"%s",guest);
}
/* Absolute, with repeated separators and "." components removed; ".." is refused, never resolved. */
static int normalize_guest(const char *guest,char *out,size_t size) {
    char raw[1100];
    if (!guest || strlen(guest)>=1000) return ENAMETOOLONG;
    absolute_guest_path(guest,raw,sizeof(raw));
    size_t n=0;
    for (const char *p=raw;*p;) {
        while (*p=='/' || IS_BACKSLASH_C(*p)) ++p;
        const char *start=p;
        while (*p && *p!='/' && !IS_BACKSLASH_C(*p)) ++p;
        const size_t length=(size_t)(p-start);
        if (!length) break;
        if (length==1 && start[0]=='.') continue;
        if (length==2 && start[0]=='.' && start[1]=='.') return EACCES;
        if (length>255) return ENAMETOOLONG;
#ifdef _WIN32
        if (memchr(start,':',length)) return ENOENT;
#endif
        if (n+length+2>size) return ENAMETOOLONG;
        out[n++]='/';
        memcpy(out+n,start,length); n+=length;
    }
    if (!n) out[n++]='/';
    out[n]=0;
    return 0;
}
static int is_root(const char *guest) {
    char path[1100];
    return !normalize_guest(guest,path,sizeof(path)) && !strcmp(path,"/");
}
static int is_mount_root(const char *normalized) {
    int found=0;
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count && !found;++i) found=!strcmp(mounts[i].guest,normalized);
    pthread_mutex_unlock(&lock);
    return found;
}
static int read_only_mount(const char *normalized) {
    int found=0;
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count && !found;++i) {
        const size_t n=strlen(mounts[i].guest);
        found=mounts[i].read_only && !strncmp(normalized,mounts[i].guest,n) && (!normalized[n] || normalized[n]=='/');
    }
    pthread_mutex_unlock(&lock);
    return found;
}
static int same_path_ignoring_case(const char *a,const char *b) {
    for (;*a && *b;++a,++b) if (tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return 0;
    return *a==*b;
}
static const Overlay *overlay_of(const char *guest) {
    if (!guest || !*guest || !overlay_count) return NULL;
    char path[1024];
    absolute_guest_path(guest,path,sizeof(path));
    for (size_t i=0;i<overlay_count;++i) if (same_path_ignoring_case(overlays[i].guest,path)) return &overlays[i];
    return NULL;
}
enum { DEVICE_NONE, DEVICE_NULL, DEVICE_ZERO, DEVICE_RANDOM, DEVICE_CONSOLE, DEVICE_STDERR };
static int device_kind(const char *guest) {
    if (!guest) return DEVICE_NONE;
    char path[64];
    absolute_guest_path(guest,path,sizeof(path));
    if (strncmp(path,"/dev/",5)) return DEVICE_NONE;
    if (!strcmp(path,"/dev/null") || !strcmp(path,"/dev/stdin") || !strcmp(path,"/dev/fd/0") ||
        !strcmp(path,"/dev/deci_stdin")) return DEVICE_NULL;
    if (!strcmp(path,"/dev/zero")) return DEVICE_ZERO;
    if (!strcmp(path,"/dev/urandom") || !strcmp(path,"/dev/random") || !strcmp(path,"/dev/srandom") ||
        !strcmp(path,"/dev/rng")) return DEVICE_RANDOM;
    if (!strcmp(path,"/dev/stderr") || !strcmp(path,"/dev/fd/2") || !strcmp(path,"/dev/deci_stderr")) return DEVICE_STDERR;
    if (!strcmp(path,"/dev/stdout") || !strcmp(path,"/dev/fd/1") || !strcmp(path,"/dev/deci_stdout") ||
        !strcmp(path,"/dev/console") || !strcmp(path,"/dev/deci_tty6") || !strcmp(path,"/dev/deci_tty7")) return DEVICE_CONSOLE;
    return DEVICE_NONE;
}
static void device_stat(GuestStat *out) {
    memset(out,0,sizeof(*out));
    out->mode=0020666; /* character device, rw for all */
    out->nlink=1;
    out->blksize=4096;
}
static void directory_stat(GuestStat *out) {
    memset(out,0,sizeof(*out));
    out->mode=0040755;
    out->nlink=2;
    out->size=512; out->blocks=1; out->blksize=4096;
}
static int contains(const void *buffer,uint64_t size,const char *text) {
    const size_t n=strlen(text);
    for (const char *at=buffer,*end=(const char *)buffer+size; (size_t)(end-at)>=n; ++at)
        if (*at==*text && !memcmp(at,text,n)) return 1;
    return 0;
}
/* Guest log lines that mark coarse startup phases before the splash video. */
static void note_guest_milestone(const void *buffer,uint64_t size) {
    static const struct { const char *text, *milestone; } marks[]={
        {"NpToolkit Initialized","guest: NpToolkit initialized"},
        {"Finished Initializing PS4 Scripts","guest: PS4 scripts initialized"},
        {"SplashScreen","guest: first SplashScreen line"},
    };
    static int seen[sizeof(marks)/sizeof(*marks)];
    if (!sos_startup_profile_active()) return;
    for (size_t i=0;i<sizeof(marks)/sizeof(*marks);++i)
        if (!__atomic_load_n(&seen[i],__ATOMIC_RELAXED) && contains(buffer,size,marks[i].text) &&
            !__atomic_exchange_n(&seen[i],1,__ATOMIC_RELAXED))
            sos_startup_milestone("%s",marks[i].milestone);
}
static void console_write(int fd,const void *buffer,uint64_t size) {
    note_guest_milestone(buffer,size);
    FILE *out=fd==2 ? stderr : stdout;
    fwrite(buffer,1,(size_t)size,out);
    fflush(out);
}
static int host_random_bytes(void *buffer,size_t size) {
#ifdef _WIN32
    return compat_random(buffer,size);
#else
    FILE *f=fopen("/dev/urandom","rb");
    if (!f) return -1;
    const size_t got=fread(buffer,1,size,f);
    fclose(f);
    return got==size ? 0 : -1;
#endif
}
static int64_t device_read(int kind,void *buffer,uint64_t size) {
    if (kind==DEVICE_NULL || kind==DEVICE_CONSOLE || kind==DEVICE_STDERR) return 0;
    if (kind==DEVICE_ZERO) { memset(buffer,0,size); return (int64_t)size; }
    return host_random_bytes(buffer,size) ? -EIO : (int64_t)size;
}
/* Registered before the guest starts: opens and reads take no lock for the table. The patches' data is borrowed, not
 * copied, and must stay valid for the life of the process. A later registration of the same file with the same length
 * adds its patches on top of the earlier ones. */
int runtime_file_overlay(const char *guest,uint64_t length,const RuntimeFilePatch *patches,size_t count) {
    if (!guest || strlen(guest)+6>=sizeof(overlays[0].guest)) return -1;
    for (size_t i=0;i<count;++i)
        if (patches[i].offset>length || patches[i].size>length-patches[i].offset) return -1;
    Overlay *existing=(Overlay *)overlay_of(guest);
    if (existing) {
        if (existing->length!=length) return -1;
        OverlayPatch *grown=realloc(existing->patches,(existing->count+count+1)*sizeof(OverlayPatch));
        if (!grown) return -1;
        existing->patches=grown;
        for (size_t i=0;i<count;++i)
            existing->patches[existing->count++]=(OverlayPatch){patches[i].offset,patches[i].size,patches[i].data};
        return 0;
    }
    if (overlay_count==MAX_OVERLAYS) return -1;
    Overlay o={.length=length,.patches=calloc(count ? count : 1,sizeof(OverlayPatch))};
    if (!o.patches) return -1;
    for (;o.count<count;++o.count) {
        const RuntimeFilePatch *p=&patches[o.count];
        o.patches[o.count]=(OverlayPatch){p->offset,p->size,p->data};
    }
    absolute_guest_path(guest,o.guest,sizeof(o.guest));
    overlays[overlay_count++]=o;
    return 0;
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
#define MAX_UNMAPPED 16
typedef struct { char prefix[64]; unsigned count; } UnmappedPrefix;
static UnmappedPrefix unmapped_prefixes[MAX_UNMAPPED];
static size_t unmapped_count;
static pthread_mutex_t unmapped_lock=PTHREAD_MUTEX_INITIALIZER;
static void guest_prefix(const char *path,char *out,size_t size) {
    if (!path || path[0]!='/') { snprintf(out,size,"%s",path ? path : ""); return; }
    if (!strncmp(path,"/archive/mount/point",20)) { snprintf(out,size,"/archive/mount/point"); return; }
    const char *second=strchr(path+1,'/');
    size_t length=second ? (size_t)(second-path) : strlen(path);
    if (length>=size) length=size-1;
    memcpy(out,path,length); out[length]=0;
}
static void note_unmapped(const char *guest) {
    char prefix[64];
    guest_prefix(guest,prefix,sizeof(prefix));
    int first=0;
    pthread_mutex_lock(&unmapped_lock);
    size_t i;
    for (i=0;i<unmapped_count;++i) if (!strcmp(prefix,unmapped_prefixes[i].prefix)) break;
    if (i==unmapped_count && unmapped_count<MAX_UNMAPPED) {
        snprintf(unmapped_prefixes[unmapped_count].prefix,sizeof(unmapped_prefixes[0].prefix),"%s",prefix);
        unmapped_prefixes[unmapped_count].count=1;
        ++unmapped_count; first=1;
    } else if (i<unmapped_count) {
        ++unmapped_prefixes[i].count;
    }
    pthread_mutex_unlock(&unmapped_lock);
    if (first) {
        const char *thread=runtime_thread_current_name();
        fprintf(stderr,"Runtime: no mount for guest path %s (prefix %s, thread %d %s)\n",
                guest,prefix,runtime_thread_id_current(),thread ? thread : "unknown");
    }
}
static int translate(const char *guest,char *out,size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1100];
    int result=normalize_guest(guest,buffer,sizeof(buffer));
    if (result) return result;
    pthread_mutex_lock(&lock);
    size_t best=0; const Mount *m=NULL;
    for (size_t i=0;i<mount_count;++i) {
        size_t n=strlen(mounts[i].guest);
        if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
    }
    size_t root=0;
    if (!m) result=ENOENT;
    else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
    else root=strlen(m->host);
    pthread_mutex_unlock(&lock);
    if (result==ENOENT) note_unmapped(guest);
#ifndef _WIN32
    if (!result) match_case(out,root);
#else
    (void)root;
#endif
    return result;
}
#ifndef _WIN32
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
    if (flags&0x80) r|=O_SYNC;
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
#ifdef _WIN32
    r|=O_BINARY;
#endif
    return r|O_CLOEXEC;
}
#endif
static void free_listing(Listing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static Listing *list_directory(const char *path) {
    DIR *d=opendir(path);
    if (!d) return NULL;
    Listing *l=calloc(1,sizeof(*l));
    size_t capacity=0,bytes=0,cap_names=0;
    struct dirent *e;
    while (l && (e=readdir(d))) {
        size_t n=strlen(e->d_name)+1;
        if (l->count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            l->offsets=realloc(l->offsets,capacity*sizeof(size_t));
            l->types=realloc(l->types,capacity);
        }
        if (bytes+n>cap_names) { cap_names=(bytes+n)*2; l->names=realloc(l->names,cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); runtime_exit(1); }
        memcpy(l->names+bytes,e->d_name,n);
        l->offsets[l->count]=bytes;
#ifdef _WIN32
        /* MinGW's dirent has no d_type. */
        char child[1100]; HostStat entry;
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        l->types[l->count]=host_stat(child,&entry) ? 0 : S_ISDIR(entry.st_mode) ? 4 : S_ISREG(entry.st_mode) ? 8 : 0;
#else
        unsigned char type=e->d_type;
        if (type==DT_LNK || type==DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(d),e->d_name,&entry,0))
                type=S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
#endif
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
static void convert_stat(const HostStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino;
    g->nlink=(uint16_t)s->st_nlink;
    g->size=s->st_size;
#ifdef _WIN32
    /* FreeBSD mode bits: directories rwxr-xr-x, files rw-r--r--. */
    g->mode=(uint16_t)(S_ISDIR(s->st_mode) ? 0040755 : 0100644);
    g->blocks=(s->st_size+511)/512; g->blksize=4096;
    g->atime=(GuestTimespec){s->st_atime,0};
    g->mtime=(GuestTimespec){s->st_mtime,0};
    g->ctime=(GuestTimespec){s->st_ctime,0};
#else
    g->mode=(uint16_t)s->st_mode;
    g->blocks=s->st_blocks; g->blksize=(uint32_t)s->st_blksize;
    g->atime=(GuestTimespec){s->st_atim.tv_sec,s->st_atim.tv_nsec};
    g->mtime=(GuestTimespec){s->st_mtim.tv_sec,s->st_mtim.tv_nsec};
    g->ctime=(GuestTimespec){s->st_ctim.tv_sec,s->st_ctim.tv_nsec};
#endif
    g->birthtime=g->ctime;
}
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
/* Game mounts and mounts registered read-only reject every change. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    char path[1100];
    if (normalize_guest(p,path,sizeof(path))) return 0;
    return (!strncmp(path,"/app0",5) && (!path[5] || path[5]=='/')) ||
           (!strncmp(path,"/hostapp",8) && (!path[8] || path[8]=='/')) || read_only_mount(path);
}
#ifdef _WIN32
/* Opened with FILE_SHARE_DELETE like POSIX: an open file can be renamed or unlinked. */
static int host_open(const char *path,int flags) {
    const int mode=flags&3;
    DWORD access=mode==0 ? GENERIC_READ : mode==1 ? GENERIC_WRITE : GENERIC_READ|GENERIC_WRITE;
    DWORD disposition=OPEN_EXISTING;
    if ((flags&0x200) && (flags&0x800)) disposition=CREATE_NEW;
    else if ((flags&0x200) && (flags&0x400)) disposition=CREATE_ALWAYS;
    else if (flags&0x200) disposition=OPEN_ALWAYS;
    else if (flags&0x400) { disposition=TRUNCATE_EXISTING; access|=GENERIC_WRITE; }
    HANDLE handle=CreateFileA(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,disposition,
                              FILE_ATTRIBUTE_NORMAL,NULL);
    if (handle==INVALID_HANDLE_VALUE) { errno=compat_errno_from_win32(GetLastError()); return -1; }
    const int fd=_open_osfhandle((intptr_t)handle,_O_BINARY|_O_NOINHERIT|((flags&0x8) ? _O_APPEND : 0)|(mode==0 ? _O_RDONLY : 0));
    if (fd<0) { CloseHandle(handle); errno=EMFILE; }
    return fd;
}
#endif
static int allocate_device_file(const char *guest,int flags,int device) {
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd>=0) {
        files[fd]=(File){.used=1,.host=-1,.device=device,.flags=flags};
        snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
        ++opens;
    }
    pthread_mutex_unlock(&lock);
    return fd;
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t open_file(const char *guest,int flags,int mode);
static int64_t do_open(const char *guest,int flags,int mode) {
    const int64_t cost=sos_cost_begin();
    const int64_t r=open_file(guest,flags,mode);
    sos_cost_end(SOS_COST_FILE_OPEN,cost,0);
    if (r<0) note_failure("open",guest,(int)-r);
    return r;
}
static int64_t open_file(const char *guest,int flags,int mode) {
    if (!guest || !*guest) return -ENOENT;
    if ((flags&3)==3) return -EINVAL;
    const int device=device_kind(guest);
    if (device) {
        const int fd=allocate_device_file(guest,flags,device);
        return fd>=0 ? fd : -EMFILE;
    }
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return is_root(guest) ? ((flags&3) ? -EISDIR : -EACCES) : -e;
    HostStat s;
    const int exists=!host_stat(path,&s);
    const int stat_error=exists ? 0 : errno;
    if (game_path(guest)) {
        if ((flags&3) || (flags&(0x8|0x400))) return -EROFS;
        if (flags&0x200) {
            if (!exists) return -EROFS;
            if (flags&0x800) return -EEXIST;
        }
    }
#ifdef _WIN32
    /* Windows cannot open a directory as a CRT descriptor: directories are listings only. */
    (void)mode;
    Listing *dir=NULL;
    int host=-1;
    if (exists && S_ISDIR(s.st_mode)) {
        if ((flags&0x200) && (flags&0x800)) return -EEXIST;
        if ((flags&3) || (flags&(0x200|0x400))) return -EISDIR;
        if (!(dir=list_directory(path))) return -EACCES;
    } else if (flags&0x20000) {
        e=!exists && stat_error==ENOENT ? ENOENT : ENOTDIR;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    } else {
        host=host_open(path,flags&~0x20000);
        if (host<0) {
            e=errno;
            if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
            return -e;
        }
    }
#else
    (void)stat_error;
    int host=open(path,host_flags(flags),mode ? mode : 0644);
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    }
    Listing *dir=NULL;
    if (!fstat(host,&s) && S_ISDIR(s.st_mode)) dir=list_directory(path);
#endif
    /* SOS_FILE_TRACE=1: every file the game opens. */
    static int file_trace=-1;
    if (file_trace<0) { const char *t=getenv("SOS_FILE_TRACE"); file_trace=t && t[0]=='1'; }
    if (file_trace) printf("Runtime: open(%s)%s\n",guest,dir ? " [directory]" : "");
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) { pthread_mutex_unlock(&lock); if (host>=0) close(host); free_listing(dir); return -EMFILE; }
    files[fd]=(File){.used=1,.host=host,.dir=dir,.overlay=dir ? NULL : overlay_of(guest),.flags=flags};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    pthread_mutex_unlock(&lock);
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    if (!f) { pthread_mutex_unlock(&lock); return runtime_socket_close_file(fd); }
    if (f->host>=0) close(f->host);
    free_listing(f->dir);
    *f=(File){0};
    pthread_mutex_unlock(&lock);
    return 0;
}
static int64_t do_fcntl(int fd,int command,intptr_t argument) {
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (command==1) result=f->fdflags;
    else if (command==2) f->fdflags=(int)argument;
    else if (command==3) result=f->flags;
    else if (command==4) f->flags=(f->flags&3)|((int)argument&~3);
    else if (command==0) {
        if (f->dir) result=-EINVAL;
        else {
            int target=(int)argument<3 ? 3 : (int)argument;
            int created=-1;
            for (int i=target;i<MAX_FILES;++i) if (!files[i].used) { created=i; break; }
            if (created<0) result=-EMFILE;
            else {
                int host=f->host>=0 ? dup(f->host) : -1;
                if (f->host>=0 && host<0) result=-errno;
                else {
                    files[created]=*f;
                    files[created].host=host;
                    files[created].fdflags=0;
                    result=created;
                }
            }
        }
    } else result=-EINVAL;
    pthread_mutex_unlock(&lock);
    if (!f) {
        if (fd>=0 && fd<3) return command==3 ? 2 : (command==1 || command==2 || command==4) ? 0 : -EINVAL;
        return runtime_socket_fcntl_file(fd,command,argument);
    }
    return result;
}
#ifdef _WIN32
/* The CRT reads and writes at most INT_MAX bytes per call. */
static ssize_t host_read(int h,void *buffer,uint64_t size) { return read(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
static ssize_t host_write(int h,const void *buffer,uint64_t size) { return write(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
#else
#define host_read read
#define host_write write
#endif
/* Pages of the destination may be write-protected for GPU tracking or not yet committed: the kernel's copy then
 * fails with EFAULT instead of faulting to our handler. A user-mode write to each page first
 * goes through the handler, which unprotects or commits it (and records the upcoming write). */
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
/* The kernel's copy from a not yet committed guest page (Windows, past the commit limit) fails instead of faulting: a
 * user-mode read of each page commits it first. */
static void touch_for_read(const void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) (void)*(volatile const unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
}
/* The host bytes, zeros past the host file's end, then the overlay's patches on top. */
static int64_t overlay_pread(const File *f,void *buffer,uint64_t size,int64_t offset) {
    const Overlay *o=f->overlay;
    if (offset<0) return -EINVAL;
    if ((uint64_t)offset>=o->length) return 0;
    if (size>o->length-(uint64_t)offset) size=o->length-(uint64_t)offset;
    HostStat s;
    if (host_fstat(f->host,&s)) return -errno;
    uint64_t host_end=(uint64_t)s.st_size, filled=0;
    while (filled<size && (uint64_t)offset+filled<host_end) {
        ssize_t n=pread(f->host,(unsigned char *)buffer+filled,size-filled,offset+(int64_t)filled);
        if (n<0) return -errno;
        if (!n) break;
        filled+=(uint64_t)n;
    }
    memset((unsigned char *)buffer+filled,0,size-filled);
    for (size_t i=0;i<o->count;++i) {
        const OverlayPatch *p=&o->patches[i];
        uint64_t lo=p->offset>(uint64_t)offset ? p->offset : (uint64_t)offset;
        uint64_t hi=p->offset+p->size<(uint64_t)offset+size ? p->offset+p->size : (uint64_t)offset+size;
        if (lo<hi) memcpy((unsigned char *)buffer+(lo-(uint64_t)offset),p->data+(lo-p->offset),hi-lo);
    }
    return (int64_t)size;
}
/* Only told what was read: an observer has no way to change how a read went for the guest. */
typedef void (*ReadObserver)(const char *guest,uint64_t offset,uint64_t size);
static ReadObserver read_observer;
void runtime_file_set_read_observer(ReadObserver observe) { __atomic_store_n(&read_observer,observe,__ATOMIC_RELEASE); }
/* The host position a read is about to start at, when someone is watching; -1 otherwise. */
static int64_t observed_position(int fd,int h) {
    return fd>=3 && __atomic_load_n(&read_observer,__ATOMIC_ACQUIRE) ? host_lseek(h,0,SEEK_CUR) : -1;
}
static void observe_read(int fd,int64_t offset,int64_t n) {
    ReadObserver observe=__atomic_load_n(&read_observer,__ATOMIC_ACQUIRE);
    if (!observe || fd<3 || offset<0 || n<=0) return;
    char path[sizeof(files[0].path)];
    pthread_mutex_lock(&lock);
    const File *f=get(fd);
    if (f) memcpy(path,f->path,sizeof(path));
    pthread_mutex_unlock(&lock);
    if (f) observe(path,(uint64_t)offset,(uint64_t)n);
}
static int64_t read_file(int fd,void *buffer,uint64_t size) {
    File *file=get(fd);
    if (!file) return fd==0 ? 0 : runtime_socket_read_file(fd,buffer,size);
    if (file->device) {
        touch_for_write(buffer,size);
        const int64_t n=device_read(file->device,buffer,size);
        if (n>0) { __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED); }
        return n;
    }
    int h=file->host;
    if (h<0) return file->dir ? -EISDIR : -EBADF;
    touch_for_write(buffer,size);
    const File *f=get(fd);
    if (f && f->overlay) {
        int64_t at=host_lseek(h,0,SEEK_CUR);
        if (at<0) return -errno;
        int64_t n=overlay_pread(f,buffer,size,at);
        if (n>0 && host_lseek(h,at+n,SEEK_SET)<0) return -errno;
        if (n>0) { __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED); }
        observe_read(fd,at,n);
        return n;
    }
    const int64_t at=observed_position(fd,h);
    ssize_t n=host_read(h,buffer,size);
    if (n<0) return -errno;
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    observe_read(fd,at,n);
    return n;
}
static int64_t pread_file(int fd,void *buffer,uint64_t size,int64_t offset) {
    File *file=get(fd);
    if (!file) return -EBADF;
    if (offset<0) return -EINVAL;
    if (file->device) {
        touch_for_write(buffer,size);
        const int64_t n=device_read(file->device,buffer,size);
        if (n>0) { __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED); }
        return n;
    }
    int h=file->host;
    if (h<0) return file->dir ? -EISDIR : -EBADF;
    touch_for_write(buffer,size);
    const File *f=get(fd);
    if (f && f->overlay) {
        int64_t n=overlay_pread(f,buffer,size,offset);
        if (n>0) { __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED); }
        observe_read(fd,offset,n);
        return n;
    }
    ssize_t n=pread(h,buffer,size,offset);
    if (n<0) return -errno;
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    observe_read(fd,offset,n);
    return n;
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    const int64_t cost=sos_cost_begin();
    const int64_t n=read_file(fd,buffer,size);
    sos_cost_end(SOS_COST_FILE_READ,cost,n>0 ? (uint64_t)n : 0);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    const int64_t cost=sos_cost_begin();
    const int64_t n=pread_file(fd,buffer,size,offset);
    sos_cost_end(SOS_COST_FILE_READ,cost,n>0 ? (uint64_t)n : 0);
    return n;
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    File *file=get(fd);
    if (!file) {
        if (fd==1 || fd==2) { touch_for_read(buffer,size); console_write(fd,buffer,size); return (int64_t)size; }
        return fd==0 ? -EBADF : runtime_socket_write_file(fd,buffer,size);
    }
    if (file->device) {
        if (file->device==DEVICE_CONSOLE || file->device==DEVICE_STDERR) {
            touch_for_read(buffer,size);
            console_write(file->device==DEVICE_STDERR ? 2 : 1,buffer,size);
        }
        return (int64_t)size;
    }
    int h=file->host;
    if (h<0) return -EBADF;
    touch_for_read(buffer,size);
    ssize_t n=host_write(h,buffer,size);
    if (n<0) return -errno;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    File *file=get(fd);
    if (!file) return -EBADF;
    if (offset<0) return -EINVAL;
    if (file->device) return (int64_t)size;
    int h=file->host;
    if (h<0) return -EBADF;
    touch_for_read(buffer,size);
    ssize_t n=pwrite(h,buffer,size,offset);
    return n<0 ? -errno : n;
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    File *f=get(fd);
    if (!f) return fd>=0 && fd<3 ? -ESPIPE : -EBADF;
    if (f->device) return -ESPIPE;
    if (whence==3 || whence==4) return -ENOTTY;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    if (f->overlay && whence==2) { offset+=(int64_t)f->overlay->length; whence=0; }
    int64_t r=host_lseek(f->host,offset,whence);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int fd,GuestStat *out) {
    File *file=get(fd);
    if (!file) {
        if (fd<0 || fd>=3) return -EBADF;
        if (!out) return -EFAULT;
        device_stat(out); return 0;
    }
    if (!out) return -EFAULT;
    if (file->device) { device_stat(out); return 0; }
    int h=file->host;
    HostStat s;
#ifdef _WIN32
    if (h<0) { /* directory: a listing only, stat the path it was opened with */
        File *f=get(fd); char path[1024];
        if (!f || !f->dir) return -EBADF;
        if (!out) return -EFAULT;
        if (translate(f->path,path,sizeof(path)) || host_stat(path,&s)) return -EBADF;
        convert_stat(&s,out); return 0;
    }
#else
    if (h<0) return -EBADF;
#endif
    if (!out) return -EFAULT;
    if (host_fstat(h,&s)) return -errno;
    convert_stat(&s,out);
    const File *f=get(fd);
    if (f && f->overlay) { out->size=(int64_t)f->overlay->length; out->blocks=(out->size+511)/512; }
    return 0;
}
static int64_t stat_file(const char *guest,GuestStat *out) {
    if (!out) return -EFAULT;
    if (device_kind(guest)) { device_stat(out); return 0; }
    char path[1024]; HostStat s;
    int e=translate(guest,path,sizeof(path));
    if (e) {
        if (is_root(guest)) { directory_stat(out); return 0; }
        return -e;
    }
    if (host_stat(path,&s)) return -errno;
    convert_stat(&s,out);
    const Overlay *o=overlay_of(guest);
    if (o && !S_ISDIR(s.st_mode)) { out->size=(int64_t)o->length; out->blocks=(out->size+511)/512; }
    return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    const int64_t cost=sos_cost_begin();
    const int64_t r=stat_file(guest,out);
    sos_cost_end(SOS_COST_FILE_STAT,cost,0);
    return r;
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (!guest) return -EFAULT;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return is_root(guest) ? (op==0 ? -EEXIST : -EBUSY) : -e;
    HostStat s;
    const int exists=!host_stat(path,&s);
    const int stat_error=exists ? 0 : errno;
    if (op==0) {
        if (exists) return -EEXIST;
        if (game_path(guest)) return -EROFS;
        return mkdir(path,mode ? mode : 0755) ? -errno : 0;
    }
    if (game_path(guest)) return -EROFS;
    if (op==1) {
        char normalized[1100];
        if (!normalize_guest(guest,normalized,sizeof(normalized)) && is_mount_root(normalized)) return -EBUSY;
        if (!exists) return -stat_error;
        if (!S_ISDIR(s.st_mode)) return -ENOTDIR;
        return rmdir(path) ? -errno : 0;
    }
    if (exists && S_ISDIR(s.st_mode)) return -EPERM;
#ifdef _WIN32
    /* POSIX removes a read-only file; the Windows attribute would refuse. */
    const DWORD attributes=GetFileAttributesA(path);
    if (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_READONLY))
        SetFileAttributesA(path,attributes&~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!DeleteFileA(path)) return -compat_errno_from_win32(GetLastError());
    return 0;
#else
    return unlink(path) ? -errno : 0;
#endif
}
#ifdef _WIN32
/* POSIX rename replaces an existing target even while it is open; MoveFileEx refuses that, a POSIX-semantics rename does not. */
static int64_t host_rename_replace(const char *from,const char *to) {
    if (MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING)) return 0;
    const DWORD error=GetLastError();
    if (error!=ERROR_ACCESS_DENIED && error!=ERROR_SHARING_VIOLATION) return -compat_errno_from_win32(error);
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_ACP,0,to,-1,wide,1024)) return -ENAMETOOLONG;
    HANDLE h=CreateFileA(from,DELETE|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if (h==INVALID_HANDLE_VALUE) return -compat_errno_from_win32(GetLastError());
    const size_t length=wcslen(wide)*sizeof(wchar_t);
    const size_t bytes=sizeof(FILE_RENAME_INFO)+length;
    FILE_RENAME_INFO *info=calloc(1,bytes);
    if (!info) { CloseHandle(h); return -ENOMEM; }
    info->Flags=0x1|0x2; /* FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS */
    info->FileNameLength=(DWORD)length;
    memcpy(info->FileName,wide,length);
    const BOOL ok=SetFileInformationByHandle(h,(FILE_INFO_BY_HANDLE_CLASS)22,info,(DWORD)bytes); /* FileRenameInfoEx */
    const DWORD failure=ok ? 0 : GetLastError();
    free(info);
    CloseHandle(h);
    return ok ? 0 : -compat_errno_from_win32(failure);
}
#endif
static int64_t do_rename(const char *from,const char *to) {
    if (!from || !to) return -EFAULT;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
    HostStat source,target;
    if (host_stat(a,&source)) return -errno;
    if (game_path(from) || game_path(to)) return -EROFS;
    const int target_exists=!host_stat(b,&target);
    if (target_exists) {
        const int source_dir=S_ISDIR(source.st_mode), target_dir=S_ISDIR(target.st_mode);
        if (source_dir && !target_dir) return -ENOTDIR;
        if (!source_dir && target_dir) return -EISDIR;
        if (source_dir && rmdir(b)) return errno==ENOTEMPTY || errno==EEXIST ? -ENOTEMPTY : -errno;
    }
#ifdef _WIN32
    return host_rename_replace(a,b);
#else
    return rename(a,b) ? -errno : 0;
#endif
}
static int64_t do_ftruncate(int fd,int64_t length) {
    File *f=get(fd);
    if (!f) return fd>=0 && fd<3 ? -EINVAL : -EBADF;
    if (f->device || f->host<0 || length<0 || !(f->flags&3)) return -EINVAL;
    if (f->overlay) return -EROFS;
#ifdef _WIN32
    return _chsize_s(f->host,length) ? -errno : 0;
#else
    return ftruncate(f->host,length) ? -errno : 0;
#endif
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (!guest) return -EFAULT;
    if (length<0) return -EINVAL;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return is_root(guest) ? -EISDIR : -e;
    HostStat s;
    if (host_stat(path,&s)) return -errno;
    if (S_ISDIR(s.st_mode)) return -EISDIR;
    if (game_path(guest)) return -EROFS;
#ifdef _WIN32
    int h=open(path,O_RDWR|O_BINARY);
    if (h<0) return -errno;
    int r=_chsize_s(h,length) ? -errno : 0;
    close(h);
    return r;
#else
    return truncate(path,length) ? -errno : 0;
#endif
}
/* A descriptor that cannot be flushed (a directory, a read-only file) has nothing to flush. */
static int64_t do_fsync(int fd) {
    File *f=get(fd);
    if (!f) return fd>=0 && fd<3 ? 0 : -EBADF;
    if (f->device || f->host<0) return 0;
    if (fsync(f->host)) { const int e=errno; return e==EBADF || e==EACCES ? 0 : -e; }
    return 0;
}
static int64_t do_access(const char *guest,int mode) {
    if (!guest) return -EFAULT;
    if (mode&~7) return -EINVAL;
    if (device_kind(guest)) return 0;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return is_root(guest) ? ((mode&2) ? -EROFS : 0) : -e;
#ifdef _WIN32
    if (access(path,mode&6)) return -errno; /* X_OK is invalid for the CRT */
#else
    if (access(path,mode&7)) return -errno;
#endif
    return (mode&2) && game_path(guest) ? -EROFS : 0;
}
static int host_set_mode(const char *path,const HostStat *s,uint32_t mode) {
#ifdef _WIN32
    if (S_ISDIR(s->st_mode)) return 0;
    return _chmod(path,(mode&0200) ? (_S_IREAD|_S_IWRITE) : _S_IREAD) ? errno : 0;
#else
    (void)s;
    return chmod(path,(mode_t)(mode&07777)) ? errno : 0;
#endif
}
static int64_t do_chmod(const char *guest,uint32_t mode) {
    if (!guest) return -EFAULT;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return is_root(guest) ? -EROFS : -e;
    HostStat s;
    if (host_stat(path,&s)) return -errno;
    if (game_path(guest)) return -EROFS;
    return -host_set_mode(path,&s,mode);
}
static int64_t do_fchmod(int fd,uint32_t mode) {
    File *f=get(fd);
    if (!f) return fd>=0 && fd<3 ? -EINVAL : -EBADF;
    if (f->device) return -EINVAL;
    char guest[sizeof(f->path)];
    snprintf(guest,sizeof(guest),"%s",f->path);
    return do_chmod(guest,mode);
}
#ifdef _WIN32
static void to_filetime(const GuestTimeval *t,FILETIME *out) {
    const uint64_t ticks=(uint64_t)t->sec*10000000u+(uint64_t)t->usec*10u+116444736000000000ull;
    out->dwLowDateTime=(DWORD)ticks; out->dwHighDateTime=(DWORD)(ticks>>32);
}
#endif
static int host_set_times(const char *path,const GuestTimeval *t) {
#ifdef _WIN32
    HANDLE h=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if (h==INVALID_HANDLE_VALUE) return compat_errno_from_win32(GetLastError());
    FILETIME access,modify;
    if (t) { to_filetime(&t[0],&access); to_filetime(&t[1],&modify); }
    else { GetSystemTimeAsFileTime(&access); modify=access; }
    const int e=SetFileTime(h,NULL,&access,&modify) ? 0 : compat_errno_from_win32(GetLastError());
    CloseHandle(h);
    return e;
#else
    struct timeval tv[2];
    if (t) for (int i=0;i<2;++i) { tv[i].tv_sec=(time_t)t[i].sec; tv[i].tv_usec=(suseconds_t)t[i].usec; }
    return utimes(path,t ? tv : NULL) ? errno : 0;
#endif
}
static int64_t do_utimes(const char *guest,const GuestTimeval *t) {
    if (!guest) return -EFAULT;
    if (t && (t[0].usec<0 || t[0].usec>=1000000 || t[1].usec<0 || t[1].usec>=1000000)) return -EINVAL;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return is_root(guest) ? -EROFS : -e;
    HostStat s;
    if (host_stat(path,&s)) return -errno;
    if (game_path(guest)) return -EROFS;
    return -host_set_times(path,t);
}
static int64_t do_futimes(int fd,const GuestTimeval *t) {
    File *f=get(fd);
    if (!f) return fd>=0 && fd<3 ? -EINVAL : -EBADF;
    if (f->device) return -EINVAL;
    char guest[sizeof(f->path)];
    snprintf(guest,sizeof(guest),"%s",f->path);
    return do_utimes(guest,t);
}
static int64_t do_flock(int fd,int operation) {
    (void)operation;
    return get(fd) || (fd>=0 && fd<3) ? 0 : -EBADF;
}
/* Vectored forms transfer the vectors in order and stop at the first short transfer. */
static int64_t vector_io(int fd,const GuestIovec *iov,int count,int64_t offset,int positioned,int writing) {
    if (count<0 || count>1024 || (count && !iov) || (positioned && offset<0)) return -EINVAL;
    int64_t total=0;
    for (int i=0;i<count;++i) {
        if (!iov[i].length) continue;
        int64_t n;
        if (writing) n=positioned ? do_pwrite(fd,iov[i].base,iov[i].length,offset+total) : do_write(fd,iov[i].base,iov[i].length);
        else n=positioned ? do_pread(fd,iov[i].base,iov[i].length,offset+total) : do_read(fd,iov[i].base,iov[i].length);
        if (n<0) return total ? total : n;
        total+=n;
        if ((uint64_t)n<iov[i].length) break;
    }
    return total;
}
static int64_t do_readv(int fd,const GuestIovec *iov,int count) { return vector_io(fd,iov,count,0,0,0); }
static int64_t do_writev(int fd,const GuestIovec *iov,int count) { return vector_io(fd,iov,count,0,0,1); }
static int64_t do_preadv(int fd,const GuestIovec *iov,int count,int64_t offset) { return vector_io(fd,iov,count,offset,1,0); }
static int64_t do_pwritev(int fd,const GuestIovec *iov,int count,int64_t offset) { return vector_io(fd,iov,count,offset,1,1); }

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *p,int f,int m),(p,f,m))
PAIR(close,(int fd),(fd))
PAIR(read,(int fd,void *b,uint64_t n),(fd,b,n))
PAIR(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(write,(int fd,const void *b,uint64_t n),(fd,b,n))
PAIR(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(lseek,(int fd,int64_t o,int w),(fd,o,w))
PAIR(fstat,(int fd,GuestStat *s),(fd,s))
PAIR(stat,(const char *p,GuestStat *s),(p,s))
PAIR(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
PAIR(rename,(const char *a,const char *b),(a,b))
PAIR(ftruncate,(int fd,int64_t l),(fd,l))
PAIR(truncate,(const char *p,int64_t l),(p,l))
PAIR(fsync,(int fd),(fd))
PAIR(chmod,(const char *p,uint32_t m),(p,m))
PAIR(fchmod,(int fd,uint32_t m),(fd,m))
PAIR(utimes,(const char *p,const GuestTimeval *t),(p,t))
PAIR(futimes,(int fd,const GuestTimeval *t),(fd,t))
PAIR(readv,(int fd,const GuestIovec *v,int n),(fd,v,n))
PAIR(writev,(int fd,const GuestIovec *v,int n),(fd,v,n))
PAIR(preadv,(int fd,const GuestIovec *v,int n,int64_t o),(fd,v,n,o))
PAIR(pwritev,(int fd,const GuestIovec *v,int n,int64_t o),(fd,v,n,o))
static ABI int64_t posix_flock(int fd,int op) { return posix(do_flock(fd,op)); }
static ABI int64_t posix_fcntl(int fd,int command,intptr_t argument) {
    return posix(do_fcntl(fd,command,argument));
}
static ABI int64_t posix_access(const char *p,int m) { return posix(do_access(p,m)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t posix_mkdir(const char *p,int m) { return posix(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t posix_rmdir(const char *p) { return posix(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }
static ABI int64_t posix_unlink(const char *p) { return posix(path_op(p,2,0)); }
static ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s; return (int32_t)sce(do_stat(p,&s));
}
static ABI int64_t sce_kernel_getdents(int fd,char *b,uint64_t n) {
    return sce(do_getdents(fd,b,n,NULL));
}

static ABI int32_t kernel_sync(void) {
    pthread_mutex_lock(&lock);
    for (int i=3;i<MAX_FILES;++i)
        if (files[i].used && !files[i].dir && files[i].host>=0) fsync(files[i].host);
    pthread_mutex_unlock(&lock);
    return 0;
}
static const RuntimeExport exports[]={
    {"sceKernelSync",kernel_sync}, {"sync",kernel_sync},
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat}, {"lstat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelGetdents",sce_kernel_getdents},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"sceKernelFdatasync",sce_fsync}, {"fdatasync",posix_fsync},
    {"sceKernelChmod",sce_chmod}, {"chmod",posix_chmod},
    {"sceKernelFchmod",sce_fchmod}, {"fchmod",posix_fchmod},
    {"sceKernelUtimes",sce_utimes}, {"utimes",posix_utimes},
    {"sceKernelFutimes",sce_futimes}, {"futimes",posix_futimes},
    {"sceKernelReadv",sce_readv}, {"readv",posix_readv},
    {"sceKernelWritev",sce_writev}, {"writev",posix_writev},
    {"sceKernelPreadv",sce_preadv}, {"preadv",posix_preadv},
    {"sceKernelPwritev",sce_pwritev}, {"pwritev",posix_pwritev},
    {"flock",posix_flock},
    {"fcntl",posix_fcntl},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int64_t runtime_file_write(int fd,const void *b,uint64_t n) { return do_write(fd,b,n); }
int64_t runtime_file_lseek(int fd,int64_t o,int w) { return do_lseek(fd,o,w); }
int64_t runtime_file_stat(const char *p,void *s) { return do_stat(p,s); }
int64_t runtime_file_fstat(int fd,void *s) { return do_fstat(fd,s); }
int64_t runtime_file_getdents(int fd,char *b,uint64_t n,int64_t *base) { return do_getdents(fd,b,n,base); }
int runtime_file_is_open(int fd) { return (fd>=0 && fd<3) || get(fd)!=NULL; }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
    pthread_mutex_lock(&unmapped_lock);
    if (unmapped_count) {
        printf("Runtime: unmapped guest prefixes=%zu\n",unmapped_count);
        for (size_t i=0;i<unmapped_count;++i)
            printf("  %s x%u\n",unmapped_prefixes[i].prefix,unmapped_prefixes[i].count);
    }
    pthread_mutex_unlock(&unmapped_lock);
}
