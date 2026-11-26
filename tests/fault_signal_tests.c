/* The POSIX fatal signal handling of the loader, in crashing child processes (Linux only; tools/test_fault_signal.ps1
 * runs it under WSL): one report in the session log with the phase and last completed stage, then the process ends by
 * its own signal, so the parent sees WIFSIGNALED and the system's core dump policy applies. Never a loop, a hang or an
 * exit status. */
#define _GNU_SOURCE
#include "fault_signal.h"
#include "runtime_log.h"
#include "startup_phase.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static const char *mode = "";
static char *volatile guarded_page;

static void say(const char *text) {
    const ssize_t written = write(2, text, strlen(text));
    (void)written;
}

/* The loader's handler without the game: a fault it can resolve returns; any other is reported once and ends. */
static void handler(int sig, siginfo_t *info, void *context) {
    (void)context;
    char *page = guarded_page;
    if (page && sig == SIGSEGV && (char *)info->si_addr == page) {
        mprotect(page, 4096, PROT_READ | PROT_WRITE);
        guarded_page = NULL;
        say("Test: recovered\n");
        return;
    }
    if (!sos_fault_begin()) {
        say("Test: fault inside the report\n");
        sos_fault_end(sig);
    }
    runtime_log_mark_crash();
    sos_fault_arm_deadline(sig, 1);
    sos_fault_write_header(2, sig, info);
    sos_fault_write_context(2);
    sos_fault_write_location(2, 0x1234, (unsigned long)(uintptr_t)info->si_addr, 0, 0);
    runtime_log_drain();
    if (!strcmp(mode, "blocked")) {
        /* The best-effort part blocks for good, as dladdr would on a loader lock another thread holds. */
        for (;;) pause();
    }
    if (!strcmp(mode, "nested")) {
        /* A real idiv (GCC turns 1 / x into compares): another fault signal while this one is reported. */
        volatile int seven = 7, zero = 0;
        say(seven / zero ? "Test: no fault\n" : "Test: zero\n");
    }
    if (!strcmp(mode, "nested_same")) *(volatile int *)8 = 1;
    if (!strcmp(mode, "concurrent")) {
        const struct timespec wait = {0, 300000000};
        nanosleep(&wait, NULL);
    }
    runtime_log_drain();
    sos_fault_end(sig);
}

static pthread_barrier_t together;
static void *fault_together(void *unused) {
    (void)unused;
    pthread_barrier_wait(&together);
    *(volatile int *)16 = 1;
    return NULL;
}

