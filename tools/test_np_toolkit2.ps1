# Headless NpToolkit2 tests: the runtime's libSceNpToolkit2 layer is compiled with a stub user service; the
# UnityNpToolkit2.prx import list is checked against the generated binding table. No game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_np_toolkit2_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test.c'
@'
#include "runtime.h"
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
void runtime_log_flush(void) { fflush(stdout); }
int runtime_user_player(int32_t user) { return user >= 1 && user <= 4 ? user : 0; }

#define ERR_SIGNED_OUT ((int32_t)0x80550006)
#define ERR_ALREADY ((int32_t)0x80552e03)
#define ERR_NOT_INIT ((int32_t)0x80552e04)
#define ERR_ARGS ((int32_t)0x80552e05)
#define LIB "#libSceNpToolkit2"

typedef void *(ABI *ObjFn)(void *);
typedef int32_t (ABI *IntFn)(const void *);
typedef int32_t (ABI *SubmitFn)(const void *, void *);
typedef const void *(ABI *DataFn)(const void *);
typedef void (ABI *BaseCtor)(void *, uint32_t, uint32_t);
typedef uint32_t (ABI *Getter)(const void *);

static int failures;
static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "ok  " : "fail", name);
    if (!condition) ++failures;
}
static void *resolve(const char *nid) {
    char name[96];
    snprintf(name, sizeof(name), "%s%s", nid, LIB);
    return (void *)runtime_np_toolkit2_resolve(name);
}
static int bytes_are(const unsigned char *p, size_t n, unsigned char value) {
    for (size_t i = 0; i < n; ++i) if (p[i] != value) return 0;
    return 1;
}

enum { GUARD = 64, INIT_SIZE = 0x98, CTOR_NID_SIZE = 7120 };

