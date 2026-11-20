/* sceSaveDataMount and sceSaveDataMount2 on the production save implementation (src/runtime_savedata.c) against a
 * disposable user folder: a failed mount logs its directory (or a placeholder), error and mode in the normal log, a
 * successful mount logs no failure, and the guest still gets the unchanged error. The file layer is stubbed. */
#include "runtime.h"
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_CREATE2 32

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
static char user_dir[600];
static int file_mounts, file_unmounts;
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest_path, const char *host_path) { (void)guest_path; (void)host_path; ++file_mounts; return 0; }
void runtime_file_unmount(const char *guest_path) { (void)guest_path; ++file_unmounts; }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
_Static_assert(sizeof(Mount1) == 80 && sizeof(Mount2) == 64 && sizeof(MountResult) == 64, "save mount layouts");

typedef int32_t (ABI *InitFn)(const void *);
typedef int32_t (ABI *Mount1Fn)(const Mount1 *, MountResult *);
typedef int32_t (ABI *Mount2Fn)(const Mount2 *, MountResult *);
typedef int32_t (ABI *UmountFn)(const MountPoint *);

static int failures;
static void check(const char *name, int condition) {
    if (condition) {
        printf("  [ok]   %s\n", name);
    } else {
        printf("  [fail] %s\n", name);
        ++failures;
    }
}

static char capture_path[700];
static char captured[8192];
static int saved_stdout = -1;
static void capture_begin(void) {
    fflush(stdout);
    saved_stdout = _dup(1);
    const int file = _open(capture_path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0600);
    _dup2(file, 1);
    _close(file);
}
static void capture_end(void) {
    fflush(stdout);
    _dup2(saved_stdout, 1);
    _close(saved_stdout);
    FILE *in = fopen(capture_path, "rb");
    size_t n = in ? fread(captured, 1, sizeof(captured) - 1, in) : 0;
    captured[n] = 0;
    if (in) fclose(in);
}
static int count(const char *text) {
    int n = 0;
    for (const char *at = captured; (at = strstr(at, text)) != NULL; at += strlen(text)) ++n;
    return n;
}
static int has(const char *text) { return strstr(captured, text) != NULL; }

static Mount1Fn mount1;
static Mount2Fn mount2;
static UmountFn umount;

typedef struct { const DirName *dir; int32_t user; uint64_t blocks; uint32_t mode; } Request;

static int32_t call(int version, Request r, MountResult *result) {
    memset(result, 0, sizeof(*result));
    capture_begin();
    int32_t answer;
    if (version == 1) {
        Mount1 m = {.user = r.user, .dir = r.dir, .blocks = r.blocks, .mode = r.mode};
        answer = mount1(&m, result);
    } else {
        Mount2 m = {.user = r.user, .dir = r.dir, .blocks = r.blocks, .mode = r.mode};
        answer = mount2(&m, result);
    }
    capture_end();
    return answer;
}

static DirName name_of(const char *text) {
    DirName d;
    memset(&d, 0, sizeof(d));
    strncpy(d.data, text, sizeof(d.data) - 1);
    return d;
}

