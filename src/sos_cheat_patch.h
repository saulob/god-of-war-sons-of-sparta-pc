/* One-instruction detours for the cheats. A site is a single game instruction inside one aligned 16-byte block; turning a
 * cheat On replaces it with a jump to a stub, Off writes the original bytes back. Each change is one locked 16-byte
 * compare-and-swap, so a thread running the game's code sees either the original instruction or the jump. */
#ifndef SOS_CHEAT_PATCH_H
#define SOS_CHEAT_PATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SOS_CHEAT_SITE_GOD_HP,
    SOS_CHEAT_SITE_GOD_GREY,
    SOS_CHEAT_SITE_SPIRIT,
    SOS_CHEAT_SITE_MAGIC,
    SOS_CHEAT_SITE_DAMAGE,
    SOS_CHEAT_SITE_MOVEMENT,
    SOS_CHEAT_SITE_JUMP,
    SOS_CHEAT_SITE_WALLET_TICK,
    SOS_CHEAT_SITE_COUNT
};

enum {
    SOS_CHEAT_FN_GET_GAME_OBJECT,
    SOS_CHEAT_FN_IS_PLAYER,
    SOS_CHEAT_FN_GET_PAWN,
    SOS_CHEAT_FN_PLAYER_INDEX,
    SOS_CHEAT_FN_TRY_GET_WALLET,
    SOS_CHEAT_FN_ITEM_BY_ID,
    SOS_CHEAT_FN_WALLET_AMOUNT,
    SOS_CHEAT_FN_WALLET_ADD,
    SOS_CHEAT_FN_SLOPE_ALIGNED,
    SOS_CHEAT_FN_JUMP_ANGLE,
    SOS_CHEAT_FN_COUNT
};

#define SOS_CHEAT_ATOMIC_BLOCK 16u
#define SOS_CHEAT_STUB_SIZE 512u
#define SOS_CHEAT_STUB_BYTES (SOS_CHEAT_SITE_COUNT * SOS_CHEAT_STUB_SIZE)
#define SOS_CHEAT_SYSV __attribute__((sysv_abi))

typedef struct {
    uint32_t begin, end;
} SosCheatRange;

typedef struct {
    uint32_t rva;
    uint8_t length;   /* bytes the detour replaces */
    uint8_t executed; /* bytes of them the game executes: a ret followed by padding is 1 */
    int8_t rip_displacement;
    uint8_t relative_branch;
    uint8_t original[SOS_CHEAT_ATOMIC_BLOCK];
    SosCheatRange method;
    uint64_t method_hash;
} SosCheatSite;

typedef struct {
    uint32_t begin, end;
    uint64_t hash;
} SosCheatFunction;

typedef struct {
    const char *build;
    uint64_t image_size;
    SosCheatSite sites[SOS_CHEAT_SITE_COUNT];
    SosCheatFunction functions[SOS_CHEAT_FN_COUNT];
    uint32_t string_new;
    uint64_t string_new_hash;
} SosCheatBuild;

/* The table of a supported build, NULL for any other. */
const SosCheatBuild *sos_cheat_build(const char *build_key);

/* 1 when the image is the build's and the site's whole method and original bytes are the ones analysed. */
int sos_cheat_site_verify(const uint8_t *image, uint64_t size, const SosCheatBuild *build, int site, const char **why);
/* 1 when every listed function (SOS_CHEAT_FN_*, or -1 for il2cpp_string_new) is the analysed code. */
int sos_cheat_functions_verify(const uint8_t *image, uint64_t size, const SosCheatBuild *build, const int *functions,
                               size_t count, const char **why);

/* What a stub saves before it calls its handler, lowest address first. The handler may change any of it but `skip`
 * comes from its return value: nonzero skips the original instruction (the handler did its work). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax, rflags, skip;
} SosCheatContext;

typedef struct {
    float f[8];
} SosCheatVector;

typedef int(SOS_CHEAT_SYSV *SosCheatHandler)(SosCheatContext *context, SosCheatVector *vectors);

/* The guest rsp at the site. */
uint64_t sos_cheat_guest_rsp(const SosCheatContext *context);

/* Writes the stub of `site` at `stub` (it is reached by a rel32 jump from the site and jumps back the same way).
 * Returns its size, or 0 when `capacity` is too small or the stub is out of rel32 reach of the image. */
size_t sos_cheat_stub_build(uint8_t *stub, size_t capacity, const uint8_t *image, const SosCheatSite *site,
                            SosCheatHandler handler);

typedef struct {
    uint8_t *at;
    uint8_t length;
    uint8_t original[SOS_CHEAT_ATOMIC_BLOCK];
    uint8_t patched[SOS_CHEAT_ATOMIC_BLOCK];
} SosCheatDetour;

/* The jump `site` gets while its cheat is On. 0 when the site is not inside one aligned block or out of reach. */
int sos_cheat_detour_prepare(SosCheatDetour *detour, uint8_t *image, const SosCheatSite *site, const uint8_t *stub);

typedef enum { SOS_CHEAT_WRITE_DONE, SOS_CHEAT_WRITE_MISMATCH, SOS_CHEAT_WRITE_PROTECT } SosCheatWrite;

/* Installs (or with install 0 restores) one detour: refused, writing nothing, unless the site holds exactly the bytes of
 * the other state. */
SosCheatWrite sos_cheat_detour_write(const SosCheatDetour *detour, int install);

/* 1 installed, 0 original, -1 neither (foreign bytes). */
int sos_cheat_detour_state(const SosCheatDetour *detour);

#ifdef __cplusplus
}
#endif

#endif
