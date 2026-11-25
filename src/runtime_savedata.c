/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched.
 *
 * As on the console, a writable mount is committed when it is unmounted: the guest works on <dir>.sos-work, and the
 * unmount swaps it in. <dir>.sos-commit marks a work copy as complete, so an interrupted swap finishes at the next
 * mount, and a work copy without it (the game stopped before unmounting) is dropped. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ftw.h>
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_BUSY ((int32_t)0x809F0003)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define ERR_EVENT_BUSY ((int32_t)0x809F0018)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16
#define BACKUP_BUSY_MS 500
#define NOTE_SAVE(api) runtime_history_note(api,"libSceSaveData",(uintptr_t)__builtin_return_address(0))
#define WORK_SUFFIX ".sos-work"
#define OLD_SUFFIX ".sos-old"
#define COMMIT_SUFFIX ".sos-commit"
#define TEMP_SUFFIX ".sos-tmp"
#define BACKUP_SUFFIX ".sos-backup"
#define BACKUP_TMP_SUFFIX ".sos-backup-tmp"
#define BACKUP_OLD_SUFFIX ".sos-backup-old"

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
typedef struct { uint32_t type; int32_t error; int32_t user; uint32_t pad; char title[16]; char dir[32]; uint8_t reserved[40]; } Event;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint8_t reserved[32]; } Backup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; Param *param; Icon *icon;
                 uint8_t reserved[32]; } CheckBackup;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Event)==104,"OrbisSaveDataEvent layout");
_Static_assert(sizeof(Backup)==64 && sizeof(CheckBackup)==72,"OrbisSaveDataBackup layouts");
#define PARAM_ALL 0
#define PARAM_TITLE 1
#define PARAM_SUBTITLE 2
#define PARAM_DETAIL 3
#define PARAM_USER 4
#define PARAM_MTIME 5
#define EVENT_UMOUNT_BACKUP 1
#define EVENT_BACKUP 2
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used, writable; int32_t user; char title[16]; char host[700], work[720], meta[700], name[33]; uint64_t blocks; } slots[SLOTS];
static size_t mounts_done, commits_done, memory_writes;
static int backup_progress;
typedef struct { uint32_t type; int32_t error; int32_t user; uint64_t announced; char title[16], name[33]; } SaveEvent;
static SaveEvent events[4];
static unsigned event_head, event_count;

void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
    const char *user=runtime_file_user_dir(), *name=user;
    for (const char *p=user;*p;++p) if ((*p=='/' || *p=='\\') && p[1]) name=p+1;
    printf("Save: profile %s, title %s\n",name,title_id);
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
static int is_dir(const char *path) { struct stat st; return !stat(path,&st) && S_ISDIR(st.st_mode); }
static int is_file(const char *path) { struct stat st; return !stat(path,&st) && S_ISREG(st.st_mode); }
static int ends_with(const char *s, const char *suffix) {
    size_t n=strlen(s), k=strlen(suffix);
    return n>=k && !strcmp(s+n-k,suffix);
}
static int internal_name(const char *name) {
    return ends_with(name,WORK_SUFFIX) || ends_with(name,OLD_SUFFIX) || ends_with(name,COMMIT_SUFFIX) ||
           ends_with(name,TEMP_SUFFIX) || ends_with(name,BACKUP_SUFFIX) || ends_with(name,BACKUP_TMP_SUFFIX) ||
           ends_with(name,BACKUP_OLD_SUFFIX);
}
#ifdef _WIN32
static void remove_tree(const char *path) { compat_remove_tree(path); }
static int replace_file(const char *from, const char *to) {
    return MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}
