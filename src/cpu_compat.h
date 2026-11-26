/* Load-time compatibility for processors without the Jaguar instructions that do not trap: LZCNT and TZCNT run as BSR
 * and BSF there and give other results; F16C, which does trap, is also handled here since the exception context holds no
 * ymm state. Each known image has a generated table of sites (cpu_compat_sites.inc); a site is whole instructions, at
 * least a jmp rel32 long, replaced by a jump to a stub that runs them. Every site is checked before anything is written:
 * an image that does not match its table is left untouched. Nothing is written to the game's files. */
#ifndef SOS_CPU_COMPAT_H
#define SOS_CPU_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "isa_emulate.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CPU_COMPAT_LZCNT = 1,
    CPU_COMPAT_TZCNT = 2,
    CPU_COMPAT_F16C = 4,
    CPU_COMPAT_MOVBE = 8,
    CPU_COMPAT_BMI1 = 16,
    CPU_COMPAT_ALL = 31
};

enum { CPU_COMPAT_SITE_MAX = 15, CPU_COMPAT_PARTS = 4, CPU_COMPAT_STUB_STRIDE = 128, CPU_COMPAT_COMMON_BYTES = 512 };

/* A part is (kind << 4) | length: kind 0 for an instruction moved as it is, n for CPU_COMPAT_* bit 1 << (n - 1). */
#define CPU_COMPAT_PART(kind, length) ((uint8_t)((kind) << 4 | (length)))

typedef struct {
    uint32_t offset;
    uint8_t length;
    uint8_t shield; /* a moved instruction accesses memory: the stub runs below the red zone */
    uint8_t parts[CPU_COMPAT_PARTS];
} CpuCompatSite;

/* The CPU_COMPAT_* bits of a site's parts. */
unsigned cpu_compat_site_kinds(const CpuCompatSite *site);

typedef struct {
    const char *name; /* a build key for a boot image, a file name for a module loaded at run time */
    uint64_t size;    /* the image's or the module's mapped size */
    uint64_t digest;  /* cpu_compat_digest of the sites' bytes */
    const CpuCompatSite *sites;
    size_t count;
} CpuCompatTable;

typedef enum {
    CPU_COMPAT_OK = 0,
    CPU_COMPAT_UNKNOWN_IMAGE, /* no table, or a size that is not the table's */
    CPU_COMPAT_BAD_SITE,      /* bytes differ, sites overlap, leave the image or hold an unsupported form */
    CPU_COMPAT_NO_ROOM        /* the stub area is too small, too far from the image, or out of emulated sites */
} CpuCompatStatus;

const char *cpu_compat_status_text(CpuCompatStatus status);

/* CPU_COMPAT_* bits of the instructions the host runs differently or not at all. */
unsigned cpu_compat_needed(IsaHostFeatures host);
/* SOS_CPU_COMPAT=force (test): every kind, as on a host that has none of them. */
unsigned cpu_compat_forced(void);

const CpuCompatTable *cpu_compat_boot_table(const char *build_key);
/* By file name alone: a known module whose size or bytes differ is then refused, not taken for one without sites. */
const CpuCompatTable *cpu_compat_module_table(const char *file_name);
/* Tables for the tests. */
size_t cpu_compat_table_count(void);
const CpuCompatTable *cpu_compat_table(size_t index);

/* FNV-1a 64 of every site's position, kind and bytes in `image`. */
uint64_t cpu_compat_digest(const uint8_t *image, uint64_t size, const CpuCompatSite *sites, size_t count);

/* The instruction at `code` (available bytes) is a supported form of `kind`; its length, 0 otherwise. */
size_t cpu_compat_instruction(const uint8_t *code, size_t available, unsigned kind);

/* Stub bytes `kinds` of the table need, its shared emulation routine included. */
size_t cpu_compat_stub_bytes(const CpuCompatTable *table, unsigned kinds);

/* Checks every site of the table against the image without writing. */
CpuCompatStatus cpu_compat_check(const uint8_t *image, uint64_t size, const CpuCompatTable *table);

/* Checks every site, then replaces those of `kinds` with jumps to stubs written to `stubs`, which must lie within
 * 2 GiB of the image and be made executable by the caller once this returns. Nothing is written on failure. *replaced
 * gets the number of instructions replaced. */
CpuCompatStatus cpu_compat_apply(uint8_t *image, uint64_t size, const CpuCompatTable *table, unsigned kinds,
                                 uint8_t *stubs, size_t stubs_size, size_t *replaced);

/* Comma-separated names of the CPU_COMPAT_* bits in `kinds`. */
void cpu_compat_kind_names(unsigned kinds, char *out, size_t size);

/* 1 when the host has what the game's code needs natively (AVX, POPCNT, CMPXCHG16B); otherwise 0 with what is wrong
 * and what to do about it. */
int cpu_compat_processor_usable(IsaHostFeatures host, char *cause, size_t cause_size, char *advice, size_t advice_size);

typedef enum { CPU_COMPAT_BOOT_IMAGE, CPU_COMPAT_MODULE } CpuCompatScope;

typedef struct {
    unsigned kinds;     /* what the host lacks, plus what is forced */
    unsigned wanted;    /* of those, what the image may hold: every kind for a boot image, its table's for a module */
    unsigned essential; /* wanted kinds the game cannot run correctly without: LZCNT, TZCNT, F16C */
    unsigned forced;
    const CpuCompatTable *table;
    size_t stub_bytes;
} CpuCompatPlan;

/* What the loader does for the boot image of build `name`, or the module loader for module file `name`, on `host`. */
CpuCompatPlan cpu_compat_plan(CpuCompatScope scope, const char *name, IsaHostFeatures host, unsigned forced);

typedef struct {
    CpuCompatStatus status;
    size_t replaced;
    int fatal;          /* the image cannot run correctly on this host as it is */
    char message[256];  /* the log line, empty when nothing was needed */
} CpuCompatOutcome;

/* Carries out a plan on the mapped image with its stubs at `stubs`; nothing is written unless it succeeds. */
CpuCompatOutcome cpu_compat_install(const CpuCompatPlan *plan, uint8_t *image, uint64_t size, uint8_t *stubs,
                                    size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
