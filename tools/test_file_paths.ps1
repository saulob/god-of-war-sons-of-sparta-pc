# Headless test of the guest file system layer: mount translation, path normalization, read-only mounts, POSIX and SCE
# return conventions and the file operations on a writable mount. runtime_file.c is compiled with small stubs; no
# game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_file_paths_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
@'
#include "runtime.h"
#include <windows.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
FILE *sos_stderr_stream(void) { return stdout; }
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) { (void)api; (void)caller; _Exit(status); }
static __thread int32_t guest_errno_value;
int32_t *runtime_errno(void) { return &guest_errno_value; }
int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case EBADF: return 9; case EACCES: return 13; case EBUSY: return 16;
    case EEXIST: return 17; case ENOTDIR: return 20; case EISDIR: return 21; case EINVAL: return 22; case ENOTTY: return 25;
    case ESPIPE: return 29; case EROFS: return 30; case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66;
    default: return 5;
    }
}
int32_t runtime_thread_id_current(void) { return 1; }
const char *runtime_thread_current_name(void) { return "test"; }
int64_t runtime_socket_close_file(int fd) { (void)fd; return -EBADF; }
int64_t runtime_socket_read_file(int fd, void *b, uint64_t n) { (void)fd; (void)b; (void)n; return -EBADF; }
int64_t runtime_socket_write_file(int fd, const void *b, uint64_t n) { (void)fd; (void)b; (void)n; return -EBADF; }
int64_t runtime_socket_fcntl_file(int fd, int c, intptr_t a) { (void)fd; (void)c; (void)a; return -EBADF; }
const char *runtime_symbol(const char *nid) { (void)nid; return NULL; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_symbol_to_nid(const char *symbol, char *out) { (void)symbol; out[0] = 0; }

int runtime_file_mount_read_only(const char *guest, const char *host);
typedef struct { int64_t sec, nsec; } Timespec;
typedef struct { int64_t sec, usec; } Timeval;
typedef struct { void *base; uint64_t length; } Iovec;
typedef struct {
    uint32_t dev, ino; uint16_t mode, nlink; uint32_t uid, gid, rdev;
    Timespec atime, mtime, ctime; int64_t size, blocks; uint32_t blksize, flags, gen; int32_t lspare; Timespec birth;
} Stat;
typedef int64_t (ABI *Open)(const char *, int, int);
typedef int64_t (ABI *Close)(int);
typedef int64_t (ABI *Rw)(int, void *, uint64_t);
typedef int64_t (ABI *Prw)(int, void *, uint64_t, int64_t);
typedef int64_t (ABI *Seek)(int, int64_t, int);
typedef int64_t (ABI *FStat)(int, Stat *);
typedef int64_t (ABI *PStat)(const char *, Stat *);
typedef int64_t (ABI *Path)(const char *);
typedef int64_t (ABI *Path2)(const char *, const char *);
typedef int64_t (ABI *PathMode)(const char *, int);
typedef int64_t (ABI *Trunc)(int, int64_t);
typedef int64_t (ABI *Utimes)(const char *, const Timeval *);
typedef int64_t (ABI *Vec)(int, const Iovec *, int);

#define G_RD 0
#define G_WR 1
#define G_RW 2
#define G_CREAT 0x200
#define G_TRUNC 0x400
#define G_EXCL 0x800
#define SCE(e) ((int64_t)(int32_t)(0x80020000u | (e)))

static int failures;
static void check(const char *name, int condition) {
    printf(condition ? "  [ok]   %s\n" : "  [fail] %s\n", name);
    if (!condition) ++failures;
}
#define F(type, name) type name = (type)runtime_file_resolve(#name)
static void *fn(const char *name) { return (void *)runtime_file_resolve(name); }
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char app0[512], user[512], path[512];
    snprintf(app0, sizeof(app0), "%s\\app0", argv[1]);
    snprintf(user, sizeof(user), "%s\\user", argv[1]);
    CreateDirectoryA(app0, NULL);
    snprintf(path, sizeof(path), "%s\\Media", app0); CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s\\Hello.txt", app0); put(path, "hello world");
    runtime_file_configure(app0, user);

    Open sce_open = fn("sceKernelOpen"), posix_open = fn("open");
    Close sce_close = fn("sceKernelClose"), posix_close = fn("close");
    Rw rd = fn("read"), wr = fn("write");
    Prw pread_ = fn("pread"), pwrite_ = fn("pwrite");
    Seek seek = fn("lseek"), sce_seek = fn("sceKernelLseek");
    FStat fstat_ = fn("fstat");
    PStat stat_ = fn("stat"), sce_stat = fn("sceKernelStat");
    Path mkdir_ = fn("mkdir"), rmdir_ = fn("rmdir"), unlink_ = fn("unlink"), sce_unlink = fn("sceKernelUnlink");
    Path2 rename_ = fn("rename");
    PathMode mkdir_mode = fn("mkdir"), access_ = fn("access"), chmod_ = fn("chmod");
    Trunc ftruncate_ = fn("ftruncate");
    Utimes utimes_ = fn("utimes");
    Vec writev_ = fn("writev"), readv_ = fn("readv");
    if (!sce_open || !posix_open || !rd || !wr || !pread_ || !pwrite_ || !seek || !fstat_ || !stat_ || !mkdir_ ||
        !rename_ || !ftruncate_ || !access_ || !chmod_ || !utimes_ || !writev_ || !readv_) {
        puts("  [fail] file exports are missing");
        return 1;
    }

    char host[1024];
    check("/temp0 translates into the user directory",
          runtime_file_translate("/temp0/a.txt", host, sizeof(host)) == 0 && strstr(host, "user") && strstr(host, "a.txt"));
    check("/app0 translates into the package root",
          runtime_file_translate("/app0/Media/x", host, sizeof(host)) == 0 && strstr(host, "app0"));
    check("repeated separators and dot components collapse",
          runtime_file_translate("/app0//Media/./x", host, sizeof(host)) == 0 && !strstr(host, "//Media") && !strstr(host, "/./"));
    check("a relative path resolves under /app0",
          runtime_file_translate("Media/x", host, sizeof(host)) == 0 && strstr(host, "app0"));
    check("an unknown prefix is ENOENT", runtime_file_translate("/nowhere/x", host, sizeof(host)) == ENOENT);
    check("/archive/mount/point is ENOENT without a mount",
          runtime_file_translate("/archive/mount/point/a.psarc", host, sizeof(host)) == ENOENT);
    check("a .. component is rejected", runtime_file_translate("/app0/../user/x", host, sizeof(host)) == EACCES);
    check("a backslash .. component is rejected", runtime_file_translate("/temp0/..\\x", host, sizeof(host)) == EACCES);
    check("a stream or drive colon is rejected", runtime_file_translate("/temp0/a:b", host, sizeof(host)) == ENOENT);
    char longname[400];
    memset(longname, 'a', sizeof(longname) - 1); longname[sizeof(longname) - 1] = 0;
    char longpath[460];
    snprintf(longpath, sizeof(longpath), "/temp0/%s", longname);
    check("a component over 255 bytes is ENAMETOOLONG", runtime_file_translate(longpath, host, sizeof(host)) == ENAMETOOLONG);

    int64_t fd = sce_open("/nowhere/x", G_RD, 0);
    check("SCE open of an unknown prefix returns the SCE ENOENT code", fd == SCE(2));
    *runtime_errno() = 0;
    fd = posix_open("/nowhere/x", G_RD, 0);
    check("POSIX open of an unknown prefix returns -1 and sets errno 2", fd == -1 && *runtime_errno() == 2);
    check("open with an invalid access mode is EINVAL", sce_open("/temp0/x", 3, 0) == SCE(22));

    fd = sce_open("/app0/Hello.txt", G_RD, 0);
    char buffer[64] = {0};
    check("/app0 file opens read-only", fd >= 3);
    check("read returns the file bytes", rd((int)fd, buffer, 5) == 5 && !memcmp(buffer, "hello", 5));
    check("pread does not move the offset", pread_((int)fd, buffer, 5, 6) == 5 && !memcmp(buffer, "world", 5) &&
                                              seek((int)fd, 0, 1) == 5);
    check("SEEK_DATA is ENOTTY", sce_seek((int)fd, 0, 3) == SCE(25));
    check("write on a read-only descriptor fails", wr((int)fd, "x", 1) == -1);
    Stat st;
    check("fstat reports the size", fstat_((int)fd, &st) == 0 && st.size == 11 && (st.mode & 0170000) == 0100000);
    check("close succeeds", posix_close((int)fd) == 0);
    check("a second close is EBADF", sce_close((int)fd) == SCE(9));

    check("/app0 is case-insensitive like its Windows host", stat_("/app0/hello.TXT", &st) == 0);
    check("write open under /app0 is EROFS", sce_open("/app0/Hello.txt", G_WR, 0) == SCE(30));
    check("create under /app0 is EROFS", sce_open("/app0/new.txt", G_RD | G_CREAT, 0) == SCE(30));
    check("create of an existing /app0 file with G_EXCL is EEXIST", sce_open("/app0/Hello.txt", G_RD | G_CREAT | G_EXCL, 0) == SCE(17));
    check("mkdir under /app0 is EROFS", sce_open("/app0/Media", G_RD, 0) >= 3 && mkdir_("/app0/newdir") == -1 && *runtime_errno() == 30);
    check("mkdir of an existing /app0 folder is EEXIST", mkdir_("/app0/Media") == -1 && *runtime_errno() == 17);
    check("unlink under /app0 is EROFS", sce_unlink("/app0/Hello.txt") == SCE(30));
    check("rename under /app0 is EROFS", rename_("/app0/Hello.txt", "/temp0/h.txt") == -1 && *runtime_errno() == 30);
    check("a missing /app0 file is ENOENT", sce_stat("/app0/none.bin", &st) == SCE(2));
    check("a missing Unity probe file inside /app0/Media is ENOENT", sce_stat("/app0/Media/RuntimeInitializeOnLoads.json", &st) == SCE(2));
    check("the root is a directory", stat_("/", &st) == 0 && (st.mode & 0170000) == 0040000);
    check("a directory stats as a directory", stat_("/app0/Media", &st) == 0 && (st.mode & 0170000) == 0040000);

    fd = sce_open("/temp0/t.bin", G_RW | G_CREAT | G_TRUNC, 0644);
    check("create in /temp0", fd >= 3);
    check("write", wr((int)fd, "0123456789", 10) == 10);
    check("pwrite at an offset", pwrite_((int)fd, "AB", 2, 4) == 2);
    check("pread sees the pwrite", pread_((int)fd, buffer, 10, 0) == 10 && !memcmp(buffer, "0123AB6789", 10));
    check("pread with a negative offset is EINVAL", pread_((int)fd, buffer, 1, -1) == -1 && *runtime_errno() == 22);
    check("lseek to the end", seek((int)fd, 0, 2) == 10);
    check("ftruncate shrinks", ftruncate_((int)fd, 4) == 0 && fstat_((int)fd, &st) == 0 && st.size == 4);
    check("ftruncate with a negative length is EINVAL", ftruncate_((int)fd, -1) == -1 && *runtime_errno() == 22);
    Iovec out[2] = {{"xy", 2}, {"z", 1}};
    seek((int)fd, 0, 0);
    check("writev writes every vector", writev_((int)fd, out, 2) == 3);
    char first[2] = {0}, second[2] = {0};
    Iovec in[2] = {{first, 2}, {second, 1}};
    seek((int)fd, 0, 0);
    check("readv fills every vector", readv_((int)fd, in, 2) == 3 && !memcmp(first, "xy", 2) && second[0] == 'z');
    check("fsync succeeds", ((Close)fn("fsync"))((int)fd) == 0);
    check("rename over an open file replaces it", (put((snprintf(path, sizeof(path), "%s\\temp0\\src.bin", user), path), "s"),
                                                    rename_("/temp0/src.bin", "/temp0/t.bin") == 0));
    check("unlink of an open file succeeds", unlink_("/temp0/t.bin") == 0);
    check("the unlinked name is gone", stat_("/temp0/t.bin", &st) == -1 && *runtime_errno() == 2);
    posix_close((int)fd);

    check("exclusive create of a new file", (fd = sce_open("/temp0/e.bin", G_WR | G_CREAT | G_EXCL, 0644)) >= 3);
    check("exclusive create of an existing file is EEXIST", sce_open("/temp0/e.bin", G_WR | G_CREAT | G_EXCL, 0644) == SCE(17));
    posix_close((int)fd);
    check("rename a file", rename_("/temp0/e.bin", "/temp0/f.bin") == 0 && stat_("/temp0/f.bin", &st) == 0);
    check("rename of a missing source is ENOENT", rename_("/temp0/none", "/temp0/g") == -1 && *runtime_errno() == 2);
    check("access finds the file", access_("/temp0/f.bin", 0) == 0);
    check("access on a missing file is ENOENT", access_("/temp0/none", 0) == -1 && *runtime_errno() == 2);
    check("access for write under /app0 is EROFS", access_("/app0/Hello.txt", 2) == -1 && *runtime_errno() == 30);
    Timeval times[2] = {{1700000000, 0}, {1700000100, 0}};
    check("utimes sets the modification time", utimes_("/temp0/f.bin", times) == 0 && stat_("/temp0/f.bin", &st) == 0 &&
                                                st.mtime.sec == 1700000100);
    check("utimes under /app0 is EROFS", utimes_("/app0/Hello.txt", times) == -1 && *runtime_errno() == 30);
    check("chmod on a missing file is ENOENT", chmod_("/temp0/none", 0644) == -1 && *runtime_errno() == 2);
    check("chmod without write permission, then with it", chmod_("/temp0/f.bin", 0444) == 0 && chmod_("/temp0/f.bin", 0644) == 0);
    check("unlink of a file succeeds", unlink_("/temp0/f.bin") == 0);
    check("unlink of a missing file is ENOENT", sce_unlink("/temp0/f.bin") == SCE(2));

    check("mkdir", mkdir_mode("/temp0/dir", 0755) == 0);
    check("mkdir of an existing folder is EEXIST", mkdir_("/temp0/dir") == -1 && *runtime_errno() == 17);
    check("a file in the folder", (fd = sce_open("/temp0/dir/a", G_WR | G_CREAT, 0644)) >= 3);
    posix_close((int)fd);
    check("rmdir of a non-empty folder is ENOTEMPTY", rmdir_("/temp0/dir") == -1 && *runtime_errno() == 66);
    check("unlink of a folder is EPERM", unlink_("/temp0/dir") == -1 && *runtime_errno() == 1);
    check("rmdir of a file is ENOTDIR", rmdir_("/temp0/dir/a") == -1 && *runtime_errno() == 20);
    check("rmdir of a mount point is EBUSY", rmdir_("/temp0") == -1 && *runtime_errno() == 16);
    check("a folder opens as a directory with a write flag refused", sce_open("/temp0/dir", G_WR, 0) == SCE(21));
    check("O_DIRECTORY on a file is ENOTDIR", sce_open("/temp0/dir/a", G_RD | 0x20000, 0) == SCE(20));
    unlink_("/temp0/dir/a");
    check("rmdir of an empty folder", rmdir_("/temp0/dir") == 0);

    {
        /* A launcher-prepared copy of one game file is served through an exact path alias; nothing else changes. */
        char original[512], streamed[512], unrelated[512], prepared[512], prepared_streamed[512], prepared_bundle[512], aliased[512];
        static const char kBundleGuest[] = "/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb69.bundle";
        snprintf(original, sizeof(original), "%s\\Media\\resources.assets", app0);
        snprintf(streamed, sizeof(streamed), "%s\\Media\\resources.assets.resS", app0);
        snprintf(unrelated, sizeof(unrelated), "%s\\Media\\resources.assets.bak", app0);
        snprintf(prepared, sizeof(prepared), "%s\\prepared.assets", argv[1]);
        snprintf(prepared_streamed, sizeof(prepared_streamed), "%s\\prepared.resS", argv[1]);
        snprintf(prepared_bundle, sizeof(prepared_bundle), "%s\\prepared.bundle", argv[1]);
        put(original, "original assets");
        put(streamed, "original streamed");
        put(unrelated, "backup");
        put(prepared, "prepared assets bytes");
        put(prepared_streamed, "prepared streamed bytes!");
        put(prepared_bundle, "prepared bundle bytes!!");
        check("before an alias the game file is the package file",
              runtime_file_translate("/app0/Media/resources.assets", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0") &&
                  strstr(aliased, "resources.assets"));
        check("an alias needs a guest and a host path", runtime_file_alias(NULL, prepared) == -1 && runtime_file_alias("/app0/Media/resources.assets", NULL) == -1 &&
                                                          runtime_file_alias("", prepared) == -1 && runtime_file_alias("/app0/Media/resources.assets", "") == -1);
        check("an alias registers", runtime_file_alias("/app0/Media/resources.assets", prepared) == 0);
        check("the aliased path resolves to the prepared copy",
              runtime_file_translate("/app0/Media/resources.assets", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared));
        check("a relative spelling and repeated separators resolve to it too",
              runtime_file_translate("Media/resources.assets", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared) &&
                  runtime_file_translate("/app0//Media/./resources.assets", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared));
        check("the streamed file is still the package file until it has its own alias",
              runtime_file_translate("/app0/Media/resources.assets.resS", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0"));
        check("the guest reads the prepared bytes", (fd = sce_open("/app0/Media/resources.assets", G_RD, 0)) >= 3 &&
                                                       rd((int)fd, buffer, sizeof(buffer)) == (int64_t)strlen("prepared assets bytes") &&
                                                       !memcmp(buffer, "prepared assets bytes", strlen("prepared assets bytes")));
        Stat aliased_stat;
        check("fstat reports the prepared size", fstat_((int)fd, &aliased_stat) == 0 && aliased_stat.size == (int64_t)strlen("prepared assets bytes"));
        posix_close((int)fd);
        check("stat by path reports the prepared size",
              sce_stat("/app0/Media/resources.assets", &aliased_stat) == 0 && aliased_stat.size == (int64_t)strlen("prepared assets bytes"));
        check("the aliased game file is still read-only", sce_open("/app0/Media/resources.assets", G_WR, 0) < 0);
        check("a file with the same prefix is not aliased",
              runtime_file_translate("/app0/Media/resources.assets.bak", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0") &&
                  strstr(aliased, "resources.assets.bak"));
        check("a sibling file is not aliased",
              runtime_file_translate("/app0/Media/Other.assets", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0"));
        check("the Media folder is not aliased",
              runtime_file_translate("/app0/Media", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0"));
        check("a file below the aliased path is not aliased",
              runtime_file_translate("/app0/Media/resources.assets/x", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0"));
        check("another mount is not aliased",
              runtime_file_translate("/temp0/resources.assets", aliased, sizeof(aliased)) == 0 && strstr(aliased, "user"));
        check("the streamed file registers its own alias", runtime_file_alias("/app0/Media/resources.assets.resS", prepared_streamed) == 0);
        check("the streamed file resolves to its prepared copy",
              runtime_file_translate("/app0/Media/resources.assets.resS", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared_streamed));
        check("the first alias still holds",
              runtime_file_translate("/app0/Media/resources.assets", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared));
        check("before its alias the icon bundle is the package file",
              runtime_file_translate(kBundleGuest, aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0") && strstr(aliased, "0342107720d621839faadc2c6c3deb69.bundle"));
        check("the icon bundle registers its own alias", runtime_file_alias(kBundleGuest, prepared_bundle) == 0);
        check("the icon bundle path resolves to its prepared copy", runtime_file_translate(kBundleGuest, aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared_bundle));
        check("another spelling of the icon bundle path resolves to it too",
              runtime_file_translate("/app0//Media/StreamingAssets/./aa/PS4/0342107720d621839faadc2c6c3deb69.bundle", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared_bundle));
        check("the guest reads the prepared bundle bytes", (fd = sce_open(kBundleGuest, G_RD, 0)) >= 3 && rd((int)fd, buffer, sizeof(buffer)) == (int64_t)strlen("prepared bundle bytes!!") &&
                                                              !memcmp(buffer, "prepared bundle bytes!!", strlen("prepared bundle bytes!!")));
        posix_close((int)fd);
        check("another bundle of the same folder is not aliased",
              runtime_file_translate("/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb6a.bundle", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0") &&
                  strstr(aliased, "deb6a.bundle"));
        check("the bundle folder is not aliased", runtime_file_translate("/app0/Media/StreamingAssets/aa/PS4", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0"));
        check("a file below the aliased bundle is not aliased",
              runtime_file_translate("/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb69.bundle/x", aliased, sizeof(aliased)) == 0 && strstr(aliased, "app0"));
        check("the earlier aliases still hold after the bundle's",
              runtime_file_translate("/app0/Media/resources.assets", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared) &&
                  runtime_file_translate("/app0/Media/resources.assets.resS", aliased, sizeof(aliased)) == 0 && !strcmp(aliased, prepared_streamed));
        char long_host[600];
        memset(long_host, 'a', sizeof(long_host) - 1);
        long_host[sizeof(long_host) - 1] = 0;
        check("a host path that does not fit is refused", runtime_file_alias("/app0/Media/long.assets", long_host) == -1);
        check("the table has room for four aliases: the three prepared icon files and one more", runtime_file_alias("/app0/Media/z.assets", prepared) == 0 &&
                                                                                              runtime_file_alias("/app0/Media/w.assets", prepared) == -1);
    }
    runtime_file_mount_read_only("/savedata0", user);
    check("a read-only mount refuses writes", sce_open("/savedata0/x.bin", G_WR | G_CREAT, 0644) == SCE(30));
    runtime_file_unmount("/savedata0");
    check("an unmounted prefix is ENOENT again", sce_open("/savedata0/x.bin", G_RD, 0) == SCE(2));

    check("/dev/urandom reads bytes", (fd = sce_open("/dev/urandom", G_RD, 0)) >= 3 && rd((int)fd, buffer, 16) == 16);
    posix_close((int)fd);
    check("/dev/null swallows writes", (fd = sce_open("/dev/null", G_WR, 0)) >= 3 && wr((int)fd, "abc", 3) == 3 && rd((int)fd, buffer, 1) == 0);
    posix_close((int)fd);
    return failures == 0 ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_file.c') (Join-Path $PSScriptRoot 'startup_profile_stubs.c') (Join-Path $root 'src\compat_win.c') '-pthread' '-lbcrypt' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the file paths test' }
$env:PATH = "$($tc.Bin);$env:PATH"
$data = Join-Path $temp 'data'
New-Item -ItemType Directory -Force -Path $data | Out-Null
& $exe $data
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
