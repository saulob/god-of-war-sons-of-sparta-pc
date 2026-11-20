#pragma once
#ifndef _WIN32
#include <signal.h>

/* A write to a socket whose peer closed must fail with EPIPE, not kill the game. */
static inline int sos_ignore_sigpipe(void) {
    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    return sigaction(SIGPIPE, &ignore, NULL);
}
#endif
