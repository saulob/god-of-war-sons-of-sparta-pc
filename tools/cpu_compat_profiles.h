/* Processor profiles and the check of an installed table, shared by tools\cpu_compat_tests.c and
 * tools\cpu_compat_module_tests.c. */
#ifndef SOS_CPU_COMPAT_PROFILES_H
#define SOS_CPU_COMPAT_PROFILES_H

#include <stdint.h>
#include <string.h>

#include "cpu_compat.h"

enum {
    L1_SSE3 = 1u << 0, L1_SSSE3 = 1u << 9, L1_FMA = 1u << 12, L1_CX16 = 1u << 13, L1_SSE41 = 1u << 19, L1_SSE42 = 1u << 20,
    L1_MOVBE = 1u << 22, L1_POPCNT = 1u << 23, L1_AES = 1u << 25, L1_XSAVE = 1u << 26, L1_OSXSAVE = 1u << 27,
    L1_AVX = 1u << 28, L1_F16C = 1u << 29, L1_RDRAND = 1u << 30,
    L7_BMI1 = 1u << 3, L7_AVX2 = 1u << 5, L7_BMI2 = 1u << 8,
    E1_LAHF = 1u << 0, E1_LZCNT = 1u << 5, E1_SSE4A = 1u << 6, E1_XOP = 1u << 11, E1_FMA4 = 1u << 16, E1_TBM = 1u << 21,
};
#define BASE_L1 (L1_SSE3 | L1_SSSE3 | L1_CX16 | L1_SSE41 | L1_SSE42 | L1_POPCNT)
#define SANDY_L1 (BASE_L1 | L1_AES | L1_XSAVE | L1_OSXSAVE | L1_AVX)
#define HASWELL_L1 (SANDY_L1 | L1_FMA | L1_MOVBE | L1_F16C | L1_RDRAND)

typedef struct {
    const char *name;
    IsaHostFeatures host;
    int usable;
    const char *required_missing, *handled;
    unsigned compat;
} Profile;