static int run_child(const char *which, const char *dir, long core_limit) {
    mode = which;
    const int null = open("/dev/null", O_WRONLY);
    if (null < 0 || dup2(null, 1) < 0) return 90;
    char path[4200];
    if (chdir(dir) || !runtime_log_start(dir, NULL, path, sizeof(path))) return 90;
    const struct rlimit limit = {(rlim_t)core_limit, (rlim_t)core_limit};
    if (setrlimit(RLIMIT_CORE, &limit)) return 90;
    if (!sos_fault_install(handler) || !sos_fault_install(handler) || !sos_fault_install_termination()) return 91;
    sos_startup_phase("launcher running");
    sos_startup_phase("play: graphics preflight");
    sos_checkpoint_note("Launcher: shown");

    if (!strcmp(which, "segv") || !strcmp(which, "nested") || !strcmp(which, "nested_same") || !strcmp(which, "blocked"))
        *(volatile int *)0 = 1;
    if (!strcmp(which, "bus")) {
        const int file = memfd_create("sos-fault-signal", 0);
        volatile char *mapped = mmap(NULL, 4096, PROT_READ, MAP_SHARED, file, 0);
        if (mapped == MAP_FAILED) return 93;
        return mapped[0];
    }
    if (!strcmp(which, "ill")) __builtin_trap();
    if (!strcmp(which, "fpe")) {
        volatile int seven = 7, zero = 0;
        return seven / zero;
    }
    if (!strcmp(which, "abort")) abort();
    if (!strcmp(which, "raised")) raise(SIGSEGV);
    if (!strcmp(which, "hangup")) raise(SIGHUP);
    if (!strcmp(which, "recover")) {
        char *page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return 93;
        guarded_page = page;
        page[0] = 7;
        if (page[0] != 7) return 94;
        say("Test: continued after the recovered fault\n");
        *(volatile int *)0 = 1;
    }
    if (!strcmp(which, "concurrent")) {
        pthread_t threads[2];
        pthread_barrier_init(&together, NULL, 2);
        for (int i = 0; i < 2; ++i) pthread_create(&threads[i], NULL, fault_together, NULL);
        for (int i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
    }
    return 92;
}

static char *read_log(const char *dir) {
    char path[4300];
    snprintf(path, sizeof(path), "%s/sons_of_sparta_001.log", dir);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    static char text[65536];
    const size_t used = fread(text, 1, sizeof(text) - 1, file);
    text[used] = 0;
    fclose(file);
    return text;
}

static int count_of(const char *text, const char *needle) {
    int count = 0;
    for (const char *at = text; (at = strstr(at, needle)); at += strlen(needle)) ++count;
    return count;
}

static int core_files_stay_local(char *pattern, size_t size) {
    FILE *file = fopen("/proc/sys/kernel/core_pattern", "r");
    if (!file) return 0;
    const int read = fgets(pattern, (int)size, file) != NULL;
    fclose(file);
    if (!read) return 0;
    pattern[strcspn(pattern, "\n")] = 0;
    return pattern[0] && pattern[0] != '|' && !strchr(pattern, '/');
}

static void remove_folder(const char *dir) {
    char command[4400];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    if (system(command)) printf("note: %s was not removed\n", dir);
}

typedef struct {
    const char *mode;
    int signal;
    int reports;           /* "CRASH: signal" lines */
    const char *const *expect;
} Case;

static void run_case(const char *self, const Case *test, long core_limit, int *core_dumped) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/sos-fault-signal-%d-%s", (int)getpid(), test->mode);
    remove_folder(dir);
    if (mkdir(dir, 0700)) {
        printf("FAILED: %s: no folder\n", test->mode);
        ++failures;
        return;
    }
    char limit_text[32];
    snprintf(limit_text, sizeof(limit_text), "%ld", core_limit);
    const pid_t child = fork();
    if (child == 0) {
        execl(self, self, "--child", test->mode, dir, limit_text, (char *)NULL);
        _exit(95);
    }
    int status = 0, ended = 0;
    for (int wait = 0; wait < 1000 && !ended; ++wait) {
        ended = waitpid(child, &status, WNOHANG) == child;
        if (!ended) {
            const struct timespec pause = {0, 20000000};
            nanosleep(&pause, NULL);
        }
    }
    const int before = failures;
    if (!ended) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        printf("FAILED: %s: the child hung\n", test->mode);
        ++failures;
    }
    const char *log = read_log(dir);
    CHECK(log != NULL);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != test->signal)
        printf("%s: status %#x, expected the end by signal %d\n", test->mode, status, test->signal);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == test->signal);
    CHECK(!WIFEXITED(status));
    if (log) {
        CHECK(count_of(log, "CRASH: signal ") == test->reports);
        if (test->reports) {
            CHECK(strstr(log, "Crash: startup phase: play: graphics preflight\n") != NULL);
            CHECK(strstr(log, "Crash: last completed stage: launcher running\n") != NULL);
            CHECK(strstr(log, "Crash: context phase=play: graphics preflight, ") != NULL);
            CHECK(strstr(log, "] Launcher: shown\n") != NULL);
        }
        /* The stage lines written before the fault reached the file (the log thread was drained). */
        CHECK(strstr(log, "Stage: play: graphics preflight started +") != NULL);
    }
    for (const char *const *text = test->expect; log && text && *text; ++text)
        if (!strstr(log, *text)) {
            printf("FAILED: %s: log lacks \"%s\"\n", test->mode, *text);
            ++failures;
        }
    *core_dumped = WIFSIGNALED(status) && WCOREDUMP(status);
    if (failures != before && log) printf("---- %s log ----\n%s\n", test->mode, log);
    remove_folder(dir);
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--child")) return run_child(argv[2], argv[3], strtol(argv[4], NULL, 10));

    static const char *const nested_text[] = {"Test: fault inside the report\n", NULL};
    static const char *const recover_text[] = {"Test: recovered\n", "Test: continued after the recovered fault\n", NULL};
    static const char *const segv_text[] = {"CRASH: signal 11 (SIGSEGV) code ", "Crash: host fault at rip 0x1234, address 0x0\n",
                                            NULL};
    static const char *const blocked_text[] = {"Crash: host fault at rip 0x1234, address 0x0\n",
                                               "Crash: the rest of the report timed out; ending by the original signal\n", NULL};
    static const char *const abort_text[] = {"CRASH: signal 6 (SIGABRT)", NULL};
    static const char *const hangup_text[] = {
        "Session: ended by signal 1 (SIGHUP) during phase: play: graphics preflight; last completed stage: launcher running\n",
        NULL};
    static const Case cases[] = {
        {"segv", SIGSEGV, 1, segv_text},
        {"bus", SIGBUS, 1, NULL},
        {"ill", SIGILL, 1, NULL},
        {"fpe", SIGFPE, 1, NULL},
        {"abort", SIGABRT, 1, abort_text},
        {"raised", SIGSEGV, 1, NULL},
        {"nested", SIGFPE, 1, nested_text},
        {"nested_same", SIGSEGV, 1, NULL},
        {"recover", SIGSEGV, 1, recover_text},
        {"concurrent", SIGSEGV, 1, NULL},
        {"hangup", SIGHUP, 0, hangup_text},
        {"blocked", SIGSEGV, 1, blocked_text},
    };
    /* RLIMIT_CORE 1 writes no core anywhere: a core_pattern pipe ignores 0, not 1. */
    const long no_core = 1, small_core = 64L * 1024 * 1024;
    int core_dumped = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) run_case(argv[0], &cases[i], no_core, &core_dumped);

    char pattern[512] = "";
    if (core_files_stay_local(pattern, sizeof(pattern))) {
        static const Case with_core = {"segv", SIGSEGV, 1, NULL};
        run_case(argv[0], &with_core, small_core, &core_dumped);
        CHECK(core_dumped);
    } else {
        printf("note: core_pattern \"%s\" is not a local file; the end by signal (which keeps the system's core dump "
               "policy) was checked, the core file itself was not\n", pattern);
    }
    printf("fault signal tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
