#include "sos_cheat_patch.h"

#include <pthread.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "sos_cheat_sites.h"

static const SosCheatBuild *const kBuilds[] = {&kCheatBuild_1_00, &kCheatBuild_1_01};

const SosCheatBuild *sos_cheat_build(const char *build_key) {
    if (!build_key) return NULL;
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(kBuilds[i]->build, build_key)) return kBuilds[i];
    return NULL;
}

static uint64_t fnv1a(const uint8_t *data, size_t size) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

static int fail(const char **why, const char *reason) {
    if (why) *why = reason;
    return 0;
}

int sos_cheat_site_verify(const uint8_t *image, uint64_t size, const SosCheatBuild *build, int index, const char **why) {
    if (!image || !build || index < 0 || index >= SOS_CHEAT_SITE_COUNT) return fail(why, "not a supported game build");
    if (size != build->image_size) return fail(why, "the boot image is not the analysed one");
    const SosCheatSite *site = &build->sites[index];
    if (site->length < 5 || site->length > SOS_CHEAT_ATOMIC_BLOCK || site->rva % SOS_CHEAT_ATOMIC_BLOCK + site->length > SOS_CHEAT_ATOMIC_BLOCK)
        return fail(why, "the site is not inside one aligned 16-byte block");
    if (site->method.end > size || site->method.begin > site->rva || site->rva + site->length > site->method.end)
        return fail(why, "the site is outside its method");
    if (memcmp(image + site->rva, site->original, site->length)) return fail(why, "the game code at the site is not the original");
    if (fnv1a(image + site->method.begin, site->method.end - site->method.begin) != site->method_hash)
        return fail(why, "the method holding the site is not the analysed code");
    return 1;
}

int sos_cheat_functions_verify(const uint8_t *image, uint64_t size, const SosCheatBuild *build, const int *functions,
                               size_t count, const char **why) {
    if (!image || !build || size != build->image_size) return fail(why, "not the analysed boot image");
    for (size_t i = 0; i < count; ++i) {
        if (functions[i] < 0) {
            if ((uint64_t)build->string_new + 32 > size || fnv1a(image + build->string_new, 32) != build->string_new_hash)
                return fail(why, "il2cpp_string_new is not the analysed code");
            continue;
        }
        if (functions[i] >= SOS_CHEAT_FN_COUNT) return fail(why, "unknown game function");
        const SosCheatFunction *f = &build->functions[functions[i]];
        if (f->end > size || f->begin >= f->end || fnv1a(image + f->begin, f->end - f->begin) != f->hash)
            return fail(why, "a game function the cheat calls is not the analysed code");
    }
    return 1;
}

uint64_t sos_cheat_guest_rsp(const SosCheatContext *context) {
    return (uint64_t)(uintptr_t)context + sizeof(*context) + 128;
}

typedef struct {
    uint8_t *start, *at, *end;
} Emit;

static void bytes(Emit *e, const uint8_t *data, size_t size) {
    if (e->at && (size_t)(e->end - e->at) >= size) {
        memcpy(e->at, data, size);
        e->at += size;
    } else {
        e->at = NULL;
    }
}

#define EMIT(e, ...)                                                                                                   \
    do {                                                                                                               \
        const uint8_t emitted_[] = {__VA_ARGS__};                                                                      \
        bytes(e, emitted_, sizeof(emitted_));                                                                          \
    } while (0)

