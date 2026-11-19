/* Names of the PS4 libc debug-stop codes passed to sceKernelDebugRaiseException. */
#include "runtime.h"

const char *runtime_debug_stop_name(uint32_t code) {
    switch (code) {
    case 0xa0020007u: return "LIBC_ASSERT";
    case 0xa0020008u: return "LIBC_TERMINATE";
    case 0xa0020009u: return "LIBC_UNEXPECTED";
    case 0xa002000au: return "LIBC_PURE_VIRTUAL";
    case 0xa002000bu: return "LIBC_ABORT";
    default: return "unknown";
    }
}
