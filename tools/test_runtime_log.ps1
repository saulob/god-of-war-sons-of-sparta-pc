# Headless concurrent logging test: normal output is one complete line per thread, including reports made of several
# printf calls, and the crash-direct writer does not corrupt it. No game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_log_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
$logs = Join-Path $temp 'logs'
@'
#include "runtime.h"
#include "runtime_log.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char path[4200];
static void *writer(void *argument) {
    const long id = (long)(intptr_t)argument;
    for (int i = 0; i < 300; ++i) {
        if (id == 7) {
            char line[128];
            snprintf(line, sizeof(line), "D%02ld-%03d-ABCDEFGHIJKLMNOPQRSTUVWXYZ\n", id, i);
            runtime_log_write_direct(line);
        } else {
            printf("T%02ld-", id);
            printf("%03d-", i);
            printf("ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
        }
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (!runtime_log_start(argv[1], NULL, path, sizeof(path))) return 1;
    pthread_t threads[8];
    for (long i = 0; i < 8; ++i) pthread_create(&threads[i], NULL, writer, (void *)(intptr_t)i);
    for (int i = 0; i < 8; ++i) pthread_join(threads[i], NULL);
    runtime_log_flush();
    runtime_log_release();
    FILE *file = fopen(path, "rb");
    if (!file) return 2;
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), file)) {
        const size_t length = strlen(line);
        const int ok = (length == 35 || length == 36) &&
                       (line[0] == 'T' || line[0] == 'D') &&
                       strstr(line, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != NULL;
        if (!ok) {
            fprintf(stderr, "corrupt line: %s", line);
            fclose(file);
            return 3;
        }
        ++count;
    }
    fclose(file);
    if (count != 2400) {
        fprintf(stderr, "line count %d\n", count);
        return 4;
    }
    return 0;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_log.c') (Join-Path $root 'src\runtime_session_log.c') `
    (Join-Path $root 'src\compat_win.c') '-pthread' '-lbcrypt' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the logging test' }
$env:PATH = "$($tc.Bin);$env:PATH"
# The runtime creates the log folder; the test receives it as its only argument. Its echo is discarded.
& $exe $logs > $null 2>&1
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