static void u32(Emit *e, uint32_t value) {
    const uint8_t data[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    bytes(e, data, 4);
}

static int rel32(const uint8_t *from_end, const uint8_t *to, int32_t *out) {
    const int64_t distance = (int64_t)((intptr_t)to - (intptr_t)from_end);
    if (distance < INT32_MIN || distance > INT32_MAX) return 0;
    *out = (int32_t)distance;
    return 1;
}

static int emit_jmp(Emit *e, const uint8_t *target) {
    int32_t distance;
    if (!e->at || !rel32(e->at + 5, target, &distance)) return 0;
    EMIT(e, 0xe9);
    u32(e, (uint32_t)distance);
    return e->at != NULL;
}

static void vectors(Emit *e, int load) {
    for (int i = 0; i < 16; ++i) {
        EMIT(e, 0xc5, (uint8_t)(i < 8 ? 0xfe : 0x7e), (uint8_t)(load ? 0x6f : 0x7f), (uint8_t)(0x84 | ((i & 7) << 3)), 0x24);
        u32(e, (uint32_t)(i * 32));
    }
}

size_t sos_cheat_stub_build(uint8_t *stub, size_t capacity, const uint8_t *image, const SosCheatSite *site,
                            SosCheatHandler handler) {
    if (!stub || !image || !site || !handler || site->executed == 0 || site->executed > site->length) return 0;
    Emit e = {stub, stub, stub + capacity};
    const uint8_t *back = image + site->rva + site->length;
    EMIT(&e, 0x48, 0x8d, 0x64, 0x24, 0x80); /* lea rsp, [rsp - 0x80]: the red zone stays untouched */
    EMIT(&e, 0x6a, 0x00);                   /* push 0: skip */
    EMIT(&e, 0x9c);                         /* pushfq */
    EMIT(&e, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55,
         0x41, 0x56, 0x41, 0x57);
    EMIT(&e, 0x48, 0x89, 0xe3);                         /* mov rbx, rsp */
    EMIT(&e, 0x48, 0x83, 0xe4, 0xe0);                   /* and rsp, -32 */
    EMIT(&e, 0x48, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00); /* sub rsp, 0x200 */
    vectors(&e, 0);
    EMIT(&e, 0x48, 0x89, 0xdf); /* mov rdi, rbx */
    EMIT(&e, 0x48, 0x89, 0xe6); /* mov rsi, rsp */
    EMIT(&e, 0xff, 0x15);       /* call [rip + slot] */
    uint8_t *slot_disp = e.at;
    u32(&e, 0);
    EMIT(&e, 0x88, 0x83);       /* mov [rbx + skip], al */
    u32(&e, (uint32_t)offsetof(SosCheatContext, skip));
    vectors(&e, 1);
    EMIT(&e, 0x48, 0x89, 0xdc); /* mov rsp, rbx */
    EMIT(&e, 0x80, 0xbc, 0x24); /* cmp byte [rsp + skip], 0 */
    u32(&e, (uint32_t)offsetof(SosCheatContext, skip));
    EMIT(&e, 0x00);
    EMIT(&e, 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5d,
         0x5b, 0x5a, 0x59, 0x58);
    EMIT(&e, 0x0f, 0x85); /* jne skipped: pop leaves the flags of the cmp */
    uint8_t *skip_disp = e.at;
    u32(&e, 0);
    EMIT(&e, 0x9d);                                     /* popfq */
    EMIT(&e, 0x48, 0x8d, 0xa4, 0x24, 0x88, 0x00, 0x00, 0x00); /* lea rsp, [rsp + 0x88] */
    uint8_t *copy = e.at;
    bytes(&e, site->original, site->executed);
    if (!e.at) return 0;
    if (site->rip_displacement >= 0 || site->relative_branch) {
        const unsigned at = site->relative_branch ? 1u : (unsigned)site->rip_displacement;
        if (at + 4 > site->executed) return 0;
        int32_t old;
        memcpy(&old, copy + at, 4);
        const int64_t moved = (int64_t)old + ((intptr_t)(image + site->rva) - (intptr_t)copy);
        if (moved < INT32_MIN || moved > INT32_MAX) return 0;
        const int32_t value = (int32_t)moved;
        memcpy(copy + at, &value, 4);
    }
    if (!emit_jmp(&e, back)) return 0;
    uint8_t *skipped = e.at;
    EMIT(&e, 0x9d);
    EMIT(&e, 0x48, 0x8d, 0xa4, 0x24, 0x88, 0x00, 0x00, 0x00);
    if (!emit_jmp(&e, back)) return 0;
    while (e.at && ((uintptr_t)e.at & 7)) EMIT(&e, 0xcc);
    uint8_t *slot = e.at;
    bytes(&e, (const uint8_t *)&handler, sizeof(handler));
    if (!e.at) return 0;
    int32_t distance;
    if (!rel32(skip_disp + 4, skipped, &distance)) return 0;
    memcpy(skip_disp, &distance, 4);
    if (!rel32(slot_disp + 4, slot, &distance)) return 0;
    memcpy(slot_disp, &distance, 4);
    return (size_t)(e.at - stub);
}

int sos_cheat_detour_prepare(SosCheatDetour *detour, uint8_t *image, const SosCheatSite *site, const uint8_t *stub) {
    if (!detour || !image || !site || !stub || site->length < 5 || site->length > SOS_CHEAT_ATOMIC_BLOCK) return 0;
    uint8_t *at = image + site->rva;
    if ((uintptr_t)at % SOS_CHEAT_ATOMIC_BLOCK + site->length > SOS_CHEAT_ATOMIC_BLOCK) return 0;
    int32_t distance;
    if (!rel32(at + 5, stub, &distance)) return 0;
    memset(detour, 0, sizeof(*detour));
    detour->at = at;
    detour->length = site->length;
    memcpy(detour->original, site->original, site->length);
    detour->patched[0] = 0xe9;
    memcpy(detour->patched + 1, &distance, 4);
    for (unsigned i = 5; i < site->length; ++i) detour->patched[i] = 0xcc;
    return 1;
}

static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;

static int writable(void *block, int enable, unsigned long *saved) {
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(block, SOS_CHEAT_ATOMIC_BLOCK, enable ? PAGE_EXECUTE_READWRITE : (DWORD)*saved, &old)) return 0;
    if (enable) *saved = old;
    return 1;
#else
    const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)block & ~(page - 1));
    (void)saved;
    return mprotect(start, page, enable ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC) == 0;