static void scenarios(int version) {
    char label[160];
    const char *which = version == 1 ? "sceSaveDataMount" : "sceSaveDataMount2";
    MountResult result;
    DirName slot = name_of(version == 1 ? "SAVEDATA01" : "SAVEDATA02");
    char expect[160];

    snprintf(label, sizeof(label), "%s: a missing save returns not-found", which);
    int32_t answer = call(version, (Request){&slot, 1, 1024, 0}, &result);
    check(label, answer == ERR_NOT_FOUND);
    snprintf(expect, sizeof(expect), "Runtime: save data mount '%s' failed (error=0x809f0008, mode=0)", slot.data);
    snprintf(label, sizeof(label), "%s: the missing save is logged with its directory, error and mode", which);
    check(label, has(expect) && count("save data mount") == 1);

    answer = call(version, (Request){&slot, 1, 1024, MODE_RDONLY}, &result);
    snprintf(label, sizeof(label), "%s: a read-only mount of a missing save returns not-found", which);
    check(label, answer == ERR_NOT_FOUND);
    snprintf(expect, sizeof(expect), "'%s' failed (error=0x809f0008, mode=0x1)", slot.data);
    snprintf(label, sizeof(label), "%s: the read-only mode is in the log", which);
    check(label, has(expect));

    answer = call(version, (Request){&slot, 1, 1024, MODE_CREATE}, &result);
    snprintf(label, sizeof(label), "%s: a create mount succeeds", which);
    check(label, answer == 0 && !strncmp(result.point.data, "/savedata", 9));
    snprintf(label, sizeof(label), "%s: a successful create produces no failure log", which);
    check(label, !has("failed") && has("save data '") && has("mounted at"));

    MountResult second;
    answer = call(version, (Request){&slot, 1, 1024, MODE_CREATE2}, &second);
    snprintf(label, sizeof(label), "%s: mounting a save that is already mounted returns the already-mounted error", which);
    check(label, answer == ERR_BAD_MOUNTED);
    snprintf(expect, sizeof(expect), "'%s' failed (error=0x809f000d, mode=0x20)", slot.data);
    snprintf(label, sizeof(label), "%s: the duplicate mount is logged", which);
    check(label, has(expect));

    capture_begin();
    const int32_t unmounted = umount(&result.point);
    capture_end();
    snprintf(label, sizeof(label), "%s: the save is unmounted", which);
    check(label, unmounted == 0);

    answer = call(version, (Request){&slot, 1, 1024, MODE_CREATE}, &result);
    snprintf(label, sizeof(label), "%s: a create mount of an existing save returns the exists error", which);
    check(label, answer == ERR_EXISTS);
    snprintf(expect, sizeof(expect), "'%s' failed (error=0x809f0007, mode=0x4)", slot.data);
    snprintf(label, sizeof(label), "%s: the exists failure is logged", which);
    check(label, has(expect));

    answer = call(version, (Request){&slot, 1, 1024, 0}, &result);
    snprintf(label, sizeof(label), "%s: a mount of an existing save succeeds", which);
    check(label, answer == 0);
    snprintf(label, sizeof(label), "%s: a successful existing mount produces no failure log", which);
    check(label, !has("failed") && has("mounted at"));
    capture_begin();
    umount(&result.point);
    capture_end();

    DirName bad_slash = name_of("A/B");
    answer = call(version, (Request){&bad_slash, 1, 1024, MODE_CREATE}, &result);
    snprintf(label, sizeof(label), "%s: a directory with a separator returns the parameter error", which);
    check(label, answer == ERR_PARAMETER);
    snprintf(label, sizeof(label), "%s: it is logged as unnamed, not echoed", which);
    check(label, has("save data mount '(unnamed)' failed (error=0x809f0000, mode=0x4)") && !has("A/B"));

    char region[48];
    memset(region, 'Q', 32);
    memset(region + 32, 0, sizeof(region) - 32);
    answer = call(version, (Request){(const DirName *)region, 1, 1024, MODE_CREATE}, &result);
    snprintf(label, sizeof(label), "%s: a directory without a terminator returns the parameter error", which);
    check(label, answer == ERR_PARAMETER);
    snprintf(label, sizeof(label), "%s: the unterminated text is never printed", which);
    check(label, has("'(unnamed)' failed") && !has("QQQQ"));

    answer = call(version, (Request){NULL, 1, 1024, MODE_CREATE}, &result);
    snprintf(label, sizeof(label), "%s: a missing directory returns the parameter error and is logged as unnamed", which);
    check(label, answer == ERR_PARAMETER && has("'(unnamed)' failed (error=0x809f0000"));

    answer = call(version, (Request){&slot, -1, 1024, MODE_CREATE}, &result);
    snprintf(label, sizeof(label), "%s: an invalid user keeps its error and is logged with the directory", which);
    check(label, answer == ERR_INVALID_USER);
    snprintf(expect, sizeof(expect), "'%s' failed (error=0x809f0011, mode=0x4)", slot.data);
    check("the invalid user failure carries the same fields", has(expect));
}

static const char *failure_line_of(const char *text, char *out, size_t size) {
    const char *at = strstr(text, "failed (error=");
    if (!at) { out[0] = 0; return out; }
    snprintf(out, size, "%.*s", (int)strcspn(at, "\r\n"), at);
    return out;
}

static void equivalence(void) {
    MountResult r1, r2;
    DirName a = name_of("EQUALDIR");
    char l1[200], l2[200];
    int32_t one = call(1, (Request){&a, 1, 1024, MODE_RDONLY}, &r1);
    failure_line_of(captured, l1, sizeof(l1));
    int32_t two = call(2, (Request){&a, 1, 1024, MODE_RDONLY}, &r2);
    failure_line_of(captured, l2, sizeof(l2));
    check("sceSaveDataMount and sceSaveDataMount2 return the same error", one == two && one == ERR_NOT_FOUND);
    check("sceSaveDataMount and sceSaveDataMount2 log the same failure", l1[0] != 0 && !strcmp(l1, l2));

    capture_begin();
    int32_t null1 = mount1(NULL, &r1);
    int32_t null2 = mount2(NULL, &r2);
    capture_end();
    check("a null request returns the parameter error from both entry points without a crash", null1 == ERR_PARAMETER && null2 == ERR_PARAMETER);
}

int main(void) {
    snprintf(user_dir, sizeof(user_dir), "%s", getenv("SOS_TEST_SAVE_DIR"));
    snprintf(capture_path, sizeof(capture_path), "%s\\capture.txt", user_dir);
    InitFn init = (InitFn)runtime_savedata_resolve("sceSaveDataInitialize3");
    mount1 = (Mount1Fn)runtime_savedata_resolve("sceSaveDataMount");
    mount2 = (Mount2Fn)runtime_savedata_resolve("sceSaveDataMount2");
    umount = (UmountFn)runtime_savedata_resolve("sceSaveDataUmount");
    if (!init || !mount1 || !mount2 || !umount) {
        puts("  [fail] the save exports are missing");
        return 1;
    }
    MountResult result;
    DirName early = name_of("EARLY");
    int32_t answer = call(1, (Request){&early, 1, 1024, MODE_CREATE}, &result);
    check("a mount before initialization returns the not-initialized error", answer == ERR_NOT_INITIALIZED);
    check("the not-initialized failure is logged with directory, error and mode", has("'EARLY' failed (error=0x809f0001, mode=0x4)"));
    init(NULL);
    scenarios(1);
    scenarios(2);
    equivalence();
    check("each successful mount reached the file layer once and was unmounted once", file_mounts == 4 && file_unmounts == 4);
    printf("save mount tests: %d failures\n", failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
