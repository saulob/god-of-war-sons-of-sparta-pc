/* The backup notification of sceSaveDataUmountWithBackup on the production save implementation (src/runtime_savedata.c),
 * and a virtual-time model of how CUSA57767 1.01 consumes it. A disposable user folder is used. */
#include "runtime.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_EVENT_BUSY ((int32_t)0x809F0018)
#define MODE_RDONLY 1
#define MODE_CREATE2 32
#define TYPE_UMOUNT_BACKUP 1
#define TYPE_BACKUP 2

#undef printf
int runtime_log_printf(const char *format, ...) { (void)format; return 0; }
static char user_dir[600];
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest_path, const char *host_path) { (void)guest_path; (void)host_path; return 0; }
void runtime_file_unmount(const char *guest_path) { (void)guest_path; }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
enum { POLL_MS = 1000 };
static uint64_t clock_ms = 1;
uint64_t runtime_monotonic_ms(void) { return clock_ms; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { uint32_t type; int32_t error; int32_t user; uint32_t pad; char title[16]; char dir[32]; uint8_t reserved[40]; } Event;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint; uint8_t reserved[32]; } Backup;
_Static_assert(sizeof(Mount) == 80 && sizeof(MountResult) == 64 && sizeof(Event) == 104 && sizeof(Backup) == 64, "save layouts");

static int32_t (ABI *init)(const void *);
static int32_t (ABI *mount)(const Mount *, MountResult *);
static int32_t (ABI *unmount)(const MountPoint *);
static int32_t (ABI *unmount_backup)(const MountPoint *);
static int32_t (ABI *poll_event)(const void *, Event *);
static int32_t (ABI *backup)(const Backup *);

static int failures;
static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "ok" : "fail", name);
    failures += !condition;
}