int main(void) {
    ObjFn init_ctor = resolve("vbh-MWtjMh4"), init_dtor = resolve("-bAjEe-hdCg");
    ObjFn term_ctor = resolve("s0u30pOfi9Q"), term_dtor = resolve("vycUuYWwilI");
    IntFn core_init = resolve("LLdnqVnnNBQ"), core_term = resolve("IcuECtP3L0E");
    check("the six Core entry points resolve", init_ctor && init_dtor && term_ctor && term_dtor && core_init && core_term);

    static unsigned char slot[GUARD + 0x200 + GUARD];
    memset(slot, 0xa5, sizeof(slot));
    unsigned char *params = slot + GUARD;
    check("InitParams constructor returns this", init_ctor(params) == params);
    check("InitParams constructor clears exactly 0x98 bytes", bytes_are(params, INIT_SIZE, 0) && bytes_are(params + INIT_SIZE, 0x200 - INIT_SIZE, 0xa5));
    check("InitParams constructor leaves the neighbours alone", bytes_are(slot, GUARD, 0xa5) && bytes_are(params + 0x200, GUARD, 0xa5));
    params[0x84] = 1;
    check("InitParams destructor returns without writing", init_dtor(params) == params && params[0x84] == 1);

    memset(slot, 0xa5, sizeof(slot));
    check("TermParams constructor returns this", term_ctor(params) == params);
    check("TermParams constructor writes one byte", params[0] == 0 && bytes_are(params + 1, 0x1ff, 0xa5) && bytes_are(slot, GUARD, 0xa5));
    check("TermParams destructor returns this", term_dtor(params) == params);

    unsigned char term[1] = {0};
    check("term before init is not initialized", core_term(term) == ERR_NOT_INIT);
    init_ctor(params);
    check("init without parameters is incorrect arguments", core_init(NULL) == ERR_ARGS);
    *(int32_t *)(params + 0x84) = 0;
    check("init with an unknown user is incorrect arguments", core_init(params) == ERR_ARGS);
    *(int32_t *)(params + 0x84) = 1;
    check("init with the initial user succeeds", core_init(params) == 0);
    check("a second init reports already initialized", core_init(params) == ERR_ALREADY);
    check("term succeeds once", core_term(term) == 0);
    check("a second term reports not initialized", core_term(term) == ERR_NOT_INIT);
    check("init works again after term", core_init(params) == 0);

    ObjFn request_ctor = resolve("HvTorT+TclQ");
    static unsigned char request[GUARD + CTOR_NID_SIZE + GUARD];
    memset(request, 0xa5, sizeof(request));
    check("a heap request constructor clears its proven size and returns this", request_ctor(request + GUARD) == request + GUARD &&
          bytes_are(request + GUARD, CTOR_NID_SIZE, 0) && bytes_are(request, GUARD, 0xa5) && bytes_are(request + GUARD + CTOR_NID_SIZE, GUARD, 0xa5));

    ObjFn response_ctor = resolve("tlpXm5KLgqk"), response_dtor = resolve("30Zxe35LtBg");
    unsigned char response[GUARD + 0x18 + GUARD];
    memset(response, 0xa5, sizeof(response));
    check("Response<Empty> constructor clears 0x18 bytes", response_ctor(response + GUARD) == response + GUARD &&
          bytes_are(response + GUARD, 0x18, 0) && bytes_are(response + GUARD + 0x18, GUARD, 0xa5));
    check("Response<Empty> destructor returns this", response_dtor(response + GUARD) == response + GUARD);

    SubmitFn submit = resolve("blewLPEBcMk");
    IntFn locked = resolve("mIinAn8TvdY"), return_code = resolve("5-UHpg8s00I");
    DataFn server_error = resolve("kxzokJHZ2nY");
    check("a request fails with the signed-out error while initialized", submit && submit(request + GUARD, response + GUARD) == ERR_SIGNED_OUT);
    check("a response is never locked and carries no error", locked && locked(response + GUARD) == 0 && return_code(response + GUARD) == 0);
    const unsigned char *empty = server_error ? server_error(response + GUARD) : NULL;
    check("result accessors return an empty block", empty && bytes_are(empty, 0x100, 0));

    BaseCtor base_ctor = resolve("+wKsYq75qr8");
    Getter function_type = resolve("El4tFb9f8dk"), service_type = resolve("lzIgWzHltTc");
    unsigned char base[16] = {0};
    base_ctor(base, 3, 5);
    check("RequestBase keeps its service and function type", service_type(base) == 3 && function_type(base) == 5);

    check("an NpToolkit2 import with an unknown ABI stays unresolved", resolve("1QxBMWqn7ZU") == NULL && resolve("AAAAAAAAAAA") == NULL);
    char scoped[96];
    snprintf(scoped, sizeof(scoped), "vbh-MWtjMh4%s#libSceNpToolkit2", LIB);
    check("the module part of a dynamic import name is ignored", runtime_np_toolkit2_resolve(scoped) == (uintptr_t)init_ctor);
    check("another library is not answered", runtime_np_toolkit2_resolve("vbh-MWtjMh4#libSceNpManager") == 0);
    const char *symbol = runtime_np_toolkit2_symbol("vbh-MWtjMh4#libSceNpToolkit2");
    check("imports are named symbolically", symbol && strstr(symbol, "InitParams::InitParams()"));
    check("an unbound import still has its symbol", runtime_np_toolkit2_symbol("1QxBMWqn7ZU#libSceNpToolkit2") != NULL);

    runtime_np_toolkit2_reset();
    check("a request before init is not initialized", submit(request + GUARD, response + GUARD) == ERR_NOT_INIT);
    return failures == 0 ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_np_toolkit2.c') '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the NpToolkit2 test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force

$python = (Get-Command python -ErrorAction SilentlyContinue)
if ($python) {
    & $python.Source (Join-Path $root 'scripts\np_toolkit2_check.py')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
} else {
    Write-Host '  [warn] python is not installed; the plugin import coverage check was skipped'
}
exit $result
