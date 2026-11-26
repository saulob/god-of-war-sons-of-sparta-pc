/* POSIX implementation of fault_signal.h (Linux). Adapted from Alienation b78805c: the report is written without stdio,
 * one thread reports, and the process ends by its own signal so core dumps and the parent's wait status stay intact. */
#define _GNU_SOURCE
#include "fault_signal.h"

#include "runtime_log.h"
#include "startup_phase.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
/* SIGTERM and SIGINT stay with SDL, which turns them into a normal quit (Steam's "Exit game"); the launcher and the game
 * window log that quit. SIGHUP (the session or terminal went away) has no such owner. */
static const int termination_signals[] = {SIGHUP};
static atomic_long reporting_thread;

static void write_text(int fd, const char *text, size_t size) {
    while (size) {
        const ssize_t written = write(fd, text, size);
        if (written <= 0) return;
        text += written;
        size -= (size_t)written;
    }
}

static void write_string(int fd, const char *text) { write_text(fd, text, strlen(text)); }

size_t sos_fault_format_unsigned(char *out, size_t size, unsigned long value) {
    char digits[24];
    size_t count = 0;
    do digits[count++] = (char)('0' + value % 10); while ((value /= 10) && count < sizeof(digits));
    size_t used = 0;
    while (count && used + 1 < size) out[used++] = digits[--count];
    if (size) out[used] = 0;
    return used;
}

const char *sos_fault_signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTERM: return "SIGTERM";
    case SIGHUP: return "SIGHUP";
    case SIGINT: return "SIGINT";
    case SIGKILL: return "SIGKILL";
    case SIGQUIT: return "SIGQUIT";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
    }
}

int sos_fault_install(SosFaultHandler handler) {
    /* The first backtrace() loads the unwinder (dlopen, malloc), which must not happen inside a fault handler. */
    void *frame;
    backtrace(&frame, 1);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler;
    /* No SA_RESETHAND: recoverable faults (GPU page tracking, ISA emulation, guest memory probes) come back here every
     * time. No SA_ONSTACK: the GPU page-tracking handler needs the thread's own stack. */
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    int installed = 1;
    for (size_t i = 0; i < sizeof(fault_signals) / sizeof(*fault_signals); ++i)
        installed &= sigaction(fault_signals[i], &action, NULL) == 0;
    return installed;
}

static void termination(int sig) {
    static atomic_int once;
    if (atomic_exchange(&once, 1)) sos_fault_end(sig);
    char number[24];
    sos_fault_format_unsigned(number, sizeof(number), (unsigned long)sig);
    write_string(2, "Session: ended by signal ");
    write_string(2, number);
    write_string(2, " (");
    write_string(2, sos_fault_signal_name(sig));
    write_string(2, ") during phase: ");
    write_string(2, sos_startup_phase_current());
    write_string(2, "; last completed stage: ");
    const char *completed = sos_startup_phase_completed();
    write_string(2, completed[0] ? completed : "-");
    write_string(2, "\n");
    runtime_log_drain();
    sos_fault_end(sig);
}

int sos_fault_install_termination(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = termination;
    sigemptyset(&action.sa_mask);
    int installed = 1;
    for (size_t i = 0; i < sizeof(termination_signals) / sizeof(*termination_signals); ++i) {
        struct sigaction previous;
        /* A signal the parent set to be ignored (nohup) stays ignored. */
        if (sigaction(termination_signals[i], NULL, &previous) == 0 && previous.sa_handler == SIG_IGN) continue;
        installed &= sigaction(termination_signals[i], &action, NULL) == 0;
    }
    return installed;
}

size_t sos_fault_format_hex(char *out, size_t size, unsigned long value) {
    static const char digits[] = "0123456789abcdef";
    char reversed[24];
    size_t count = 0;
    do reversed[count++] = digits[value & 15]; while ((value >>= 4) && count < sizeof(reversed));
    size_t used = 0;
    if (used + 1 < size) out[used++] = '0';
    if (used + 1 < size) out[used++] = 'x';
    while (count && used + 1 < size) out[used++] = reversed[--count];
    if (size) out[used] = 0;
    return used;
}

