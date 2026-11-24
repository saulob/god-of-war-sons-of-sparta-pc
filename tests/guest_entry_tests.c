/* sos_guest_enter: the guest entry must see a zero frame pointer (the terminator of the guest's C++ unwinder) and
 * must receive its arguments through the SysV ABI; a supplied guest stack is used, and the host RSP/RBP are restored
 * if the entry returns. No game, launcher or window is started. */
#include "guest_entry.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "ok" : "fail", name);
    fflush(stdout);
    if (!condition) ++failures;
}

/* The assembler references these directly; make them externally visible so they are not dropped. */
uint64_t seen_rbp, seen_rsp, seen_saved_rbp, seen_arg0, seen_arg1;

#define RETURN_VALUE UINT64_C(0x123456789abcdef0)

/* Mimics a guest entry prologue (push rbp) and records both the incoming RBP and the value its prologue saved. */
__attribute__((naked)) SOS_GUEST_SYSV uint64_t capture(void *arg0, void *arg1) {
    __asm__ volatile(
        "movq %rbp, seen_rbp(%rip)\n"
        "movq %rsp, seen_rsp(%rip)\n"
        "movq %rdi, seen_arg0(%rip)\n"
        "movq %rsi, seen_arg1(%rip)\n"
        "pushq %rbp\n"
        "movq %rsp, %rbp\n"
        "movq (%rbp), %rax\n"
        "movq %rax, seen_saved_rbp(%rip)\n"
        "popq %rbp\n"
        "movabsq $0x123456789abcdef0, %rax\n"
        "retq\n");
}

static uint64_t host_rsp(void) {
    uint64_t value;
    __asm__ volatile("movq %%rsp, %0" : "=r"(value));
    return value;
}
static uint64_t host_rbp(void) {
    uint64_t value;
    __asm__ volatile("movq %%rbp, %0" : "=r"(value));
    return value;
}

int main(void) {
    const uint64_t arg0 = UINT64_C(0x1122334455667788);
    const uint64_t arg1 = UINT64_C(0x99aabbccddeeff00);

    const uint64_t null_before_rsp = host_rsp();
    const uint64_t null_before_rbp = host_rbp();
    seen_rbp = seen_rsp = seen_saved_rbp = seen_arg0 = seen_arg1 = 0;
    void *result = sos_guest_enter((void *)capture, (void *)arg0, (void *)arg1, NULL);
    check("the entry result is returned unchanged", (uint64_t)result == RETURN_VALUE);
    check("the entry receives both arguments", seen_arg0 == arg0 && seen_arg1 == arg1);
    check("the entry sees a zero frame pointer", seen_rbp == 0);
    check("the entry prologue saves a zero frame pointer", seen_saved_rbp == 0);
    check("the host RSP is restored after the entry returns", host_rsp() == null_before_rsp);
    check("the host RBP is restored after the entry returns", host_rbp() == null_before_rbp);

    enum { STACK_BYTES = 64 * 1024 };
    unsigned char *stack = (unsigned char *)malloc(STACK_BYTES);
    if (!stack) {
        printf("  [fail] could not allocate the guest test stack\n");
        return 1;
    }
    const uint64_t before_rsp = host_rsp();
    const uint64_t before_rbp = host_rbp();
    seen_rbp = seen_rsp = seen_saved_rbp = seen_arg0 = seen_arg1 = 0;
    result = sos_guest_enter((void *)capture, (void *)arg0, (void *)arg1, stack + STACK_BYTES - 64);
    check("the entry runs on the supplied stack",
          seen_rsp >= (uint64_t)(uintptr_t)stack && seen_rsp < (uint64_t)(uintptr_t)(stack + STACK_BYTES));
    check("the supplied stack entry still sees a zero frame pointer", seen_rbp == 0);
    check("the supplied stack entry prologue still saves a zero frame pointer", seen_saved_rbp == 0);
    check("the supplied stack entry still receives both arguments", seen_arg0 == arg0 && seen_arg1 == arg1);
    check("the entry result is returned on the supplied stack", (uint64_t)result == RETURN_VALUE);
    check("the host RSP is restored after the supplied stack entry", host_rsp() == before_rsp);
    check("the host RBP is restored after the supplied stack entry", host_rbp() == before_rbp);
    free(stack);

    printf("guest entry tests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
