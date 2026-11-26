/* POSIX native faults and termination requests. The port's handler writes its report into the session log, then the
 * signal is delivered again with its default action, so the process ends the way the system expects: the same wait
 * status for the parent (Steam, a shell) and the system's core dump policy (ulimit -c, core_pattern, systemd-coredump).
 * Nothing here calls exit or _exit while the default action can still run. */
#ifndef SOS_FAULT_SIGNAL_H
#define SOS_FAULT_SIGNAL_H

#ifndef _WIN32
#include <signal.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SosFaultHandler)(int sig, siginfo_t *info, void *context);

/* SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT (abort from an assertion or std::terminate). 0 when one could not be
 * installed. Installing again replaces the same handler. */
int sos_fault_install(SosFaultHandler handler);

/* SIGHUP: one "Session: ended by signal" line with the phase, then the default action. SIGTERM and SIGINT are left to
 * SDL, which turns them into a normal quit that the launcher and game window log. */
int sos_fault_install_termination(void);

/* 1: this thread writes the report. 0: it faulted again inside its own report and must end at once. A fault on another
 * thread meanwhile waits here until the reporting thread ends the process. Async-signal-safe. */
int sos_fault_begin(void);

/* "CRASH: signal 11 (SIGSEGV)" header, then "Crash: startup phase: ...", "Crash: last completed stage: ..." and
 * "Crash: context ..." lines. Async-signal-safe. */
void sos_fault_write_header(int fd, int sig, const siginfo_t *info);
void sos_fault_write_context(int fd);

/* "SIGSEGV" for 11; "signal" for an unknown number. */
const char *sos_fault_signal_name(int sig);

/* Formats `value` in decimal / as 0x-prefixed hex into `out` (no stdio); returns the length. Async-signal-safe. */
size_t sos_fault_format_unsigned(char *out, size_t size, unsigned long value);
size_t sos_fault_format_hex(char *out, size_t size, unsigned long value);

/* The name of thread `tid` from /proc/self/task/<tid>/comm (a guest thread's host name is its guest name), NUL-terminated
 * in `out`; returns its length, 0 when unknown. Async-signal-safe (open, read, close). */
size_t sos_fault_thread_name(long tid, char *out, size_t size);

/* "Crash: thread <tid> (<name>)" for the calling thread. Async-signal-safe. */
void sos_fault_write_thread(int fd);

/* "Crash: guest fault at guest offset 0x..., address 0x..." or "Crash: host fault at rip 0x..., address 0x...".
 * Async-signal-safe. */
void sos_fault_write_location(int fd, unsigned long rip, unsigned long address, int guest, unsigned long guest_offset);

/* Before the best-effort part of a report (dladdr, backtrace: they take the dynamic loader's lock and can block): after
 * `seconds` the process ends by `sig` anyway, with its default action (core dump kept). Async-signal-safe. */
void sos_fault_arm_deadline(int sig, unsigned seconds);

/* Restores the default action of `sig` and delivers it again; never returns. Async-signal-safe. */
__attribute__((noreturn)) void sos_fault_end(int sig);

#ifdef __cplusplus
}
#endif

#endif
#endif
