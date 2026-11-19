# Headless Move state test: no controller is exposed, handles are synthetic and distinct, and errors
# match the shadPS4 semantics. No game, launcher, GPU or controller is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_move_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$source = Join-Path $temp 'test_move.c'
@'
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *name; void *function; } RuntimeExport;
static unsigned notes;
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_history_note(const char *api, const char *library, uintptr_t caller) {
    (void)api; (void)library; (void)caller; ++notes;
}
int runtime_log_printf(const char *format, ...) { (void)format; return 0; }

uintptr_t runtime_move_resolve(const char *name);
#define ABI __attribute__((sysv_abi))
static int failures;

typedef struct { float sphere_radius; float accelerometer_offset[3]; } DeviceInfo;
typedef struct { unsigned char bytes[64]; } Data;
typedef struct { unsigned char bytes[20]; } Extension;
typedef int32_t (ABI *InitFn)(void);
typedef int32_t (ABI *OpenFn)(int32_t, int32_t, int32_t);
typedef int32_t (ABI *InfoFn)(int32_t, DeviceInfo *);
typedef int32_t (ABI *LatestFn)(int32_t, Data *);
typedef int32_t (ABI *RecentFn)(int32_t, int64_t, Data *, int32_t *);
typedef int32_t (ABI *ExtensionFn)(int32_t, Extension *);
typedef int32_t (ABI *ByteFn)(int32_t, uint8_t);
typedef int32_t (ABI *LightFn)(int32_t, uint8_t, uint8_t, uint8_t);
typedef int32_t (ABI *HandleFn)(int32_t);
typedef int32_t (ABI *VoidFn)(void);

#define MOVE_NO_CONTROLLER ((int32_t)1)
#define MOVE_NOT_INIT ((int32_t)0x80EE0001)
#define MOVE_ALREADY_INIT ((int32_t)0x80EE0002)
#define MOVE_INVALID_ARG ((int32_t)0x80EE0003)

#define CHECK(condition, text) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", text); ++failures; } \
} while (0)

static int unchanged(const void *now, const void *before, size_t size) {
    return !memcmp(now, before, size);
}

int main(void) {
    InitFn init = (InitFn)runtime_move_resolve("sceMoveInit");
    OpenFn open = (OpenFn)runtime_move_resolve("sceMoveOpen");
    InfoFn info = (InfoFn)runtime_move_resolve("sceMoveGetDeviceInfo");
    LatestFn latest = (LatestFn)runtime_move_resolve("sceMoveReadStateLatest");
    RecentFn recent = (RecentFn)runtime_move_resolve("sceMoveReadStateRecent");
    ExtensionFn extension = (ExtensionFn)runtime_move_resolve("sceMoveGetExtensionPortInfo");
    ByteFn vibration = (ByteFn)runtime_move_resolve("sceMoveSetVibration");
    LightFn light = (LightFn)runtime_move_resolve("sceMoveSetLightSphere");
    HandleFn reset_light = (HandleFn)runtime_move_resolve("sceMoveResetLightSphere");
    HandleFn close_handle = (HandleFn)runtime_move_resolve("sceMoveClose");
    VoidFn term = (VoidFn)runtime_move_resolve("sceMoveTerm");
    if (!init || !open || !info || !latest || !recent || !extension || !vibration || !light ||
        !reset_light || !close_handle || !term) {
        fprintf(stderr, "missing Move export\n");
        return 2;
    }

    DeviceInfo device;
    memset(&device,0,sizeof(device));
    CHECK(info(1, &device) == MOVE_NOT_INIT, "GetDeviceInfo before init");
    CHECK(open(0, 0, 0) == MOVE_NOT_INIT, "Open before init");
    CHECK(init() == 0, "first Init succeeds");
    CHECK(init() == MOVE_ALREADY_INIT, "second Init reports ALREADY_INIT");

    const int32_t first = open(0, 0, 0), second = open(0, 0, 1);
    CHECK(first > 0 && second > 0, "Open returns non-negative handles");
    CHECK(first != second, "Open returns distinct handles");

    unsigned char before[64];
    memset(&device, 0xA5, sizeof(device));
    memcpy(before, &device, sizeof(device));
    CHECK(info(first, NULL) == MOVE_INVALID_ARG, "GetDeviceInfo rejects a null output");
    CHECK(info(first, &device) == MOVE_NO_CONTROLLER, "GetDeviceInfo reports no controller");
    CHECK(unchanged(&device, before, sizeof(device)), "GetDeviceInfo leaves the output untouched");

    Data data;
    memset(&data, 0xA5, sizeof(data));
    memcpy(before, &data, sizeof(data));
    CHECK(latest(first, NULL) == MOVE_INVALID_ARG, "ReadStateLatest rejects a null output");
    CHECK(latest(first, &data) == MOVE_NO_CONTROLLER, "ReadStateLatest reports no controller");
    CHECK(unchanged(&data, before, sizeof(data)), "ReadStateLatest leaves the output untouched");

    int32_t count = 7;
    CHECK(recent(first, -1, &data, &count) == MOVE_INVALID_ARG, "ReadStateRecent rejects a negative timestamp");
    CHECK(recent(first, 0, NULL, &count) == MOVE_INVALID_ARG, "ReadStateRecent rejects null data");
    CHECK(recent(first, 0, &data, NULL) == MOVE_INVALID_ARG, "ReadStateRecent rejects null count");
    CHECK(count == 7, "ReadStateRecent leaves the count untouched");
    CHECK(recent(first, 0, &data, &count) == MOVE_NO_CONTROLLER, "ReadStateRecent reports no controller");

    Extension ext;
    memset(&ext, 0xA5, sizeof(ext));
    memcpy(before, &ext, sizeof(ext));
    CHECK(extension(first, NULL) == MOVE_INVALID_ARG, "GetExtensionPortInfo rejects a null output");
    CHECK(extension(first, &ext) == MOVE_NO_CONTROLLER, "GetExtensionPortInfo reports no controller");
    CHECK(unchanged(&ext, before, sizeof(ext)), "GetExtensionPortInfo leaves the output untouched");

    CHECK(vibration(first, 128) == MOVE_NO_CONTROLLER, "SetVibration reports no controller");
    CHECK(light(first, 1, 2, 3) == MOVE_NO_CONTROLLER, "SetLightSphere reports no controller");
    CHECK(reset_light(first) == 0, "ResetLightSphere succeeds when initialized");
    CHECK(close_handle(first) == 0, "Close succeeds when initialized");
    CHECK(term() == 0, "Term succeeds when initialized");
    CHECK(term() == MOVE_NOT_INIT, "second Term reports NOT_INIT");
    CHECK(notes >= 3, "Init, Open and Term enter the history ring");

    if (failures) return 1;
    puts("move: all checks passed");
    return 0;
}
'@ | Set-Content -LiteralPath $source -Encoding ascii

$tc = Resolve-Toolchain 'auto'
$clang = Join-Path $tc.Bin 'clang.exe'
$exe = Join-Path $temp 'test_move.exe'
& $clang '--target=x86_64-w64-mingw32' '-std=c11' '-Wall' '-Wextra' '-Werror' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' `
    '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
    $source (Join-Path $root 'src\runtime_move.c') '-pthread' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'could not compile the Move test' }
$env:PATH = "$($tc.Bin);$env:PATH"
& $exe
$result = $LASTEXITCODE
Remove-Item -LiteralPath $temp -Recurse -Force
exit $result

