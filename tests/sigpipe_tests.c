/* POSIX only: the loader's SIGPIPE policy (src/sos_posix_signals.h). A write to a socket whose peer closed must fail
 * with EPIPE instead of killing the process. Local AF_UNIX socketpair, no network. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sos_posix_signals.h"

static int failures;
static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "ok  " : "fail", name);
    if (!condition) ++failures;
}

/* Child: closes the peer end of a socketpair and writes. The first write can still be buffered by the kernel, so it
 * retries until the closed peer is seen. Exit 0: the write failed with EPIPE and the process lived. */
static void write_to_closed_peer(int startup_policy) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) _exit(90);
    if (startup_policy && sos_ignore_sigpipe()) _exit(92);
    close(fds[0]);
    ssize_t sent = -1;
    int saved = 0;
    for (int attempt = 0; attempt < 200; ++attempt) {
        sent = write(fds[1], "x", 1);
        saved = errno;
        if (sent < 0 && saved == EPIPE) break;
        usleep(1000);
    }
    _exit(sent < 0 && saved == EPIPE ? 0 : 91);
}

static int run_child(void (*body)(int), int argument, int *signal_out) {
    const pid_t child = fork();
    if (child == 0) {
        body(argument);
        _exit(99);
    }
    int status = 0;
    waitpid(child, &status, 0);
    *signal_out = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void raise_other_signal(int startup_policy) {
    if (startup_policy) sos_ignore_sigpipe();
    raise(SIGUSR1);
    _exit(0);
}

static void report_dispositions(int startup_policy) {
    if (startup_policy) sos_ignore_sigpipe();
    struct sigaction term, usr1, pipe_action;
    sigaction(SIGTERM, NULL, &term);
    sigaction(SIGUSR1, NULL, &usr1);
    sigaction(SIGPIPE, NULL, &pipe_action);
    _exit((term.sa_handler == SIG_DFL ? 1 : 0) | (usr1.sa_handler == SIG_DFL ? 2 : 0) | (pipe_action.sa_handler == SIG_IGN ? 4 : 0));
}

int main(void) {
    int sig = 0;
    int code = run_child(write_to_closed_peer, 0, &sig);
    check("default disposition: the write to a closed peer kills the process with SIGPIPE", sig == SIGPIPE);
    check("default disposition: the child did not exit normally", code == -1);

    code = run_child(write_to_closed_peer, 1, &sig);
    check("startup policy: the same write survives", sig == 0);
    check("startup policy: the write failed with EPIPE and the process lived", code == 0);

    run_child(raise_other_signal, 1, &sig);
    check("startup policy: an unrelated signal (SIGUSR1) still terminates the process", sig == SIGUSR1);

    code = run_child(report_dispositions, 1, &sig);
    check("startup policy: only SIGPIPE is ignored (SIGTERM and SIGUSR1 keep their defaults)", code == 7);

    printf("sigpipe tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