static const Profile profiles[] = {
    {"Haswell (no SSE4a)", {HASWELL_L1, L7_BMI1 | L7_AVX2 | L7_BMI2, E1_LAHF | E1_LZCNT}, 1, "", "SSE4a", 0},
    {"Ivy Bridge", {SANDY_L1 | L1_F16C | L1_RDRAND, 0, E1_LAHF}, 1, "", "MOVBE, BMI1, LZCNT, SSE4a",
     CPU_COMPAT_LZCNT | CPU_COMPAT_TZCNT | CPU_COMPAT_BMI1 | CPU_COMPAT_MOVBE},
    {"Sandy Bridge", {SANDY_L1, 0, E1_LAHF}, 1, "", "MOVBE, BMI1, LZCNT, F16C, SSE4a", CPU_COMPAT_ALL},
    {"Sandy Bridge without AES-NI", {SANDY_L1 & ~L1_AES, 0, E1_LAHF}, 1, "", "MOVBE, BMI1, LZCNT, F16C, SSE4a", CPU_COMPAT_ALL},
    {"AMD FX Bulldozer", {SANDY_L1, 0, E1_LAHF | E1_LZCNT | E1_SSE4A | E1_XOP | E1_FMA4}, 1, "", "MOVBE, BMI1, F16C",
     CPU_COMPAT_TZCNT | CPU_COMPAT_BMI1 | CPU_COMPAT_F16C | CPU_COMPAT_MOVBE},
    {"AMD FX Piledriver", {SANDY_L1 | L1_FMA | L1_F16C, L7_BMI1, E1_LAHF | E1_LZCNT | E1_SSE4A | E1_XOP | E1_FMA4 | E1_TBM}, 1, "",
     "MOVBE", CPU_COMPAT_MOVBE},
    {"AVX without AVX2", {HASWELL_L1, L7_BMI1 | L7_BMI2, E1_LAHF | E1_LZCNT}, 1, "", "SSE4a", 0},
    {"modern Intel with AVX2", {HASWELL_L1, L7_BMI1 | L7_AVX2 | L7_BMI2, E1_LAHF | E1_LZCNT}, 1, "", "SSE4a", 0},
    {"modern AMD with AVX2", {HASWELL_L1, L7_BMI1 | L7_AVX2 | L7_BMI2, E1_LAHF | E1_LZCNT | E1_SSE4A}, 1, "", "", 0},
    {"Westmere, no AVX", {BASE_L1 | L1_AES, 0, E1_LAHF}, 0, "AVX", "MOVBE, BMI1, LZCNT, F16C, SSE4a", CPU_COMPAT_ALL},
    {"AVX without OS support", {BASE_L1 | L1_AES | L1_XSAVE, 0, E1_LAHF}, 0, "AVX", "MOVBE, BMI1, LZCNT, F16C, SSE4a",
     CPU_COMPAT_ALL},
    {"Haswell without MOVBE", {HASWELL_L1 & ~L1_MOVBE, L7_BMI1 | L7_AVX2, E1_LZCNT}, 1, "", "MOVBE, SSE4a", CPU_COMPAT_MOVBE},
    {"Haswell without BMI1", {HASWELL_L1, L7_AVX2, E1_LZCNT}, 1, "", "BMI1, SSE4a", CPU_COMPAT_TZCNT | CPU_COMPAT_BMI1},
    {"Haswell without LZCNT", {HASWELL_L1, L7_BMI1 | L7_AVX2, 0}, 1, "", "LZCNT, SSE4a", CPU_COMPAT_LZCNT},
    {"Haswell without F16C", {HASWELL_L1 & ~L1_F16C, L7_BMI1 | L7_AVX2, E1_LZCNT}, 1, "", "F16C, SSE4a", CPU_COMPAT_F16C},
    {"Haswell with SSE4a", {HASWELL_L1, L7_BMI1 | L7_AVX2, E1_LZCNT | E1_SSE4A}, 1, "", "", 0},
    {"Haswell without POPCNT", {HASWELL_L1 & ~L1_POPCNT, L7_BMI1 | L7_AVX2, E1_LZCNT}, 0, "POPCNT", "SSE4a", 0},
    {"Haswell without CMPXCHG16B", {HASWELL_L1 & ~L1_CX16, L7_BMI1 | L7_AVX2, E1_LZCNT}, 0, "CMPXCHG16B", "SSE4a", 0},
};

static inline unsigned table_parts(const CpuCompatTable *t, unsigned kinds) {
    unsigned n = 0;
    for (size_t i = 0; i < t->count; ++i)
        for (unsigned p = 0; p < CPU_COMPAT_PARTS; ++p)
            n += t->sites[i].parts[p] >> 4 && ((1u << ((t->sites[i].parts[p] >> 4) - 1)) & kinds);
    return n;
}

enum { STUB_FILL = 0x5a };

/* After an install of `kinds`: each site holding one of them is a jmp into the stubs followed by int3, every other byte
 * of the image is the original, and the count is the table's. */
static inline int installed_as_planned(const CpuCompatTable *table, unsigned kinds, const uint8_t *pristine, const uint8_t *image,
                                uint64_t size, const uint8_t *stubs, size_t stubs_size, size_t replaced, unsigned *sites) {
    uint64_t at = 0;
    int ok = replaced == table_parts(table, kinds);
    *sites = 0;
    for (size_t i = 0; i < table->count && ok; ++i) {
        const CpuCompatSite *s = &table->sites[i];
        ok &= !memcmp(image + at, pristine + at, (size_t)(s->offset - at));
        if (cpu_compat_site_kinds(s) & kinds) {
            int32_t rel;
            memcpy(&rel, image + s->offset + 1, 4);
            const uint8_t *to = image + s->offset + 5 + rel;
            ok &= image[s->offset] == 0xe9 && to >= stubs && to < stubs + stubs_size;
            for (unsigned k = 5; k < s->length; ++k) ok &= image[s->offset + k] == 0xcc;
            ++*sites;
        } else {
            ok &= !memcmp(image + s->offset, pristine + s->offset, s->length);
        }
        at = s->offset + s->length;
    }
    return ok && !memcmp(image + at, pristine + at, (size_t)(size - at));
}

static inline int untouched_stubs(const uint8_t *stubs, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (stubs[i] != STUB_FILL) return 0;
    return 1;
}

#endif