static void sync_dir(const char *path) { (void)path; } /* MOVEFILE_WRITE_THROUGH flushes the rename */
#else
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
static void remove_tree(const char *path) { nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
static int replace_file(const char *from, const char *to) { return rename(from,to); }
static void sync_dir(const char *path) {
    int fd=open(path,O_RDONLY);
    if (fd>=0) { fsync(fd); close(fd); }
}
#endif
static void parent_of(const char *path, char *out, size_t size) {
    snprintf(out,size,"%s",path);
    char *slash=strrchr(out,'/');
    if (slash && slash!=out) *slash=0;
}
static int sync_file(const char *path) {
    int fd=open(path,O_RDWR);
    if (fd<0) return errno==EACCES ? 0 : -1; /* a read-only file was copied, not written */
    int r=fsync(fd);
    close(fd);
    return r;
}
/* Temporary file, flushed to disk, then renamed over `path`: a reader sees the old file or the new one. */
static int write_file_atomic(const char *path, const void *data, size_t size) {
    char temp[760], dir[760];
    snprintf(temp,sizeof(temp),"%s" TEMP_SUFFIX,path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fwrite(data,1,size,f)==size && !fflush(f) && !fsync(fileno(f));
    ok&=!fclose(f);
    if (!ok || replace_file(temp,path)) { remove(temp); return -1; }
    parent_of(path,dir,sizeof(dir));
    sync_dir(dir);
    return 0;
}
static int copy_file(const char *from, const char *to) {
    FILE *in=fopen(from,"rb");
    if (!in) return -1;
    FILE *out=fopen(to,"wb");
    if (!out) { fclose(in); return -1; }
    char buffer[65536];
    size_t n;
    int ok=1;
    while (ok && (n=fread(buffer,1,sizeof(buffer),in))>0) ok=fwrite(buffer,1,n,out)==n;
    ok&=!ferror(in);
    fclose(in);
    ok&=!fflush(out) && !fsync(fileno(out));
    ok&=!fclose(out);
    return ok ? 0 : -1;
}
static int copy_tree(const char *from, const char *to) {
    if (make_dirs(to)) return -1;
    DIR *d=opendir(from);
    if (!d) return -1;
    int r=0;
    for (struct dirent *e;!r && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char a[1100], b[1100];
        snprintf(a,sizeof(a),"%s/%s",from,e->d_name);
        snprintf(b,sizeof(b),"%s/%s",to,e->d_name);
        r=is_dir(a) ? copy_tree(a,b) : copy_file(a,b);
    }
    closedir(d);
    return r;
}
static int sync_tree(const char *path) {
    DIR *d=opendir(path);
    if (!d) return -1;
    int r=0;
    for (struct dirent *e;!r && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1100];
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        r=is_dir(child) ? sync_tree(child) : sync_file(child);
    }
    closedir(d);
    sync_dir(path);
    return r;
}
static void sibling(const char *host, const char *suffix, char *out, size_t size) { snprintf(out,size,"%s%s",host,suffix); }
static const char *leaf(const char *path) { const char *slash=strrchr(path,'/'); return slash ? slash+1 : path; }

/* Finishes a committed swap or drops an uncommitted work copy left by a stopped process. -1: a committed swap is
 * still pending (folder in use) and the save must not be used yet. */
static int recover(const char *host) {
    char work[720], old[720], marker[720];
    sibling(host,WORK_SUFFIX,work,sizeof(work));
    sibling(host,OLD_SUFFIX,old,sizeof(old));
    sibling(host,COMMIT_SUFFIX,marker,sizeof(marker));
    if (is_file(marker)) {
        if (is_dir(work)) {
            if (is_dir(host)) {
                if (is_dir(old)) remove_tree(old);
                if (rename(host,old)) return -1;
            }
            if (rename(work,host)) { if (!is_dir(host) && is_dir(old)) rename(old,host); return -1; }
            printf("Save: finished the interrupted save of '%s'\n",leaf(host));
        }
        remove(marker);
        if (is_dir(old)) remove_tree(old);
        return 0;
    }
    if (is_dir(work)) {
        remove_tree(work);
        printf("Save: dropped unsaved changes of '%s' (the game stopped before saving)\n",leaf(host));
    }
    if (is_dir(old)) { if (is_dir(host)) remove_tree(old); else rename(old,host); }
    return 0;
}
/* The work copy is flushed, marked complete, then renamed in place of the save. 0: done; 1: complete but not swapped
 * in yet (the next mount finishes it); -1: nothing saved, the previous save stays. */
static int commit(const char *host, const char *work) {
    char old[720], marker[720], dir[720];
    sibling(host,OLD_SUFFIX,old,sizeof(old));
    sibling(host,COMMIT_SUFFIX,marker,sizeof(marker));
    parent_of(host,dir,sizeof(dir));
    if (sync_tree(work) || write_file_atomic(marker,"",0)) { remove(marker); remove_tree(work); return -1; }
    if (is_dir(old)) remove_tree(old);
    if (is_dir(host) && rename(host,old)) return 1;
    if (rename(work,host)) { rename(old,host); return 1; }
    sync_dir(dir);
    remove(marker);
    remove_tree(old);
    return 0;
}

/* A complete copy of the committed save, swapped in only after every file was flushed. */
static int backup_save(const char *host) {
    char backup[720], tmp[740], old[740], dir[720], meta[700], meta_tmp[760];
    sibling(host,BACKUP_SUFFIX,backup,sizeof(backup));
    sibling(host,BACKUP_TMP_SUFFIX,tmp,sizeof(tmp));
    sibling(host,BACKUP_OLD_SUFFIX,old,sizeof(old));
    parent_of(host,dir,sizeof(dir));
    snprintf(meta,sizeof(meta),"%s.sce_sys",host);
    snprintf(meta_tmp,sizeof(meta_tmp),"%s/.sce_sys",tmp);
    remove_tree(tmp);
    if (copy_tree(host,tmp) || (is_dir(meta) && copy_tree(meta,meta_tmp)) || sync_tree(tmp)) {
        remove_tree(tmp);
        return -1;
    }
    if (is_dir(old)) remove_tree(old);
    const int had_backup=is_dir(backup);
    if (had_backup && rename(backup,old)) { remove_tree(tmp); return -1; }
    if (rename(tmp,backup)) {
        if (had_backup && !is_dir(backup) && is_dir(old)) rename(old,backup);
        remove_tree(tmp);
        return -1;
    }
    sync_dir(dir);
    if (is_dir(old)) remove_tree(old);
    return 0;
}
static void event_push(uint32_t type,int32_t error,int32_t user,const char *title,const char *name) {
    if (event_count==sizeof(events)/sizeof(*events)) {
        event_head=(event_head+1)%(sizeof(events)/sizeof(*events));
        --event_count;
    }
    SaveEvent *event=&events[(event_head+event_count)%(sizeof(events)/sizeof(*events))];
    memset(event,0,sizeof(*event));
    event->type=type; event->error=error; event->user=user;
    snprintf(event->title,sizeof(event->title),"%s",title ? title : "");
    snprintf(event->name,sizeof(event->name),"%s",name ? name : "");
    ++event_count;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    Param copy=*p; copy.mtime=time(NULL);
    if (write_file_atomic(path,&copy,sizeof(copy))) printf("Save: cannot write %s\n",path);
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}

/* A save's param (title, subtitle, date) for the save dialog; 0 when it exists. */
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    if (!valid_name(dir,32)) return -1;
    char base[600], meta[680];
    root(user,NULL,base,sizeof(base));
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir);
    return read_param(meta,(Param *)param);
}
/* Host path of a save's icon0.png (the location picture in the save dialog); 0 when named. */
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) {
    if (!valid_name(dir,32)) return -1;
    char base[600];
    root(user,NULL,base,sizeof(base));
    snprintf(out,size,"%s/%s.sce_sys/icon0.png",base,dir);
    return 0;
}
static ABI int32_t save_initialize(const void *param) { NOTE_SAVE("sceSaveDataInitialize3"); (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i) if (slots[i].used) { pthread_mutex_unlock(&lock); return ERR_BUSY; }
    pthread_mutex_unlock(&lock);
    initialized=0; return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint64_t blocks, uint32_t mode, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data)) || internal_name(dir->data)) return ERR_PARAMETER;
    char base[600], host[640], meta[680], work[720];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    sibling(host,WORK_SUFFIX,work,sizeof(work));
    const int writable=!(mode & MODE_RDONLY);
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (recover(host)) {
        pthread_mutex_unlock(&lock);
        printf("Save: '%s' is still being swapped in (folder in use)\n",dir->data);
        return ERR_INTERNAL;
    }
    const int exists=is_dir(host);
    if ((mode & MODE_CREATE) && exists) { pthread_mutex_unlock(&lock); return ERR_EXISTS; }
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    if (writable && copy_tree(host,work)) {
        remove_tree(work);
        pthread_mutex_unlock(&lock);
        printf("Save: cannot prepare '%s' for writing\n",dir->data);
        return ERR_INTERNAL;
    }
    slots[slot].used=1;
    slots[slot].writable=writable;
    slots[slot].user=user;
    snprintf(slots[slot].title,sizeof(slots[slot].title),"%s",title && *title ? title : title_id);
    slots[slot].blocks=blocks;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].work,sizeof(slots[slot].work),"%s",writable ? work : host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    snprintf(slots[slot].name,sizeof(slots[slot].name),"%s",dir->data);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    if (runtime_file_mount(result->point.data,slots[slot].work)) {
        slots[slot].used=0;
        if (writable) remove_tree(work);
        pthread_mutex_unlock(&lock);
        printf("Save: cannot map '%s' (path too long or too many mounts)\n",dir->data);
        return ERR_INTERNAL;
    }
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",writable ? "" : ", read-only");
    return 0;
}
static int32_t mount_logged(int32_t user, const char *title, const DirName *dir, uint64_t blocks, uint32_t mode, MountResult *result) {
    const int32_t r=mount(user,title,dir,blocks,mode,result);
    if (r) {
        char name[33];
        snprintf(name,sizeof(name),"%s",dir && valid_name(dir->data,sizeof(dir->data)) ? dir->data : "(unnamed)");
        printf("Runtime: save data mount '%s' failed (error=%#x, mode=%#x)\n",name,(unsigned)r,mode);
    }
    return r;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount_logged(m->user,m->title ? m->title->data : NULL,m->dir,m->blocks,m->mode,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount_logged(m->user,NULL,m->dir,m->blocks,m->mode,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static int32_t umount(const MountPoint *point,int with_backup) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    runtime_file_unmount(point->data);
    const int writable=slots[slot].writable;
    const int32_t user=slots[slot].user;
    char host[700], name[33], title[16];
    snprintf(host,sizeof(host),"%s",slots[slot].host);
    snprintf(name,sizeof(name),"%s",slots[slot].name);
    snprintf(title,sizeof(title),"%s",slots[slot].title);
    int committed=0, pending=0;
    if (writable) {
        committed=commit(host,slots[slot].work);
        pending=committed>0;
        if (committed>=0) ++commits_done;
    }
    slots[slot].used=0;
    int32_t result=committed<0 ? ERR_INTERNAL : 0;
    int backup_error=0;
    if (with_backup) {
        backup_progress=0;
        if (committed<0) backup_error=ERR_INTERNAL;
        else {
            if (pending && recover(host)) backup_error=ERR_INTERNAL;
            else {
                if (pending) committed=0;
                pending=0;
                if (backup_save(host)) backup_error=ERR_INTERNAL;
            }
        }
        backup_progress=100;
        event_push(EVENT_UMOUNT_BACKUP,backup_error,user,title,name);
        if (backup_error) result=ERR_INTERNAL;
    }
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data unmounted from %s\n",point->data);
    if (writable) {
        if (committed==0) printf("Save: '%s' written\n",name);
        else if (committed>0) printf("Save: '%s' written; its folder is swapped in at the next mount\n",name);
        else printf("Save: '%s' could not be written; the previous save is kept\n",name);
    }
    if (with_backup && backup_error) printf("Save: backup of '%s' failed; the save itself is kept\n",name);
    return result;
}
static ABI int32_t save_umount(const MountPoint *point) { return umount(point,0); }
static ABI int32_t save_umount_with_backup(const MountPoint *point) { return umount(point,1); }
#define BLOCK_SIZE 32768
static uint64_t used_blocks(const char *dir) {
    uint64_t bytes=0;
    DIR *d=opendir(dir);
    if (!d) return 0;
    for (struct dirent *e;(e=readdir(d));) {
        char path[1100]; struct stat st;
        snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);
        if (strcmp(e->d_name,".") && strcmp(e->d_name,"..") && !stat(path,&st) && S_ISREG(st.st_mode)) bytes+=(uint64_t)st.st_size;
    }
    closedir(d);
    return (bytes+BLOCK_SIZE-1)/BLOCK_SIZE;
}
static ABI int32_t save_get_mount_info(const MountPoint *point, SearchInfo *info) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!info) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    uint64_t used=used_blocks(slots[slot].work), total=slots[slot].blocks>used ? slots[slot].blocks : used;
    pthread_mutex_unlock(&lock);
    memset(info,0,sizeof(*info));
    info->blocks=total; info->free_blocks=total-used;
    return 0;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    if (!slots[slot].writable) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0 && !slots[slot].writable) r=ERR_BAD_MOUNTED;
    else if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        r=write_file_atomic(path,icon->buffer,icon->data_size<icon->buffer_size ? icon->data_size : icon->buffer_size) ? ERR_INTERNAL : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_get_param(const MountPoint *point,uint32_t type,void *buffer,uint64_t size,uint64_t *got) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !buffer || type>PARAM_MTIME) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p;
    read_param(slots[slot].meta,&p);
    uint64_t written=0;
    int32_t r=0;
    switch (type) {
    case PARAM_ALL:
        if (size<sizeof(p)) { r=ERR_PARAMETER; break; }
        memcpy(buffer,&p,sizeof(p)); written=sizeof(p); break;
    case PARAM_TITLE: case PARAM_SUBTITLE: case PARAM_DETAIL: {
        if (!size) { r=ERR_PARAMETER; break; }
        const char *src=type==PARAM_TITLE ? p.title : type==PARAM_SUBTITLE ? p.subtitle : p.detail;
        const size_t limit=type==PARAM_TITLE ? sizeof(p.title) : type==PARAM_SUBTITLE ? sizeof(p.subtitle) : sizeof(p.detail);
        size_t length=strnlen(src,limit);
        if (length>=size) length=size-1;
        memcpy(buffer,src,length);
        ((char *)buffer)[length]=0;
        written=length+1;
        break;
    }
    case PARAM_USER:
        if (size<sizeof(p.user_param)) { r=ERR_PARAMETER; break; }
        memcpy(buffer,&p.user_param,sizeof(p.user_param)); written=sizeof(p.user_param); break;
    case PARAM_MTIME:
        if (size<sizeof(p.mtime)) { r=ERR_PARAMETER; break; }
        memcpy(buffer,&p.mtime,sizeof(p.mtime)); written=sizeof(p.mtime); break;
    }
    pthread_mutex_unlock(&lock);
    if (!r && got) *got=written;
    return r;
}
static ABI int32_t save_load_icon(const MountPoint *point,Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    char path[700];
    snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
    FILE *f=fopen(path,"rb");
    if (!f) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    fseek(f,0,SEEK_END);
    const long length=ftell(f);
    rewind(f);
    const uint64_t file_size=length>0 ? (uint64_t)length : 0;
    const size_t take=file_size>icon->buffer_size ? (size_t)icon->buffer_size : (size_t)file_size;
    const size_t read=take ? fread((void *)icon->buffer,1,take,f) : 0;
    const int ok=!ferror(f) && read==take;
    fclose(f);
    pthread_mutex_unlock(&lock);
    if (!ok) return ERR_INTERNAL;
    ((Icon *)icon)->data_size=file_size;
    return 0;
}
static ABI int32_t save_get_progress(float *progress) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!progress) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    *progress=(float)backup_progress/100.0f;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t save_clear_progress(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    backup_progress=0;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* A backup outlives the unmount that starts it. Games lose a notification that comes with the unmount, and the unmount
 * itself can repeat a running poll, so the first poll and those right after it are busy. */
static ABI int32_t save_get_event_result(const void *param,Event *event) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!event || param) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!event_count) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    const uint64_t now=runtime_monotonic_ms();
    if (!events[event_head].announced || now-events[event_head].announced<BACKUP_BUSY_MS) {
        for (unsigned i=0;i<event_count;++i) {
            SaveEvent *event_at=&events[(event_head+i)%(sizeof(events)/sizeof(*events))];
            if (!event_at->announced) event_at->announced=now;
        }
        pthread_mutex_unlock(&lock);
        return ERR_EVENT_BUSY;
    }
    const SaveEvent source=events[event_head];
    event_head=(event_head+1)%(sizeof(events)/sizeof(*events));
    --event_count;
    pthread_mutex_unlock(&lock);
    memset(event,0,sizeof(*event));
    event->type=source.type; event->error=source.error; event->user=source.user;
    memcpy(event->title,source.title,sizeof(event->title));
    memcpy(event->dir,source.name,sizeof(event->dir));
    if (!source.error) printf("Save: backup of '%s' finished\n",source.name);
    return 0;
}
static ABI int32_t save_check_backup(const CheckBackup *c) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!c || !c->dir || !valid_name(c->dir->data,sizeof(c->dir->data)) || internal_name(c->dir->data))
        return ERR_PARAMETER;
    if (c->user<0) return ERR_INVALID_USER;
    char base[600], host[640], backup[720], meta[760];
    root(c->user,c->title ? c->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,c->dir->data);
    snprintf(backup,sizeof(backup),"%s%s",host,BACKUP_SUFFIX);
    snprintf(meta,sizeof(meta),"%s/.sce_sys",backup);
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i)
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BUSY; }
    if (!is_dir(backup)) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    if (c->param) read_param(meta,c->param);
    if (c->icon && c->icon->buffer) {
        char path[780];
        snprintf(path,sizeof(path),"%s/icon0.png",meta);
        FILE *f=fopen(path,"rb");
        if (f) {
            fseek(f,0,SEEK_END);
            const long length=ftell(f);
            rewind(f);
            const uint64_t size=length>0 ? (uint64_t)length : 0;
            const size_t take=size>c->icon->buffer_size ? (size_t)c->icon->buffer_size : (size_t)size;
            const size_t got=take ? fread((void *)c->icon->buffer,1,take,f) : 0;
            const int ok=!ferror(f) && got==take;
            fclose(f);
            if (!ok) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
            c->icon->data_size=size;
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t save_backup(const Backup *b) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!b || !b->dir || !valid_name(b->dir->data,sizeof(b->dir->data)) || internal_name(b->dir->data)) return ERR_PARAMETER;
    if (b->user<0) return ERR_INVALID_USER;
    char base[600], host[640];
    root(b->user,b->title ? b->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,b->dir->data);
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i)
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BUSY; }
    if (!is_dir(host)) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    backup_progress=0;
    const int32_t r=backup_save(host) ? ERR_INTERNAL : 0;
    backup_progress=100;
    event_push(EVENT_BACKUP,r,b->user,b->title ? b->title->data : title_id,b->dir->data);
    pthread_mutex_unlock(&lock);
    if (r) printf("Save: backup of '%s' failed\n",b->dir->data);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data)) || internal_name(d->dir->data)) return ERR_PARAMETER;
    char base[600], host[640], meta[680], leftover[720];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i)
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
    if (recover(host)) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!is_dir(host)) { pthread_mutex_unlock(&lock); return ERR_NOT_FOUND; }
    remove_tree(host);
    remove_tree(meta);
    sibling(host,WORK_SUFFIX,leftover,sizeof(leftover));
    if (is_dir(leftover)) remove_tree(leftover);
    sibling(host,BACKUP_SUFFIX,leftover,sizeof(leftover));
    if (is_dir(leftover)) remove_tree(leftover);
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=32 || ends_with(e->d_name,".sce_sys") || internal_name(e->d_name)) continue;
        if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
        if (count==capacity) {
            size_t next_cap=capacity ? capacity*2 : 16;
            Entry *grown=realloc(entries,next_cap*sizeof(*entries));
            if (!grown) { if (d) closedir(d); free(entries); return ERR_INTERNAL; }
            entries=grown; capacity=next_cap;
        }
        snprintf(entries[count].name,32,"%s",e->d_name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
        read_param(meta,&entries[count].param);
        ++count;
    }
    if (d) closedir(d);
    if (count) {
        pthread_mutex_lock(&lock);
        sort_key=cond->key; sort_order=cond->order;
        qsort(entries,count,sizeof(*entries),compare);
        pthread_mutex_unlock(&lock);
    }
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title. */
typedef struct { void *buffer; uint64_t size; int64_t offset; uint8_t reserved[40]; } MemoryData;
typedef struct { int32_t user; int32_t pad; MemoryData *data; Param *param; Icon *icon;
                 uint32_t slot; uint8_t reserved[28]; } MemoryGet2;
