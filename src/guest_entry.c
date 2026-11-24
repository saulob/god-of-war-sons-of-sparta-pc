#include "guest_entry.h"

#if !defined(__x86_64__)
#error "sos_guest_enter requires an x86-64 host"
#endif

/* The guest entry is called with RBP=0. Its prologue (push rbp) therefore leaves a zero saved frame pointer,
 * and the guest C++ unwinder stops there: without it, the entry saves the host's RBP and return address, and
 * a backtrace walks into host frames (on the main thread the host log path was read as the next frame). The
 * host RBP/RSP are restored if the entry ever returns, and a non-null stack_top switches to the guest stack
 * first. RAX (the entry's result) is left untouched.
 *
 * A top-level asm definition (rather than a naked C function) keeps the same code on GCC and Clang; the header's
 * SysV declaration gives callers the guest ABI on Windows. */
__asm__(
    ".text\n"
    ".globl sos_guest_enter\n"
    "sos_guest_enter:\n"
    "pushq %rbp\n"
    "movq %rsp, %rbp\n"
    "pushq %rbx\n"
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    "subq $8, %rsp\n"          /* 16-byte alignment before the guest call */
    "movq %rdi, %rax\n"        /* entry */
    "movq %rsi, %r12\n"        /* arg0 */
    "movq %rdx, %r13\n"        /* arg1 */
    "movq %rcx, %r14\n"        /* stack_top */
    "movq %rsp, %rbx\n"        /* host RSP */
    "movq %rbp, %r15\n"        /* host RBP */
    "testq %r14, %r14\n"
    "jz 1f\n"
    "movq %r14, %rsp\n"
    "andq $-16, %rsp\n"
    "1:\n"
    "xorl %ebp, %ebp\n"        /* the guest's frame-chain terminator */
    "movq %r12, %rdi\n"
    "movq %r13, %rsi\n"
    "callq *%rax\n"
    "movq %r15, %rbp\n"
    "movq %rbx, %rsp\n"
    "addq $8, %rsp\n"
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"
    "popq %rbx\n"
    "popq %rbp\n"
    "retq\n");