#endif
}

static int swap16(volatile void *block, const uint8_t expected[16], const uint8_t desired[16]) {
    uint64_t old_lo, old_hi, new_lo, new_hi;
    memcpy(&old_lo, expected, 8);
    memcpy(&old_hi, expected + 8, 8);
    memcpy(&new_lo, desired, 8);
    memcpy(&new_hi, desired + 8, 8);
    unsigned char swapped;
    __asm__ __volatile__("lock cmpxchg16b %1\n\tsete %0"
                         : "=q"(swapped), "+m"(*(volatile uint64_t(*)[2])block), "+a"(old_lo), "+d"(old_hi)
                         : "b"(new_lo), "c"(new_hi)
                         : "memory", "cc");
    return swapped;
}

int sos_cheat_detour_state(const SosCheatDetour *detour) {
    if (!detour || !detour->at) return -1;
    if (!memcmp(detour->at, detour->patched, detour->length)) return 1;
    if (!memcmp(detour->at, detour->original, detour->length)) return 0;
    return -1;
}

SosCheatWrite sos_cheat_detour_write(const SosCheatDetour *detour, int install) {
    if (!detour || !detour->at) return SOS_CHEAT_WRITE_MISMATCH;
    uint8_t *block = (uint8_t *)((uintptr_t)detour->at & ~(uintptr_t)(SOS_CHEAT_ATOMIC_BLOCK - 1));
    const size_t offset = (size_t)(detour->at - block);
    const uint8_t *from = install ? detour->original : detour->patched;
    const uint8_t *to = install ? detour->patched : detour->original;
    pthread_mutex_lock(&write_lock);
    SosCheatWrite result = SOS_CHEAT_WRITE_MISMATCH;
    unsigned long saved = 0;
    if (!writable(block, 1, &saved)) {
        pthread_mutex_unlock(&write_lock);
        return SOS_CHEAT_WRITE_PROTECT;
    }
    uint8_t expected[SOS_CHEAT_ATOMIC_BLOCK], desired[SOS_CHEAT_ATOMIC_BLOCK];
    memcpy(expected, block, sizeof(expected));
    if (!memcmp(expected + offset, from, detour->length)) {
        memcpy(desired, expected, sizeof(desired));
        memcpy(desired + offset, to, detour->length);
        if (swap16(block, expected, desired)) result = SOS_CHEAT_WRITE_DONE;
    }
    writable(block, 0, &saved);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), block, SOS_CHEAT_ATOMIC_BLOCK);
#endif
    pthread_mutex_unlock(&write_lock);
    return result;
}