typedef struct { int32_t user; int32_t pad; const MemoryData *data; const Param *param; const Icon *icon;
                 uint32_t data_count, slot; uint8_t reserved[32]; } MemorySet2;
typedef struct { uint32_t option; int32_t user; uint64_t size, icon_size; const Param *init_param;
                 const Icon *init_icon; uint32_t slot; uint8_t reserved[20]; } MemorySetup2;
typedef struct { uint64_t existed; uint8_t reserved[16]; } MemorySetupResult;
typedef struct { int32_t user; uint32_t slot, option; uint8_t reserved[28]; } MemorySync;
_Static_assert(sizeof(MemoryData)==64 && sizeof(MemoryGet2)==64 && sizeof(MemorySet2)==72,
               "SaveDataMemory2 request layouts");
_Static_assert(sizeof(MemorySetup2)==64 && sizeof(MemorySetupResult)==24 && sizeof(MemorySync)==40,
               "SaveDataMemory2 setup layouts");
#define MEMORY_SLOTS 16
static struct { char path[720]; uint64_t size; } memory[MEMORY_SLOTS];
static void memory_dir(int32_t user,uint32_t slot,char *out,size_t size) {
    char base[600];
    root(user,NULL,base,sizeof(base));
    if (slot) snprintf(out,size,"%s.memory%u",base,slot);
    else snprintf(out,size,"%s.memory",base);
}
static int32_t memory_setup_impl(int32_t user, uint32_t slot, uint64_t size, const Param *param, const Icon *icon, uint64_t *existed) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size || slot>=MEMORY_SLOTS) return ERR_PARAMETER;
    char dir[640];
    memory_dir(user,slot,dir,sizeof(dir));
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    snprintf(memory[slot].path,sizeof(memory[slot].path),"%s/memory.dat",dir);
    FILE *f=fopen(memory[slot].path,"r+b");
    if (!f) f=fopen(memory[slot].path,"w+b");
    int32_t r=ERR_INTERNAL;
    uint64_t current=0;
    if (f) {
        fseek(f,0,SEEK_END);
        current=(uint64_t)ftell(f);
        if (current<size && ftruncate(fileno(f),(off_t)size)) goto done;
        memory[slot].size=current>size ? current : size; r=0;
        if (existed) *existed=current;
    done:
        fclose(f);
    }
    if (!r && !current) {
        if (param) write_param(dir,param);
        if (icon && icon->buffer && icon->buffer_size) {
            char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",dir);
            write_file_atomic(path,icon->buffer,icon->buffer_size);
        }
    }
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory ready (%llu bytes, slot %u)\n",(unsigned long long)size,slot);
    return r;
}
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    uint64_t existed=0;
    const int32_t r=memory_setup_impl(user,0,size,NULL,NULL,&existed);
    if (!r && param) {
        char dir[640];
        memory_dir(user,0,dir,sizeof(dir));
        pthread_mutex_lock(&lock);
        write_param(dir,param);
        pthread_mutex_unlock(&lock);
    }
    return r;
}
/* Reads stop at the end of the memory; writes past it extend it. */
static int32_t memory_io(uint32_t slot, void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (slot>=MEMORY_SLOTS) return ERR_PARAMETER;
    if (!buffer || offset<0) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memory[slot].size) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if (!write) {
        if ((uint64_t)offset>=memory[slot].size) size=0;
        else if ((uint64_t)offset+size>memory[slot].size) size=memory[slot].size-(uint64_t)offset;
    }
    FILE *f=fopen(memory[slot].path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,(long)offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) {
        ++memory_writes;
        if ((uint64_t)offset+size>memory[slot].size) memory[slot].size=(uint64_t)offset+size;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(0,buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(0,buffer,size,offset,1); }
static ABI int32_t memory_setup2(const MemorySetup2 *setup, MemorySetupResult *result) {
    NOTE_SAVE("sceSaveDataSetupSaveDataMemory2");
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!setup) return ERR_PARAMETER;
    uint64_t existed=0;
    const int32_t r=memory_setup_impl(setup->user,setup->slot,setup->size,setup->init_param,setup->init_icon,&existed);
    if (r) return r;
    if (result) { memset(result,0,sizeof(*result)); result->existed=existed; }
    return 0;
}
static int32_t memory_slot_ready(uint32_t slot) {
    if (slot>=MEMORY_SLOTS) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int ready=memory[slot].size!=0;
    pthread_mutex_unlock(&lock);
    return ready ? 0 : ERR_MEMORY_NOT_READY;
}
static ABI int32_t memory_get2(const MemoryGet2 *get) {
    NOTE_SAVE("sceSaveDataGetSaveDataMemory2");
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!get) return ERR_PARAMETER;
    int32_t r=memory_slot_ready(get->slot);
    if (r) return r;
    if (get->data && (r=memory_io(get->slot,get->data->buffer,get->data->size,get->data->offset,0))) return r;
    char dir[640];
    memory_dir(get->user,get->slot,dir,sizeof(dir));
    if (get->param) {
        pthread_mutex_lock(&lock);
        const int stored=!read_param(dir,get->param);
        pthread_mutex_unlock(&lock);
        if (!stored) memset(get->param,0,sizeof(*get->param));
    }
    if (get->icon) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",dir);
        uint64_t got=0;
        pthread_mutex_lock(&lock);
        FILE *f=get->icon->buffer ? fopen(path,"rb") : NULL;
        if (f) { got=fread((void *)get->icon->buffer,1,get->icon->buffer_size,f); fclose(f); }
        pthread_mutex_unlock(&lock);
        ((Icon *)get->icon)->data_size=got;
    }
    return 0;
}
static ABI int32_t memory_set2(const MemorySet2 *set) {
    NOTE_SAVE("sceSaveDataSetSaveDataMemory2");
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!set) return ERR_PARAMETER;
    int32_t r=memory_slot_ready(set->slot);
    if (r) return r;
    if (set->data) {
        const uint32_t count=set->data_count>1 ? set->data_count : 1;
        for (uint32_t i=0;i<count;++i)
            if ((r=memory_io(set->slot,set->data[i].buffer,set->data[i].size,set->data[i].offset,1))) return r;
    }
    char dir[640];
    memory_dir(set->user,set->slot,dir,sizeof(dir));
    pthread_mutex_lock(&lock);
    if (set->param) write_param(dir,set->param);
    if (set->icon && set->icon->buffer) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",dir);
        write_file_atomic(path,set->icon->buffer,set->icon->buffer_size);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t memory_sync(const MemorySync *sync) {
    NOTE_SAVE("sceSaveDataSyncSaveDataMemory");
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!sync) return ERR_PARAMETER;
    const int32_t r=memory_slot_ready(sync->slot);
    if (r) return r;
    pthread_mutex_lock(&lock);
    sync_file(memory[sync->slot].path);
    pthread_mutex_unlock(&lock);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",save_umount},
    {"sceSaveDataUmountWithBackup",save_umount_with_backup}, {"sceSaveDataGetMountInfo",save_get_mount_info},
    {"sceSaveDataSetParam",save_set_param}, {"sceSaveDataGetParam",save_get_param},
    {"sceSaveDataSaveIcon",save_icon}, {"sceSaveDataLoadIcon",save_load_icon},
    {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search}, {"sceSaveDataBackup",save_backup},
    {"sceSaveDataCheckBackupData",save_check_backup},
    {"sceSaveDataGetProgress",save_get_progress}, {"sceSaveDataClearProgress",save_clear_progress},
    {"sceSaveDataGetEventResult",save_get_event_result},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
    {"sceSaveDataSetupSaveDataMemory2",memory_setup2}, {"sceSaveDataGetSaveDataMemory2",memory_get2},
    {"sceSaveDataSetSaveDataMemory2",memory_set2}, {"sceSaveDataSyncSaveDataMemory",memory_sync},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) {
    printf("Runtime: save data mounts=%zu, commits=%zu, memory writes=%zu\n",mounts_done,commits_done,memory_writes);
}
