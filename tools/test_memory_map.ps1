# Headless guest memory map tests. The runtime memory layer is compiled with small stubs for logging and file reads;
# no game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_memory_map_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
@'
#include "runtime.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { FILE_SIZE = 40000 };
int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
void runtime_log_flush(void) { fflush(stdout); }
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) {
    (void)api; (void)caller; _Exit(status);
}
FILE *sos_stderr_stream(void) { return stdout; }
int32_t *runtime_errno(void) { static int32_t value; return &value; }
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset) {
    if (fd != 7) return -9;
    unsigned char *bytes = buffer;
    int64_t count = 0;
    for (; (uint64_t)count < size && offset + count < FILE_SIZE; ++count) bytes[count] = (unsigned char)((offset + count) * 7 + 3);
    return count;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef void *(ABI *MmapFn)(void *, uint64_t, int, int, int, int64_t);
typedef int32_t (ABI *UnmapFn)(void *, uint64_t);
typedef int32_t (ABI *FlexFn)(void **, uint64_t, int, int, const char *);
typedef int32_t (ABI *ReserveFn)(void **, uint64_t, int, uint64_t);
typedef int32_t (ABI *ProtectFn)(const void *, uint64_t, int);
typedef int32_t (ABI *AvailFn)(uint64_t *);

enum { MAP_FIXED_FLAG = 0x10, MAP_NO_OVERWRITE_FLAG = 0x80, MAP_ANON_FLAG = 0x1000, MAP_PRIVATE_FLAG = 0x2 };
#define EINVAL_SCE ((int32_t)0x80020016)
#define ENOMEM_SCE ((int32_t)0x8002000c)
static int failures;
static void check(const char *name, int condition) {
    if (condition) {
        printf("  [ok]   %s\n", name);
    } else {
        printf("  [fail] %s\n", name);
        ++failures;
    }
}
static void *resolve(const char *name) { return (void *)runtime_memory_resolve(name); }

int main(void) {
    runtime_memory_reserve();
    MmapFn mmap_fn = (MmapFn)resolve("mmap");
    UnmapFn munmap_fn = (UnmapFn)resolve("munmap");
    FlexFn flex = (FlexFn)resolve("sceKernelMapNamedFlexibleMemory");
    ReserveFn reserve = (ReserveFn)resolve("sceKernelReserveVirtualRange");
    ProtectFn protect = (ProtectFn)resolve("sceKernelMprotect");
    AvailFn available = (AvailFn)resolve("sceKernelAvailableFlexibleMemorySize");
    if (!mmap_fn || !munmap_fn || !flex || !reserve || !protect || !available) { puts("  [fail] memory exports are missing"); return 1; }

    const uint64_t page = 16384;
    unsigned char *anon = mmap_fn(NULL, 2 * page, 3, MAP_ANON_FLAG | MAP_PRIVATE_FLAG, -1, 0);
    check("anonymous mmap succeeds", anon != (void *)-1 && anon != NULL);
    int zero = 1;
    for (uint64_t i = 0; i < 2 * page; ++i) zero &= anon[i] == 0;
    check("anonymous mmap is zeroed", zero);
    anon[page + 5] = 0x5a;
    check("anonymous mmap is writable", anon[page + 5] == 0x5a);
    check("anonymous mapping is tracked", runtime_memory_is_mapped((uintptr_t)anon, 2 * page));
    check("anonymous mmap with a descriptor is refused", mmap_fn(NULL, page, 3, MAP_ANON_FLAG | MAP_PRIVATE_FLAG, 7, 0) == (void *)-1);

    unsigned char *file = mmap_fn(NULL, FILE_SIZE, 1, MAP_PRIVATE_FLAG, 7, 0);
    check("file mmap succeeds", file != (void *)-1 && file != NULL);
    int match = 1;
    for (int i = 0; i < FILE_SIZE; ++i) match &= file[i] == (unsigned char)(i * 7 + 3);
    check("file mmap holds the file bytes", match);
    int tail = 1;
    for (uint64_t i = FILE_SIZE; i < 3 * page; ++i) tail &= file[i] == 0;
    check("file mmap is zero past the end of the file", tail);
    unsigned char *offset_map = mmap_fn(NULL, page, 1, MAP_PRIVATE_FLAG, 7, (int64_t)page);
    check("file mmap at an offset starts at that offset",
          offset_map != (void *)-1 && offset_map[0] == (unsigned char)(page * 7 + 3));
    check("file mmap with a misaligned offset is refused", mmap_fn(NULL, page, 1, MAP_PRIVATE_FLAG, 7, 100) == (void *)-1);

    void *fixed = anon;
    check("fixed flexible map without MAP_NO_OVERWRITE_FLAG replaces the mapping",
          flex(&fixed, page, 3, MAP_FIXED_FLAG, "test") == 0 && fixed == (void *)anon);
    fixed = anon + page;
    check("fixed flexible map with MAP_NO_OVERWRITE_FLAG over a mapping is refused",
          flex(&fixed, page, 3, MAP_FIXED_FLAG | MAP_NO_OVERWRITE_FLAG, "test") == ENOMEM_SCE);
    void *reserved = anon;
    check("fixed reserve over a mapping replaces it", reserve(&reserved, page, MAP_FIXED_FLAG, 0) == 0 && reserved == (void *)anon);
    check("a reserved range is not mapped memory", !runtime_memory_is_mapped((uintptr_t)anon, page));
    reserved = anon + page;
    check("fixed reserve with MAP_NO_OVERWRITE_FLAG over a mapping is refused",
          reserve(&reserved, page, MAP_FIXED_FLAG | MAP_NO_OVERWRITE_FLAG, 0) == ENOMEM_SCE);
    reserved = anon + 8 * page;
    check("reserve with a null fixed address is invalid", (reserved = NULL, reserve(&reserved, page, MAP_FIXED_FLAG, 0)) == EINVAL_SCE);

    check("mprotect of mapped memory succeeds", protect(anon + page, page, 1) == 0);
    check("mprotect of unmapped memory is refused", protect((void *)(anon + 1024 * page), page, 3) != 0);
    check("munmap succeeds", munmap_fn(anon + page, page) == 0);
    check("munmapped range is no longer mapped", !runtime_memory_is_mapped((uintptr_t)(anon + page), page));
    check("file mapping munmap succeeds", munmap_fn(file, 3 * page) == 0);
    check("munmapped file range is gone", !runtime_memory_is_mapped((uintptr_t)file, page));

    uint64_t left = 0;
    check("available flexible memory is reported", available(&left) == 0 && left > 0);
    void *huge = NULL;
    check("a flexible map larger than the flexible pool is invalid", flex(&huge, left + page, 3, 0, "huge") == EINVAL_SCE);
    return failures == 0 ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_memory.c') (Join-Path $PSScriptRoot 'startup_profile_stubs.c') '-pthread' '-Wl,--disable-high-entropy-va' '-lmincore' '-lpsapi' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the memory map test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
