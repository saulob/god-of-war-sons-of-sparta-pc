# Headless test of the unwind-table lookup behind sceKernelGetModuleInfoForUnwind and of the PS4 libc stop-code names.
# The module layer is compiled with small stubs; no game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_unwind_info_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
@'
#include "runtime.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
FILE *sos_stderr_stream(void) { return stdout; }
void runtime_log_exit_request(const char *api, int status, uintptr_t caller) { (void)api; (void)status; (void)caller; }
void runtime_log_flush(void) {}
const char *sos_startup_phase_current(void) { return "test"; }
void runtime_describe_guest_address(uintptr_t address, char *out, size_t size) { (void)address; snprintf(out, size, "test"); }
uint64_t runtime_add_module_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align) { (void)data; (void)filesz; (void)memsz; (void)align; return 0; }
void runtime_remove_module_tls(uint64_t module) { (void)module; }
uint32_t runtime_thread_tcb_offset(void) { return 0; }
uint32_t runtime_thread_thread_offset(void) { return 0; }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
static unsigned char self_file[512];
static size_t self_size;
int64_t runtime_file_open(const char *path, int flags, int mode) {
    (void)flags; (void)mode;
    return strstr(path, "Headers.prx") ? 7 : -2;
}
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
int64_t runtime_file_lseek(int fd, int64_t offset, int whence) { (void)fd; (void)offset; (void)whence; return -29; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) {
    if (fd != 7) return -1;
    const size_t n = size < self_size ? (size_t)size : self_size;
    memcpy(buffer, self_file, n);
    return (int64_t)n;
}
uintptr_t runtime_resolve(const char *name, int is_data) { (void)name; (void)is_data; return 0; }
void runtime_symbol_to_nid(const char *symbol, char *out) { (void)symbol; out[0] = 0; }
const char *runtime_import_name(const char *name) { return name; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef int32_t (ABI *UnwindFn)(uint64_t, int32_t, void *);
typedef struct { uint64_t size; char name[256]; uint64_t hdr, frames, frames_size, seg0, seg0_size; } Info;
static int failures;
static void check(const char *name, int condition) {
    printf(condition ? "  [ok]   %s\n" : "  [fail] %s\n", name);
    if (!condition) ++failures;
}

/* A SELF holding only program headers: PT_LOAD (rx) for the first 0x30000 bytes and PT_GNU_EH_FRAME. */
static void build_self(uint32_t hdr, uint32_t hdr_size) {
    memset(self_file, 0, sizeof(self_file));
    const uint32_t magic = 0x1d3d154f;
    memcpy(self_file, &magic, 4);
    const uint64_t elf = 32;
    memcpy(self_file + elf, """ELF", 7);
    const uint16_t machine = 62, phentsize = 56, phnum = 2;
    const uint64_t phoff = 64;
    memcpy(self_file + elf + 18, &machine, 2);
    memcpy(self_file + elf + 32, &phoff, 8);
    memcpy(self_file + elf + 54, &phentsize, 2);
    memcpy(self_file + elf + 56, &phnum, 2);
    const struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } ph[2] = {
        {1, 5, 0, 0, 0, 0x30000, 0x30000, 0x4000}, {0x6474e550u, 4, 0, hdr, 0, hdr_size, hdr_size, 4}};
    memcpy(self_file + elf + phoff, ph, sizeof(ph));
    self_size = sizeof(self_file);
}
static void write_table(unsigned char *base, uint32_t frames, uint32_t hdr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *fde = base + frames + 0x40 * i;
        const uint32_t length = 0x30, cie = 0x10;
        memcpy(fde, &length, 4);
        memcpy(fde + 4, &cie, 4);
    }
    base[hdr] = 1; base[hdr + 1] = 0x1b; base[hdr + 2] = 0x03; base[hdr + 3] = 0x3b;
    const int32_t frames_rel = (int32_t)frames - (int32_t)(hdr + 4);
    memcpy(base + hdr + 4, &frames_rel, 4);
    memcpy(base + hdr + 8, &count, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t pc = (int32_t)(0x20000 + 0x100 * i) - (int32_t)hdr;
        const int32_t fde = (int32_t)(frames + 0x40 * i) - (int32_t)hdr;
        memcpy(base + hdr + 12 + 8 * i, &pc, 4);
        memcpy(base + hdr + 16 + 8 * i, &fde, 4);
    }
}
/* What the unwinder does with the reply: binary search of the table for the pc, then a structural check of the FDE. */
static int find_fde(const Info *info, uint64_t pc) {
    const unsigned char *h = (const unsigned char *)(uintptr_t)info->hdr;
    uint32_t count;
    memcpy(&count, h + 8, 4);
    uint32_t lo = 0, hi = count;
    while (lo + 1 < hi) {
        const uint32_t mid = (lo + hi) / 2;
        int32_t entry;
        memcpy(&entry, h + 12 + 8 * mid, 4);
        if ((uint64_t)((int64_t)info->hdr + entry) <= pc) lo = mid; else hi = mid;
    }
    int32_t fde_rel;
    memcpy(&fde_rel, h + 16 + 8 * lo, 4);
    const unsigned char *fde = (const unsigned char *)(uintptr_t)((int64_t)info->hdr + fde_rel);
    uint32_t length, cie;
    memcpy(&length, fde, 4);
    memcpy(&cie, fde + 4, 4);
    return (uint64_t)(uintptr_t)fde >= info->frames && (uint64_t)(uintptr_t)fde < info->frames + info->frames_size &&
           length != 0 && length != 0xffffffffu && cie != 0;
}

int main(void) {
    const size_t size = 1 << 20;
    unsigned char *base = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) return 1;
    const uint32_t frames = 0x1000, hdr = 0x8000, count = 32;
    write_table(base, frames, hdr, count);
    check("the module registers", runtime_module_register_linked("/app0/Media/Modules/Fake.prx", base, size, 0x100000) >= 0);

    UnwindFn kernel = (UnwindFn)runtime_module_resolve("sceKernelGetModuleInfoForUnwind");
    UnwindFn sysmodule = (UnwindFn)runtime_module_resolve("sceSysmoduleGetModuleInfoForUnwind");
    check("both unwind exports resolve to the same lookup", kernel && kernel == sysmodule);
    Info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    check("the structure is the 0x130 bytes libc passes", sizeof(info) == 0x130);
    const int32_t result = kernel((uint64_t)(uintptr_t)(base + 0x20100), 1, &info);
    check("an address inside the module is found", result == 0);
    check("the module name is the file name", !strcmp(info.name, "Fake.prx"));
    check("eh_frame_hdr points at the table", info.hdr == (uint64_t)(uintptr_t)(base + hdr));
    check("eh_frame follows eh_frame_ptr", info.frames == (uint64_t)(uintptr_t)(base + frames));
    check("eh_frame size runs up to the header", info.frames_size == hdr - frames);
    check("segment 0 is the module", info.seg0 == (uint64_t)(uintptr_t)base && info.seg0_size == size);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    check("an address outside any module is ESRCH", kernel(0x1234, 1, &info) == (int32_t)0x80020003);
    check("flags >= 3 are EINVAL", kernel((uint64_t)(uintptr_t)base, 3, &info) == (int32_t)0x80020016);
    info.size = 8;
    check("a short structure is EINVAL", kernel((uint64_t)(uintptr_t)base, 1, &info) == (int32_t)0x80020016);
    check("a null structure is EFAULT", kernel((uint64_t)(uintptr_t)base, 1, NULL) == (int32_t)0x8002000e);

    info.size = sizeof(info);
    kernel((uint64_t)(uintptr_t)(base + 0x20100), 1, &info);
    check("the FDE for a pc is found and structurally valid", find_fde(&info, (uint64_t)(uintptr_t)(base + 0x20100)));

    /* A second module whose headers come from its file: exact hdr and first executable segment, no signature scan. */
    unsigned char *second = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    write_table(second, 0x2000, 0x9000, count);
    build_self(0x9000, 12 + 8 * count);
    check("a module with a program-header file registers", runtime_module_register_linked("/app0/Media/Modules/Headers.prx", second, size, 0x300000) >= 0);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    check("headers module found", kernel((uint64_t)(uintptr_t)(second + 0x20100), 1, &info) == 0);
    check("eh_frame_hdr comes from PT_GNU_EH_FRAME", info.hdr == (uint64_t)(uintptr_t)(second + 0x9000));
    check("eh_frame follows eh_frame_ptr (headers)", info.frames == (uint64_t)(uintptr_t)(second + 0x2000));
    check("segment 0 is the first executable PT_LOAD", info.seg0 == (uint64_t)(uintptr_t)second && info.seg0_size == 0x30000);

    check("0xa0020007 is LIBC_ASSERT", !strcmp(runtime_debug_stop_name(0xa0020007u), "LIBC_ASSERT"));
    check("0xa0020008 is LIBC_TERMINATE", !strcmp(runtime_debug_stop_name(0xa0020008u), "LIBC_TERMINATE"));
    check("0xa0020009 is LIBC_UNEXPECTED", !strcmp(runtime_debug_stop_name(0xa0020009u), "LIBC_UNEXPECTED"));
    check("0xa002000a is LIBC_PURE_VIRTUAL", !strcmp(runtime_debug_stop_name(0xa002000au), "LIBC_PURE_VIRTUAL"));
    check("0xa002000b is LIBC_ABORT", !strcmp(runtime_debug_stop_name(0xa002000bu), "LIBC_ABORT"));
    check("other codes are unknown", !strcmp(runtime_debug_stop_name(0x1234u), "unknown"));
    return failures == 0 ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_module.c') (Join-Path $PSScriptRoot 'startup_profile_stubs.c') `
    (Join-Path $root 'src\runtime_stop.c') (Join-Path $root 'src\cpu_compat.c') (Join-Path $root 'src\isa_emulate.c') '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the unwind info test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result