size_t sos_fault_thread_name(long tid, char *out, size_t size) {
    if (!size) return 0;
    out[0] = 0;
    /* "/proc/self/task/<tid>/comm" without stdio. */
    char path[64] = "/proc/self/task/";
    size_t at = strlen(path);
    at += sos_fault_format_unsigned(path + at, sizeof(path) - at, (unsigned long)tid);
    static const char comm[] = "/comm";
    if (at + sizeof(comm) > sizeof(path)) return 0;
    memcpy(path + at, comm, sizeof(comm));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t got;
    do got = read(fd, out, size - 1); while (got < 0 && errno == EINTR);
    close(fd);
    size_t used = got > 0 ? (size_t)got : 0;
    while (used && (out[used - 1] == '\n' || out[used - 1] == '\r')) --used;
    for (size_t i = 0; i < used; ++i)
        if ((unsigned char)out[i] < 0x20) out[i] = '?';
    out[used] = 0;
    return used;
}

void sos_fault_write_thread(int fd) {
    const long tid = syscall(SYS_gettid);
    char number[24], name[32];
    sos_fault_format_unsigned(number, sizeof(number), (unsigned long)tid);
    write_string(fd, "Crash: thread ");
    write_string(fd, number);
    write_string(fd, " (");
    write_string(fd, sos_fault_thread_name(tid, name, sizeof(name)) ? name : "unnamed");
    write_string(fd, ")\n");
}

void sos_fault_write_location(int fd, unsigned long rip, unsigned long address, int guest, unsigned long guest_offset) {
    char number[24];
    write_string(fd, guest ? "Crash: guest fault at guest offset " : "Crash: host fault at rip ");
    sos_fault_format_hex(number, sizeof(number), guest ? guest_offset : rip);
    write_string(fd, number);
    write_string(fd, ", address ");
    sos_fault_format_hex(number, sizeof(number), address);
    write_string(fd, number);
    write_string(fd, "\n");
}

/* The best-effort part of a report (dladdr, the unwinder) takes the dynamic loader's lock: a fault while another thread
 * holds it (dlopen) would hang the report, the other threads waiting in sos_fault_begin, and the core dump with them.
 * The deadline ends the process by the original signal instead. */
static volatile sig_atomic_t deadline_signal;

static void deadline(int unused) {
    (void)unused;
    write_string(2, "Crash: the rest of the report timed out; ending by the original signal\n");
    sos_fault_end(deadline_signal ? deadline_signal : SIGABRT);
}

void sos_fault_arm_deadline(int sig, unsigned seconds) {
    deadline_signal = sig;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = deadline;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    sigset_t only;
    sigemptyset(&only);
    sigaddset(&only, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &only, NULL);
    alarm(seconds);
}

int sos_fault_begin(void) {
    const long self = syscall(SYS_gettid);
    long expected = 0;
    if (atomic_compare_exchange_strong(&reporting_thread, &expected, self)) return 1;
    if (expected == self) return 0;
    for (;;) pause();
}

void sos_fault_write_header(int fd, int sig, const siginfo_t *info) {
    char number[24];
    sos_fault_format_unsigned(number, sizeof(number), (unsigned long)sig);
    write_string(fd, "CRASH: signal ");
    write_string(fd, number);
    write_string(fd, " (");
    write_string(fd, sos_fault_signal_name(sig));
    write_string(fd, ")");
    if (info) {
        sos_fault_format_unsigned(number, sizeof(number), (unsigned long)(unsigned)info->si_code);
        write_string(fd, " code ");
        write_string(fd, number);
    }
    write_string(fd, "\n");
}

void sos_fault_write_context(int fd) {
    static const char phase[] = "Crash: startup phase: ", completed[] = "Crash: last completed stage: ",
                      context[] = "Crash: context ";
    write_text(fd, phase, sizeof(phase) - 1);
    write_string(fd, sos_startup_phase_current());
    write_text(fd, "\n", 1);
    const char *last = sos_startup_phase_completed();
    write_text(fd, completed, sizeof(completed) - 1);
    write_string(fd, last[0] ? last : "-");
    write_text(fd, "\n", 1);
    char line[600];
    const size_t start = sizeof(context) - 1;
    memcpy(line, context, start);
    const size_t used = sos_crash_context_describe(line + start, sizeof(line) - start - 1);
    line[start + used] = '\n';
    write_text(fd, line, start + used + 1);
}

void sos_fault_end(int sig) {
    struct sigaction standard;
    memset(&standard, 0, sizeof(standard));
    standard.sa_handler = SIG_DFL;
    sigemptyset(&standard.sa_mask);
    sigaction(sig, &standard, NULL);
    sigset_t only;
    sigemptyset(&only);
    sigaddset(&only, sig);
    pthread_sigmask(SIG_UNBLOCK, &only, NULL);
    raise(sig);
    /* Reached only for a signal whose default action does not end the process. */
    _exit(128 + sig);
}
