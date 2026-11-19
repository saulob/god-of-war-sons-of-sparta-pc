// SPDX-License-Identifier: GPL-2.0-or-later
// Windows: small POSIX helpers the renderer uses for diagnostics and thread
// identity. Force-included into the GPU library on Windows only (CMakeLists.txt); kept free
// of <windows.h> so its macros do not reach every translation unit.
#pragma once
#ifdef _WIN32
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
FILE* sos_stderr_stream(void);
static inline int gettid(void) { return (int)GetCurrentThreadId(); }
#ifdef __cplusplus
}
#endif
#undef stderr
#define stderr sos_stderr_stream()
#endif
