#ifndef SOS_GUEST_ENTRY_H
#define SOS_GUEST_ENTRY_H

#if defined(_WIN32)
/* The guest follows the SysV ABI; make the host call it that way even though the executable is built for Windows. */
#define SOS_GUEST_SYSV __attribute__((sysv_abi))
#else
#define SOS_GUEST_SYSV
#endif

/* The System V ABI terminates a backtrace at a zero frame pointer. The guest entry is the outermost guest frame,
 * so it must save zero, not the host's RBP: otherwise the guest's C++ unwinder walks into the host frames above
 * it (on the main thread it read the host log path as the next frame). A non-null stack_top switches to that
 * stack first (the guest main thread uses its own stack on POSIX); the host stack is restored if the entry
 * returns. Returns the entry's result. */
SOS_GUEST_SYSV void *sos_guest_enter(void *entry, void *arg0, void *arg1, void *stack_top);

#endif
