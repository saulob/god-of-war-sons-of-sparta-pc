# Headless history-ring test: wrapping keeps the last 32 transitions, module and host events are
# retained, and caller text appears only when a guest caller exists. No game or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_history_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test_history.c'
@'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *capture;
FILE *sos_stderr_stream(void) { return capture; }
int32_t runtime_thread_id_current(void) { return 3; }
void runtime_describe_guest_address(uintptr_t address, char *out, size_t size) {
    snprintf(out, size, "image+0x%llx", (unsigned long long)address);
}
void runtime_history_note(const char *api, const char *library, uintptr_t caller);
void runtime_history_dump(void);

int main(void) {
    capture = tmpfile();
    if (!capture) return 2;
    char api[32];
    for (int i = 0; i < 40; ++i) {
        snprintf(api, sizeof(api), "api-%d", i);
        runtime_history_note(api, "test", i ? (uintptr_t)(0x1000 + i) : 0);
    }
    runtime_history_note("loaded module 0x8f (libSceMove)", "libSceSysmodule", (uintptr_t)0xaa3860);
    runtime_history_note("VideoOut stage: present", "dcpgpu", 0);
    runtime_history_dump();
    fflush(capture);
    fseek(capture, 0, SEEK_END);
    const long size = ftell(capture);
    rewind(capture);
    char *text = malloc((size_t)size + 1);
    if (!text) return 3;
    const size_t read = fread(text, 1, (size_t)size, capture);
    text[read] = 0;
    fclose(capture);

    int failures = 0;
    if (!strstr(text, "Recent PS4 calls (32)")) { fprintf(stderr, "FAIL: ring size\n"); ++failures; }
    if (strstr(text, "api-9 ")) { fprintf(stderr, "FAIL: oldest entry retained\n"); ++failures; }
    if (!strstr(text, "api-10 ")) { fprintf(stderr, "FAIL: first retained entry missing\n"); ++failures; }
    if (!strstr(text, "api-39 ")) { fprintf(stderr, "FAIL: newest entry missing\n"); ++failures; }
    if (!strstr(text, "loaded module 0x8f (libSceMove) (libSceSysmodule) thread 3")) {
        fprintf(stderr, "FAIL: module event missing\n"); ++failures;
    }
    if (!strstr(text, "loaded module 0x8f (libSceMove) (libSceSysmodule) thread 3 0 ms ago, caller image+0xaa3860")) {
        fprintf(stderr, "FAIL: module caller missing\n"); ++failures;
    }
    if (!strstr(text, "VideoOut stage: present (dcpgpu) thread 3")) {
        fprintf(stderr, "FAIL: host event missing\n"); ++failures;
    }
    if (strstr(text, "VideoOut stage: present (dcpgpu) thread 3 0 ms ago, caller")) {
        fprintf(stderr, "FAIL: host event has a caller\n"); ++failures;
    }
    free(text);
    if (failures) return 1;
    puts("history: all checks passed");
    return 0;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test_history.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_history.c') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the history test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
