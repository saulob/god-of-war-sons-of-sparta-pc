// dcp: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/device_lost.h"
#include "common/logging/log.h"

void assert_fail_impl() {
    // After a device loss the first device-lost report is the crash reason, not this assertion.
    if (Common::DeviceLost::IsLost()) {
        Common::DeviceLost::Global().Observe("a GPU assertion (see the log line above)");
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