static DirName name_of(const char *text) {
    DirName d;
    memset(&d, 0, sizeof(d));
    snprintf(d.data, sizeof(d.data), "%s", text);
    return d;
}
static void path_of(const char *name, const char *suffix, char *out, size_t size) {
    snprintf(out, size, "%s/savedata/1/CUSA57767/%s%s", user_dir, name, suffix);
}
static int write_text(const char *name, const char *suffix, const char *text) {
    char path[800];
    path_of(name, suffix, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return !fclose(f) && ok;
}
static int has_text(const char *name, const char *suffix, const char *text) {
    char path[800], buffer[64] = {0};
    path_of(name, suffix, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t got = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    return got == strlen(text) && !memcmp(buffer, text, got);
}
static int32_t open_save(const char *name, uint32_t mode, MountResult *result) {
    const DirName dir = name_of(name);
    const Mount m = {.user = 1, .dir = &dir, .blocks = 1024, .mode = mode};
    memset(result, 0, sizeof(*result));
    return mount(&m, result);
}
static int save_with_backup(const char *name, const char *text) {
    MountResult result;
    return open_save(name, MODE_CREATE2, &result) == 0 && write_text(name, ".sos-work/data.bin", text) &&
           unmount_backup(&result.point) == 0;
}
static int next_event(uint32_t type, const char *name, int32_t error) {
    Event event;
    clock_ms += POLL_MS;
    return poll_event(NULL, &event) == 0 && event.type == type && event.error == error && event.user == 1 &&
           !strcmp(event.dir, name);
}
static int32_t poll_result(void) {
    Event event;
    memset(&event, 0x5a, sizeof(event));
    const Event before = event;
    const int32_t result = poll_event(NULL, &event);
    if (result && memcmp(&event, &before, sizeof(event))) return 0;
    return result;
}

static void contract(void) {
    MountResult result;
    Event event = {0};
    check("the empty queue reports no event", poll_result() == ERR_NOT_FOUND);
    check("a null event is refused", poll_event(NULL, NULL) == ERR_PARAMETER);
    check("event parameters are refused", poll_event(&event, &event) == ERR_PARAMETER);
    check("the first boot has no global save", open_save("GOWSOSSAVE999", MODE_RDONLY, &result) == ERR_NOT_FOUND);
    check("the new global save is created", open_save("GOWSOSSAVE999", MODE_CREATE2, &result) == 0 && result.status == 1);
    check("the global save is written", write_text("GOWSOSSAVE999", ".sos-work/data.bin", "global"));
    check("nothing is persisted before the unmount", !has_text("GOWSOSSAVE999", "/data.bin", "global"));
    check("no event exists before the unmount", poll_result() == ERR_NOT_FOUND);
    check("the unmount with backup succeeds", unmount_backup(&result.point) == 0);
    check("the save is persisted before any notification", has_text("GOWSOSSAVE999", "/data.bin", "global"));
    check("the backup is persisted before any notification", has_text("GOWSOSSAVE999", ".sos-backup/data.bin", "global"));
    check("the first poll is busy and leaves the event untouched", poll_result() == ERR_EVENT_BUSY);
    check("the next poll delivers the notification", next_event(TYPE_UMOUNT_BACKUP, "GOWSOSSAVE999", 0));
    check("the notification is delivered once", poll_result() == ERR_NOT_FOUND);

    check("a plain unmount posts no notification",
          open_save("GOWSOSSAVE0", MODE_CREATE2, &result) == 0 && unmount(&result.point) == 0 && poll_result() == ERR_NOT_FOUND);

    check("two campaign saves are written", save_with_backup("GOWSOSSAVE0", "one") && save_with_backup("GOWSOSSAVE1", "two"));
    check("one busy poll covers every finished backup", poll_result() == ERR_EVENT_BUSY);
    check("notifications keep their order", next_event(TYPE_UMOUNT_BACKUP, "GOWSOSSAVE0", 0));
    check("a save during the notifications is written", save_with_backup("GOWSOSSAVE2", "three"));
    check("the announced notification is not delayed again", next_event(TYPE_UMOUNT_BACKUP, "GOWSOSSAVE1", 0));
    check("the later backup is busy first", poll_result() == ERR_EVENT_BUSY);
    check("the later notification follows", next_event(TYPE_UMOUNT_BACKUP, "GOWSOSSAVE2", 0));
    check("the queue is empty again", poll_result() == ERR_NOT_FOUND);

    check("a save whose unmount meets the running poll is written", save_with_backup("GOWSOSSAVE3", "four"));
    check("the poll that meets the unmount is busy", poll_result() == ERR_EVENT_BUSY);
    check("the poll woken by that unmount is busy too", poll_result() == ERR_EVENT_BUSY);
    check("the notification follows a second later", next_event(TYPE_UMOUNT_BACKUP, "GOWSOSSAVE3", 0));
    check("it is delivered once", poll_result() == ERR_NOT_FOUND);

    const DirName dir = name_of("GOWSOSSAVE0");
    const Backup request = {.user = 1, .dir = &dir};
    check("an explicit backup succeeds", backup(&request) == 0);
    check("an explicit backup is busy first", poll_result() == ERR_EVENT_BUSY);
    check("an explicit backup notifies", next_event(TYPE_BACKUP, "GOWSOSSAVE0", 0));

    check("the blocked save mounts", open_save("BLOCKED", MODE_CREATE2, &result) == 0 &&
                                         write_text("BLOCKED", ".sos-work/data.bin", "keep"));
    check("the backup destination is blocked", write_text("BLOCKED", ".sos-backup", "block"));
    check("a failed backup is reported by the unmount", unmount_backup(&result.point) == ERR_INTERNAL);
    check("a failed backup keeps the committed save", has_text("BLOCKED", "/data.bin", "keep"));
    check("a failed backup is busy first", poll_result() == ERR_EVENT_BUSY);
    check("the notification carries the backup error", next_event(TYPE_UMOUNT_BACKUP, "BLOCKED", ERR_INTERNAL));
    check("a failed backup leaves no pending event", poll_result() == ERR_NOT_FOUND);
}

enum { WORKERS = 4, ROUNDS = 3 };
static void *save_worker(void *arg) {
    const int index = (int)(intptr_t)arg;
    char name[32];
    snprintf(name, sizeof(name), "PARALLEL%d", index);
    intptr_t ok = 1;
    for (int i = 0; i < ROUNDS; ++i) {
        MountResult result;
        char text[16];
        snprintf(text, sizeof(text), "%d-%d", index, i);
        ok &= open_save(name, MODE_CREATE2, &result) == 0 && write_text(name, ".sos-work/data.bin", text);
        ok &= unmount_backup(&result.point) == 0 && has_text(name, "/data.bin", text) &&
              has_text(name, ".sos-backup/data.bin", text);
    }
    return (void *)ok;
}
static void concurrency(void) {
    pthread_t threads[WORKERS];
    intptr_t ok = 1;
    for (int i = 0; i < WORKERS; ++i) ok &= !pthread_create(&threads[i], NULL, save_worker, (void *)(intptr_t)i);
    for (int i = 0; i < WORKERS; ++i) {
        void *done = NULL;
        pthread_join(threads[i], &done);
        ok &= (intptr_t)done;
    }
    check("concurrent saves are all persisted and backed up", (int)ok);
    int delivered = 0, polls = 0;
    for (; polls < 64; ++polls) {
        Event event;
        clock_ms += POLL_MS;
        const int32_t r = poll_event(NULL, &event);
        if (r == ERR_NOT_FOUND) break;
        delivered += r == 0;
    }
    check("concurrent saves leave the queue drained", delivered == 4 && polls < 64);
}

/* CommitSaveTask of 1.01 arms one flag per step and awaits it with WaitForResponseAsync(15000 ms, 10 ms polls). Every
 * response clears the flag, UpdateState dispatches one queued response per call, and both PS4Controller.Update and the
 * wait itself call it. The notification thread polls after the unmount and again a second later unless nothing is left;
 * the unmount of the next save ends that second early, so a poll can meet an unmount and be repeated at once. */
typedef struct { double frame, phase, delay, arrival, lag; int immediate, meets; } Timing;
enum { RESPONSE_UNMOUNT = 1, RESPONSE_BACKUP = 2, STEP_UNMOUNT = 0, STEP_BACKUP = 1, STEP_DONE = 2 };
typedef struct { int kind; double at; } Response;
typedef struct {
    int waiting, step, polls;
    double resume;
    Response queue[8];
    int count;
    int lost;
} Guest;

static void push_response(Guest *g, int kind, double at) { g->queue[g->count++] = (Response){kind, at}; }
static void update_state(Guest *g, double now) {
    if (!g->count || g->queue[0].at > now) return;
    memmove(g->queue, g->queue + 1, sizeof(*g->queue) * (size_t)--g->count);
    g->waiting = 0;
}
static void wait_for_response(Guest *g, double now, double delay) {
    for (;;) {
        if (!g->waiting || g->polls * 10 >= 15000) {
            g->lost |= g->waiting;
            g->waiting = 0;
            if (++g->step == STEP_DONE) return;
            g->waiting = 1;
            g->polls = 0;
            continue;
        }
        update_state(g, now);
        g->resume = now + delay;
        return;
    }
}
static int commit_completes(const Timing *t, double *elapsed) {
    Guest g = {.waiting = 1};
    MountResult result;
    if (open_save("MODEL", MODE_CREATE2, &result) || !write_text("MODEL", ".sos-work/data.bin", "model")) return 0;
    g.resume = t->delay;
    double next_poll = -1;
    uint64_t start = clock_ms;
    int unmounted = 0;
    for (long frame = 1; frame < 4000 && g.step != STEP_DONE; ++frame) {
        const double starts[2] = {frame * t->frame, frame * t->frame + t->phase};
        for (int part = 0; part < 2; ++part) {
            const double now = starts[part];
            if (!unmounted && t->arrival <= now) {
                unmounted = 1;
                if (unmount_backup(&result.point)) return 0;
                push_response(&g, RESPONSE_UNMOUNT, t->arrival);
                if (t->meets && poll_result() == 0) push_response(&g, RESPONSE_BACKUP, t->arrival);
                next_poll = t->arrival + t->lag;
            }
            while (next_poll >= 0 && next_poll <= now) {
                Event event;
                clock_ms = start + (uint64_t)next_poll;
                int32_t r = poll_event(NULL, &event);
                if (r == ERR_EVENT_BUSY && t->immediate) {
                    clock_ms = start += POLL_MS;
                    r = poll_event(NULL, &event);
                }
                if (r == 0) push_response(&g, RESPONSE_BACKUP, next_poll);
                next_poll = r == ERR_NOT_FOUND ? -1 : next_poll + 1000;
            }
            if (part == 0) {
                update_state(&g, now);
            } else if (g.resume <= now) {
                ++g.polls;
                wait_for_response(&g, now, t->delay);
                if (g.step == STEP_DONE) *elapsed = now - t->arrival;
            }
        }
    }
    for (int i = 0; i < 64 && poll_result() != ERR_NOT_FOUND; ++i) clock_ms += POLL_MS;
    return g.step == STEP_DONE && !g.lost;
}
static void guest_model(void) {
    static const double frames[] = {1000.0 / 60, 1000.0 / 90};
    static const double delays[] = {10, 31.2};
    int runs = 0, completed = 0, met = 0, lost_when_immediate = 0;
    double slowest = 0, stalled = 0;
    for (size_t f = 0; f < 2; ++f)
        for (size_t d = 0; d < 2; ++d)
            for (int a = 0; a < 3; ++a) {
                Timing t = {frames[f], frames[f] * 0.4, delays[d], 40 + a * frames[f] / 3, 0.5, 0, 0};
                double elapsed = 0;
                ++runs;
                completed += commit_completes(&t, &elapsed);
                if (elapsed > slowest) slowest = elapsed;
                t.meets = 1;
                met += commit_completes(&t, &elapsed);
                if (elapsed > slowest) slowest = elapsed;
                t.meets = 0;
                t.immediate = 1;
                elapsed = 0;
                if (!commit_completes(&t, &elapsed)) {
                    ++lost_when_immediate;
                    if (elapsed > stalled) stalled = elapsed;
                }
            }
    printf("  game model: %d timings, slowest save %.0f ms; a notification with the unmount is lost in %d, up to %.1f s each\n",
           runs, slowest, lost_when_immediate, stalled / 1000);
    check("the 1.01 save completes without a lost notification in every timing", completed == runs);
    check("the 1.01 save completes when the running poll meets its unmount", met == runs);
    check("the 1.01 save completes within two seconds", slowest > 0 && slowest < 2000);
    check("the model reproduces the stall when the notification arrives with the unmount",
          lost_when_immediate > 0 && stalled > 15000);
}

static int resolve(void) {
    init = (int32_t (ABI *)(const void *))runtime_savedata_resolve("sceSaveDataInitialize3");
    mount = (int32_t (ABI *)(const Mount *, MountResult *))runtime_savedata_resolve("sceSaveDataMount");
    unmount = (int32_t (ABI *)(const MountPoint *))runtime_savedata_resolve("sceSaveDataUmount");
    unmount_backup = (int32_t (ABI *)(const MountPoint *))runtime_savedata_resolve("sceSaveDataUmountWithBackup");
    poll_event = (int32_t (ABI *)(const void *, Event *))runtime_savedata_resolve("sceSaveDataGetEventResult");
    backup = (int32_t (ABI *)(const Backup *))runtime_savedata_resolve("sceSaveDataBackup");
    return init && mount && unmount && unmount_backup && poll_event && backup;
}

int main(void) {
    const char *dir = getenv("SOS_TEST_SAVE_DIR");
    if (!dir || !*dir || !resolve()) {
        puts("  [fail] the test folder or the save exports are missing");
        return 1;
    }
    snprintf(user_dir, sizeof(user_dir), "%s", dir);
    runtime_savedata_configure("CUSA57767");
    Event event;
    check("a poll before initialization is refused", poll_event(NULL, &event) == ERR_NOT_INITIALIZED);
    init(NULL);
    contract();
    concurrency();
    guest_model();
    printf("save backup tests: %d failures\n", failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
