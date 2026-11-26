/* Headless CPU compatibility test (tools\test_cpu_compat.ps1): the processor requirement for synthetic CPUID profiles,
 * F16C conversion against the host's own instructions, every stub form run natively against the instruction it replaces
 * (registers, flags, ymm, MXCSR, memory, rsp and the red zone), table validation with mutations, and (--image,
 * --module) the tables applied to real game code. Nothing of the game runs. */
#include "cpu_compat.h"
#include "cpu_compat_profiles.h"
#include "isa_emulate.h"
#include <cpuid.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include "redzone_analysis.h"
#else
#include <sys/mman.h>
#endif
#include <Zydis/Zydis.h>

static unsigned checks, failures;

/* Memory the tests may run code from. */
static void *code_memory(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
static void code_memory_free(void *p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

static void check(int ok, const char *what, unsigned long long a, unsigned long long b) {
    ++checks;
    if (ok) return;
    if (++failures <= 60) printf("  FAIL %s (%#llx, %#llx)\n", what, a, b);
}
#define CHECK(condition, what) check(!!(condition), what, 0, 0)

static uint64_t random_state = 0x2545f4914f6cdd1dull;
static uint64_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

/* --- processor profiles --------------------------------------------------------------------------------------------- */


static void test_profiles(void) {
    for (size_t i = 0; i < sizeof profiles / sizeof *profiles; ++i) {
        const Profile *p = &profiles[i];
        char required[128], handled[128];
        const int missing = isa_missing_sets(p->host, 0, required, sizeof required);
        isa_missing_sets(p->host, 1, handled, sizeof handled);
        const unsigned compat = cpu_compat_needed(p->host);
        check((missing == 0) == p->usable, p->name, (unsigned long long)missing, (unsigned long long)p->usable);
        check(!strcmp(required, p->required_missing), p->name, 1, 0);
        check(!strcmp(handled, p->handled), p->name, 2, 0);
        check(compat == p->compat, p->name, compat, p->compat);
        printf("  %-30s %s; handled in software: %s; replaced at load: ", p->name, p->usable ? "runs" : "refused",
               *handled ? handled : "none");
        char kinds[64];
        cpu_compat_kind_names(compat, kinds, sizeof kinds);
        printf("%s%s%s\n", *kinds ? kinds : "none", missing ? "; missing " : "", missing ? required : "");
        /* AVX2, BMI2, FMA and the non-VEX SSE4 bits change nothing: the game has none of those instructions. */
        IsaHostFeatures variant = p->host;
        variant.leaf7_ebx ^= L7_AVX2 | L7_BMI2;
        variant.leaf1_ecx ^= L1_FMA | L1_SSE41 | L1_SSE42 | L1_AES;
        char required2[128], handled2[128];
        isa_missing_sets(variant, 0, required2, sizeof required2);
        isa_missing_sets(variant, 1, handled2, sizeof handled2);
        check(!strcmp(required, required2) && !strcmp(handled, handled2) && cpu_compat_needed(variant) == compat,
              "AVX2, BMI2, FMA, AES and SSE4.1/4.2 are not requirements", i, 0);
    }
    const IsaHostFeatures real = isa_host_features();
    char name[64], present[128];
    isa_host_name(name, sizeof name);
    isa_present_sets(real, present, sizeof present);
    printf("  host: %s; native %s\n", name, present);
}

/* The decisions the loader and the module loader take, with no game data: cpu_compat_processor_usable,
 * cpu_compat_plan and cpu_compat_install for every profile, forced, and the names nothing is known about. */
static void test_plans(void) {
    static const char *const builds[] = {"CUSA57767-1.00", "CUSA57767-1.01"};
    for (size_t i = 0; i < sizeof profiles / sizeof *profiles; ++i) {
        const Profile *p = &profiles[i];
        char cause[160] = "", advice[160] = "";
        const int usable = cpu_compat_processor_usable(p->host, cause, sizeof cause, advice, sizeof advice);
        check(usable == p->usable && (usable || (strstr(cause, p->required_missing) && strstr(advice, "AVX"))), p->name, usable, 0);
        for (unsigned b = 0; b < 2; ++b) {
            const CpuCompatPlan plan = cpu_compat_plan(CPU_COMPAT_BOOT_IMAGE, builds[b], p->host, 0);
            const CpuCompatTable *table = cpu_compat_boot_table(builds[b]);
            check(plan.kinds == p->compat && plan.wanted == p->compat && plan.forced == 0 &&
                      plan.essential == (p->compat & (CPU_COMPAT_LZCNT | CPU_COMPAT_TZCNT | CPU_COMPAT_F16C)) &&
                      plan.table == (p->compat ? table : NULL) && plan.stub_bytes == cpu_compat_stub_bytes(plan.table, p->compat),
                  p->name, plan.kinds, b);
        }
        const CpuCompatPlan fmod = cpu_compat_plan(CPU_COMPAT_MODULE, "libfmod.prx", p->host, 0);
        check(fmod.wanted == (p->compat & (CPU_COMPAT_LZCNT | CPU_COMPAT_BMI1)) && fmod.essential == (p->compat & CPU_COMPAT_LZCNT),
              p->name, fmod.wanted, 1);
        const CpuCompatPlan studio = cpu_compat_plan(CPU_COMPAT_MODULE, "libfmodstudio.prx", p->host, 0);
        check(studio.wanted == (p->compat & (CPU_COMPAT_TZCNT | CPU_COMPAT_BMI1)) && studio.essential == (p->compat & CPU_COMPAT_TZCNT),
              p->name, studio.wanted, 2);
    }
    const IsaHostFeatures modern = profiles[8].host, sandy = profiles[2].host;
    const CpuCompatPlan forced = cpu_compat_plan(CPU_COMPAT_BOOT_IMAGE, builds[0], modern, CPU_COMPAT_ALL);
    check(forced.kinds == CPU_COMPAT_ALL && forced.wanted == CPU_COMPAT_ALL && forced.forced == CPU_COMPAT_ALL &&
          forced.table == cpu_compat_boot_table(builds[0]) && forced.stub_bytes == cpu_compat_stub_bytes(forced.table, CPU_COMPAT_ALL),
          "forced plan on a processor that has everything", forced.kinds, 0);
    const CpuCompatPlan none = cpu_compat_plan(CPU_COMPAT_BOOT_IMAGE, builds[0], modern, 0);
    check(!none.kinds && !none.wanted && !none.table && !none.stub_bytes, "nothing planned where nothing is missing", none.kinds, 0);

    /* An unknown build is fatal where LZCNT, TZCNT or F16C are missing; a module without a table has nothing to do. */
    uint8_t image[64], copy[64];
    for (size_t i = 0; i < sizeof image; ++i) image[i] = (uint8_t)i;
    memcpy(copy, image, sizeof image);
    const CpuCompatPlan unknown = cpu_compat_plan(CPU_COMPAT_BOOT_IMAGE, "CUSA57767-9.99", sandy, 0);
    const CpuCompatOutcome refused = cpu_compat_install(&unknown, image, sizeof image, NULL, 0);
    check(!unknown.table && refused.fatal && refused.status == CPU_COMPAT_UNKNOWN_IMAGE && !memcmp(image, copy, sizeof image),
          "unknown build refused", refused.status, 0);
    const CpuCompatPlan piledriver = cpu_compat_plan(CPU_COMPAT_BOOT_IMAGE, "CUSA57767-9.99", profiles[5].host, 0);
    const CpuCompatOutcome emulated = cpu_compat_install(&piledriver, image, sizeof image, NULL, 0);
    check(!emulated.fatal && emulated.message[0] && !memcmp(image, copy, sizeof image), "MOVBE alone falls back to emulation",
          emulated.status, 0);
    const CpuCompatPlan other = cpu_compat_plan(CPU_COMPAT_MODULE, "SaveData.prx", sandy, 0);
    const CpuCompatOutcome nothing = cpu_compat_install(&other, image, sizeof image, NULL, 0);
    check(!other.wanted && !other.stub_bytes && !nothing.fatal && !nothing.message[0] && !memcmp(image, copy, sizeof image),
          "a module without sites is left alone", nothing.status, 0);
    printf("  plans of the loader and the module loader: %zu profiles x CUSA57767 1.00 and 1.01, libfmod.prx and "
           "libfmodstudio.prx; forced; unknown build refused; module without sites left alone\n",
           sizeof profiles / sizeof *profiles);
}

/* --- F16C conversions against the host's instructions ----------------------------------------------------------------- */

static int host_f16c(void) { return (isa_host_features().leaf1_ecx & bit_F16C) != 0; }

static unsigned get_mxcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }

static uint32_t native_widen(uint16_t half) {
    uint32_t out;
    __asm__ volatile("vmovd %1, %%xmm0\n vcvtph2ps %%xmm0, %%xmm1\n vmovd %%xmm1, %0" : "=r"(out) : "r"((uint32_t)half) : "xmm0", "xmm1");
    return out;
}
static uint16_t native_narrow(uint32_t single, unsigned control) {
    uint32_t out = 0;
#define NARROW(imm) __asm__ volatile("vmovd %1, %%xmm0\n vcvtps2ph $" #imm ", %%xmm0, %%xmm1\n vmovd %%xmm1, %0" : "=r"(out) : "r"(single) : "xmm0", "xmm1")
    switch (control) {
    case 0: NARROW(0); break;
    case 1: NARROW(1); break;
    case 2: NARROW(2); break;
    case 3: NARROW(3); break;
    default: NARROW(4); break;
    }
#undef NARROW
    return (uint16_t)out;
}

static void test_conversions(void) {
    if (!host_f16c()) { printf("  [skip] the host has no F16C to compare with\n"); return; }
    const unsigned saved = get_mxcsr();
    unsigned widened = 0, narrowed = 0;
    for (unsigned daz = 0; daz < 2; ++daz) {
        set_mxcsr((saved & ~0x8040u) | (daz ? 0x8040u : 0));
        for (uint32_t h = 0; h < 65536; ++h, ++widened)
            check(native_widen((uint16_t)h) == isa_half_to_float((uint16_t)h), "VCVTPH2PS", h, daz);
    }
    uint32_t samples[64];
    for (unsigned round = 0; round < 4096 * 4; ++round) {
        unsigned n = 0;
        const uint32_t sign = round & 1 ? 0x80000000u : 0;
        const uint32_t exponent = round / 2 % 256;
        samples[n++] = sign | exponent << 23;
        samples[n++] = sign | exponent << 23 | 0x7fffff;
        samples[n++] = sign | exponent << 23 | 0x1000;
        samples[n++] = sign | exponent << 23 | 0x0fff;
        samples[n++] = sign | exponent << 23 | 0x2000;
        samples[n++] = sign | exponent << 23 | 0x3000;
        samples[n++] = sign | exponent << 23 | 0x1001;
        while (n < 64) samples[n++] = round < 512 ? (sign | exponent << 23 | ((uint32_t)next_random() & 0x7fffff))
                                                  : (uint32_t)next_random();
        for (unsigned mode = 0; mode < 8; ++mode) {
            const unsigned control = mode < 4 ? mode : 4, rounding = mode < 4 ? mode : mode - 4;
            for (unsigned flags = 0; flags < 4; ++flags) {
                const int daz = flags & 1, ftz = flags >> 1;
                set_mxcsr((saved & ~0xe040u) | rounding << 13 | (daz ? 0x40u : 0) | (ftz ? 0x8000u : 0));
                for (unsigned k = 0; k < n; ++k, ++narrowed) {
                    const uint16_t want = native_narrow(samples[k], control);
                    const uint16_t got = isa_float_to_half(samples[k], control < 4 ? control : rounding, daz);
                    check(want == got, "VCVTPS2PH", samples[k], (unsigned long long)want << 16 | got);
                }
            }
        }
    }
    /* Through the instruction: MXCSR's DAZ and rounding reach VCVTPS2PH xmm1,xmm0,4. */
    static const uint8_t narrow_by_mxcsr[] = {0xc4, 0xe3, 0x79, 0x1d, 0xc1, 0x04};
    static const uint32_t denormals[4] = {0x00000001u, 0x80000001u, 0x00400000u, 0x80000400u};
    for (unsigned mode = 0; mode < 8; ++mode) {
        const unsigned mxcsr = 0x1f80u | (mode & 3) << 13 | (mode & 4 ? 0x40u : 0);
        uint8_t ymm[16][32];
        memset(ymm, 0x77, sizeof ymm);
        memcpy(ymm[0], denormals, sizeof denormals);
        uint64_t gpr[16] = {0};
        CHECK(isa_emulate_f16c(narrow_by_mxcsr, 0, gpr, ymm, mxcsr) == sizeof narrow_by_mxcsr, "VCVTPS2PH length");
        uint64_t native = 0;
        set_mxcsr(mxcsr);
        __asm__ volatile("vmovdqu %1, %%xmm0\n vcvtps2ph $4, %%xmm0, %%xmm1\n vmovq %%xmm1, %0"
                         : "=r"(native) : "m"(denormals) : "xmm0", "xmm1");
        set_mxcsr(saved);
        uint64_t emulated;
        memcpy(&emulated, ymm[1], 8);
        static const uint8_t zero[24] = {0};
        check(emulated == native && !memcmp(ymm[1] + 8, zero, sizeof zero), "VCVTPS2PH with MXCSR rounding and DAZ", emulated, native);
    }
    set_mxcsr(saved);
    printf("  %u halves widened, %u singles narrowed in every rounding with DAZ and FTZ: same as the host's F16C\n", widened,
           narrowed);
}

/* --- running code natively ---------------------------------------------------------------------------------------------- */

typedef struct {
    uint64_t gpr[16];   /* gpr[4]: rsp after the code ran */
    uint64_t flags;     /* 128 */
    uint64_t canary_ok; /* 136 */
    uint8_t ymm[16][32];/* 144 */
    uint32_t mxcsr_in, mxcsr_out; /* 656, 660 */
    uint64_t rsp_before; /* 664 */
} Machine;
_Static_assert(__builtin_offsetof(Machine, ymm) == 144 && __builtin_offsetof(Machine, mxcsr_in) == 656 &&
               __builtin_offsetof(Machine, rsp_before) == 664, "harness layout");

void *cc_target;
Machine *cc_machine;

/* Loads every register, flag, ymm and MXCSR from cc_machine, fills the 128 bytes below the code's rsp, calls cc_target and
 * stores everything back, with whether those 128 bytes survived. */
__asm__(
    ".text\n.intel_syntax noprefix\n.p2align 4\n.globl cc_run\ncc_run:\n"
    "push rbx\npush rbp\npush rdi\npush rsi\npush r12\npush r13\npush r14\npush r15\n"
    "sub rsp, 168\n"
    ".irp i,6,7,8,9,10,11,12,13,14,15\nmovdqu [rsp+16*(\\i-6)], xmm\\i\n.endr\n"
    "stmxcsr [rsp+160]\n"
    "mov rax, qword ptr [rip+cc_machine]\n"
    "mov [rax+664], rsp\n"
    "lea rdi, [rsp-136]\nmov rcx, 16\nmov rdx, 0xA5A5A5A5A5A5A5A5\n"
    "1: mov [rdi], rdx\nadd rdi, 8\ndec rcx\njnz 1b\n"
    ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\nvmovdqu ymm\\i, [rax+144+32*\\i]\n.endr\n"
    "ldmxcsr [rax+656]\n"
    "push qword ptr [rax+128]\npopfq\n"
    "mov rcx, [rax+8]\nmov rdx, [rax+16]\nmov rbx, [rax+24]\nmov rbp, [rax+40]\nmov rsi, [rax+48]\nmov rdi, [rax+56]\n"
    "mov r8, [rax+64]\nmov r9, [rax+72]\nmov r10, [rax+80]\nmov r11, [rax+88]\nmov r12, [rax+96]\nmov r13, [rax+104]\n"
    "mov r14, [rax+112]\nmov r15, [rax+120]\nmov rax, [rax]\n"
    "call qword ptr [rip+cc_target]\n"
    "push rax\nmov rax, qword ptr [rip+cc_machine]\npop qword ptr [rax]\n"
    "mov [rax+8], rcx\nmov [rax+16], rdx\nmov [rax+24], rbx\nmov [rax+32], rsp\nmov [rax+40], rbp\nmov [rax+48], rsi\n"
    "mov [rax+56], rdi\nmov [rax+64], r8\nmov [rax+72], r9\nmov [rax+80], r10\nmov [rax+88], r11\nmov [rax+96], r12\n"
    "mov [rax+104], r13\nmov [rax+112], r14\nmov [rax+120], r15\n"
    "pushfq\npop qword ptr [rax+128]\n"
    "stmxcsr [rax+660]\n"
    ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\nvmovdqu [rax+144+32*\\i], ymm\\i\n.endr\n"
    "lea rdi, [rsp-136]\nmov rcx, 16\nmov rdx, 0xA5A5A5A5A5A5A5A5\nmov r8d, 1\n"
    "2: cmp [rdi], rdx\nje 3f\nxor r8d, r8d\n3: add rdi, 8\ndec rcx\njnz 2b\n"
    "mov [rax+136], r8\n"
    "vzeroupper\nldmxcsr [rsp+160]\n"
    ".irp i,6,7,8,9,10,11,12,13,14,15\nmovdqu xmm\\i, [rsp+16*(\\i-6)]\n.endr\n"
    "add rsp, 168\npop r15\npop r14\npop r13\npop r12\npop rsi\npop rdi\npop rbp\npop rbx\nret\n"
    ".att_syntax prefix\n");
void cc_run(void);

static void run_code(const void *code, Machine *m) {
    cc_target = (void *)code;
    cc_machine = m;
    cc_run();
}

/* --- stubs against the instructions they replace -------------------------------------------------------------------------- */

enum { NONE = -1, FLAGS_ALL = 0x8d5, FLAGS_COUNT = 0x41, FLAGS_LOGIC = 0x8c5, FLAGS_BMI = 0x8c1, FLAGS_BEXTR = 0x841 };
enum { K_PLAIN = 0, K_LZ = 1, K_TZ = 2, K_F16 = 3, K_MOVBE = 4, K_BMI = 5 };

typedef struct {
    const char *name;
    uint8_t bytes[CPU_COMPAT_SITE_MAX];
    uint8_t parts[CPU_COMPAT_PARTS];
    uint8_t shield;
    uint8_t below;        /* the stub runs something below the red zone */
    int base, index;      /* registers that must hold the data address and a small index */
    int rip_disp;         /* offset of a rip-relative disp32 into the data, -1 for none */
    unsigned rip_after;   /* bytes after that displacement */
    unsigned flags_mask;
} Snippet;

#define P CPU_COMPAT_PART
static const Snippet snippets[] = {
    {"lzcnt eax,ecx; xor eax,31", {0xf3, 0x0f, 0xbd, 0xc1, 0x83, 0xf0, 0x1f}, {P(K_LZ, 4), P(K_PLAIN, 3)}, 0, 0, NONE, NONE, -1, 0, FLAGS_LOGIC},
    {"lzcnt r9,r10", {0xf3, 0x4d, 0x0f, 0xbd, 0xca}, {P(K_LZ, 5)}, 0, 0, NONE, NONE, -1, 0, FLAGS_COUNT},
    {"lzcnt cx,dx", {0x66, 0xf3, 0x0f, 0xbd, 0xca}, {P(K_LZ, 5)}, 0, 0, NONE, NONE, -1, 0, FLAGS_COUNT},
    {"lzcnt eax,[rbx+8]", {0xf3, 0x0f, 0xbd, 0x43, 0x08}, {P(K_LZ, 5)}, 0, 1, 3, NONE, -1, 0, FLAGS_COUNT},
    {"lzcnt eax,[rsp-16]", {0xf3, 0x0f, 0xbd, 0x44, 0x24, 0xf0}, {P(K_LZ, 6)}, 0, 0, NONE, NONE, -1, 0, FLAGS_COUNT},
    {"lzcnt rdx,[rbx+rcx*8+0x100]", {0xf3, 0x48, 0x0f, 0xbd, 0x94, 0xcb, 0x00, 0x01, 0x00, 0x00}, {P(K_LZ, 10)}, 0, 1, 3, 1, -1, 0, FLAGS_COUNT},
    {"tzcnt eax,ecx; test eax,eax", {0xf3, 0x0f, 0xbc, 0xc1, 0x85, 0xc0}, {P(K_TZ, 4), P(K_PLAIN, 2)}, 0, 0, NONE, NONE, -1, 0, FLAGS_LOGIC},
    {"tzcnt r15,rax", {0xf3, 0x4c, 0x0f, 0xbc, 0xf8}, {P(K_TZ, 5)}, 0, 0, NONE, NONE, -1, 0, FLAGS_COUNT},
    {"tzcnt dx,[rbx]", {0x66, 0xf3, 0x0f, 0xbc, 0x13}, {P(K_TZ, 5)}, 0, 1, 3, NONE, -1, 0, FLAGS_COUNT},
    {"mov eax,edx; lzcnt ecx,eax", {0x89, 0xd0, 0xf3, 0x0f, 0xbd, 0xc8}, {P(K_PLAIN, 2), P(K_LZ, 4)}, 0, 0, NONE, NONE, -1, 0, FLAGS_COUNT},
    {"lzcnt eax,ecx; lzcnt edx,r8d; xor eax,edx", {0xf3, 0x0f, 0xbd, 0xc1, 0xf3, 0x41, 0x0f, 0xbd, 0xd0, 0x31, 0xd0},
     {P(K_LZ, 4), P(K_LZ, 5), P(K_PLAIN, 2)}, 0, 0, NONE, NONE, -1, 0, FLAGS_LOGIC},
    {"lzcnt eax,ecx; mov edx,[rbx] (shielded)", {0xf3, 0x0f, 0xbd, 0xc1, 0x8b, 0x13}, {P(K_LZ, 4), P(K_PLAIN, 2)}, 1, 1, 3, NONE, -1, 0, FLAGS_COUNT},
    {"tzcnt ecx,eax; lzcnt eax,ecx", {0xf3, 0x0f, 0xbc, 0xc8, 0xf3, 0x0f, 0xbd, 0xc1}, {P(K_TZ, 4), P(K_LZ, 4)}, 0, 0, NONE, NONE, -1, 0, FLAGS_COUNT},
    {"vcvtph2ps xmm1,xmm2", {0xc4, 0xe2, 0x79, 0x13, 0xca}, {P(K_F16, 5)}, 0, 1, NONE, NONE, -1, 0, FLAGS_ALL},
    {"vcvtph2ps xmm9,[rbx+16]", {0xc4, 0x62, 0x79, 0x13, 0x4b, 0x10}, {P(K_F16, 6)}, 0, 1, 3, NONE, -1, 0, FLAGS_ALL},
    {"vcvtps2ph xmm3,xmm12,0", {0xc4, 0x63, 0x79, 0x1d, 0xe3, 0x00}, {P(K_F16, 6)}, 0, 1, NONE, NONE, -1, 0, FLAGS_ALL},
    {"vcvtps2ph [rbx+32],xmm5,4", {0xc4, 0xe3, 0x79, 0x1d, 0x6b, 0x20, 0x04}, {P(K_F16, 7)}, 0, 1, 3, NONE, -1, 0, FLAGS_ALL},
    {"vcvtps2ph xmm14,xmm0,3", {0xc4, 0xc3, 0x79, 0x1d, 0xc6, 0x03}, {P(K_F16, 6)}, 0, 1, NONE, NONE, -1, 0, FLAGS_ALL},
    {"vcvtph2ps xmm0,[rip]", {0xc4, 0xe2, 0x79, 0x13, 0x05, 0, 0, 0, 0}, {P(K_F16, 9)}, 0, 1, NONE, NONE, 5, 0, FLAGS_ALL},
    {"vcvtps2ph [rip],xmm7,1", {0xc4, 0xe3, 0x79, 0x1d, 0x3d, 0, 0, 0, 0, 0x01}, {P(K_F16, 10)}, 0, 1, NONE, NONE, 5, 1, FLAGS_ALL},
    {"movbe eax,[rbx+4]", {0x0f, 0x38, 0xf0, 0x43, 0x04}, {P(K_MOVBE, 5)}, 0, 1, 3, NONE, -1, 0, FLAGS_ALL},
    {"movbe [rbx+8],r10", {0x4c, 0x0f, 0x38, 0xf1, 0x53, 0x08}, {P(K_MOVBE, 6)}, 0, 1, 3, NONE, -1, 0, FLAGS_ALL},
    {"andn eax,ecx,edx", {0xc4, 0xe2, 0x70, 0xf2, 0xc2}, {P(K_BMI, 5)}, 0, 1, NONE, NONE, -1, 0, FLAGS_BMI},
    {"bextr r11,[rbx],r9", {0xc4, 0x62, 0xb0, 0xf7, 0x1b}, {P(K_BMI, 5)}, 0, 1, 3, NONE, -1, 0, FLAGS_BEXTR},
    {"blsr eax,ecx", {0xc4, 0xe2, 0x78, 0xf3, 0xc9}, {P(K_BMI, 5)}, 0, 1, NONE, NONE, -1, 0, FLAGS_BMI},
    {"mov eax,edx; tzcnt ecx,eax; mov edx,[rbx] (shielded)", {0x89, 0xd0, 0xf3, 0x0f, 0xbc, 0xc8, 0x8b, 0x13},
     {P(K_PLAIN, 2), P(K_TZ, 4), P(K_PLAIN, 2)}, 1, 1, 3, NONE, -1, 0, FLAGS_COUNT},
};
#undef P

static size_t snippet_length(const Snippet *s) {
    size_t n = 0;
    for (unsigned p = 0; p < CPU_COMPAT_PARTS && s->parts[p]; ++p) n += s->parts[p] & 15;
    return n;
}

static uint8_t data_master[512], *data;

static void input_state(Machine *m, unsigned variant, const Snippet *s) {
    memset(m, 0, sizeof *m);
    static const uint64_t fixed[] = {0, 1, 0x8000000000000000ull, ~0ull, 0x80000000ull, 0x8000ull, 0x10000ull};
    for (unsigned r = 0; r < 16; ++r) {
        uint64_t v = next_random();
        if (variant < sizeof fixed / sizeof *fixed) v = fixed[variant];
        else if (variant % 3 == 0) v &= 0xffff;
        else if (variant % 3 == 1) v >>= next_random() & 63;
        m->gpr[r] = v;
    }
    for (unsigned r = 0; r < 16; ++r)
        for (unsigned b = 0; b < 32; b += 8) {
            uint64_t v = next_random();
            if (variant % 2) v &= 0x3fff3fff3fff3fffull;
            memcpy(m->ymm[r] + b, &v, 8);
        }
    m->flags = (next_random() & FLAGS_ALL) | 2;
    m->mxcsr_in = 0x1f80u | (unsigned)(next_random() & 3) << 13 | (variant % 4 == 1 ? 0x8040u : 0);
    if (s->base != NONE) m->gpr[s->base] = (uintptr_t)(data + 64);
    if (s->index != NONE) m->gpr[s->index] &= 3;
    m->gpr[4] = 0;
}

static int same_result(const Machine *a, const Machine *b, unsigned flags_mask) {
    return !memcmp(a->gpr, b->gpr, sizeof a->gpr) && ((a->flags ^ b->flags) & flags_mask) == 0 &&
           !memcmp(a->ymm, b->ymm, sizeof a->ymm) && ((a->mxcsr_out ^ b->mxcsr_out) & ~0x3fu) == 0 && a->canary_ok &&
           b->canary_ok && a->gpr[4] == a->rsp_before && b->gpr[4] == b->rsp_before;
}

/* One snippet in an image of its own: run natively, replaced, run again, compared. */
static void run_snippet(const Snippet *s, unsigned kinds, const char *label) {
    const size_t length = snippet_length(s), image_size = 4096, stubs_size = 8192;
    uint8_t *image = code_memory(image_size + stubs_size);
    if (!image) { CHECK(0, "executable memory"); return; }
    const size_t at = 1024 + 7;
    memset(image, 0xcc, image_size + stubs_size);
    memcpy(image + at, s->bytes, length);
    image[at + length] = 0xc3;
    if (s->rip_disp >= 0) {
        const int32_t disp = (int32_t)((intptr_t)(data + 96) - (intptr_t)(image + at + s->rip_disp + 4 + s->rip_after));
        memcpy(image + at + s->rip_disp, &disp, 4);
    }
    CpuCompatSite site = {(uint32_t)at, (uint8_t)length, s->shield, {0}};
    memcpy(site.parts, s->parts, sizeof site.parts);
    const CpuCompatTable table = {"snippet", image_size, cpu_compat_digest(image, image_size, &site, 1), &site, 1};

    enum { VARIANTS = 24 };
    Machine native[VARIANTS];
    static uint8_t native_data[VARIANTS][512];
    const uint64_t seed = random_state;
    for (unsigned v = 0; v < VARIANTS; ++v) {
        memcpy(data, data_master, sizeof data_master);
        input_state(&native[v], v, s);
        run_code(image + at, &native[v]);
        memcpy(native_data[v], data, sizeof data_master);
    }
    size_t replaced = 0;
    const CpuCompatStatus status = cpu_compat_apply(image, image_size, &table, kinds, image + image_size, stubs_size, &replaced);
    check(status == CPU_COMPAT_OK, label, status, 0);
    const int expect_jump = (cpu_compat_site_kinds(&site) & kinds) != 0;
    check(expect_jump == (image[at] == 0xe9), label, image[at], expect_jump);
    if (expect_jump && status == CPU_COMPAT_OK) {
        /* Memory a page fault can stop is reached only below the red zone: lea rsp,[rsp-128] precedes it in the stub. */
        int32_t rel;
        memcpy(&rel, image + at + 1, 4);
        const uint8_t *stub = image + at + 5 + rel;
        static const uint8_t down[] = {0x48, 0x8d, 0x64, 0x24, 0x80};
        int lowered = 0;
        for (unsigned k = 0; k + sizeof down <= CPU_COMPAT_STUB_STRIDE; ++k) lowered |= !memcmp(stub + k, down, sizeof down);
        check(lowered == s->below, label, lowered, s->below);
        if (s->shield) check(!memcmp(stub, down, sizeof down), label, stub[0], 0x48);
    }
    random_state = seed;
    for (unsigned v = 0; v < VARIANTS && status == CPU_COMPAT_OK; ++v) {
        Machine m;
        memcpy(data, data_master, sizeof data_master);
        input_state(&m, v, s);
        run_code(image + at, &m);
        check(same_result(&native[v], &m, s->flags_mask), label, v, m.flags ^ native[v].flags);
        check(!memcmp(native_data[v], data, sizeof data_master), label, v, 1);
    }
    code_memory_free(image, image_size + stubs_size);
}

static void test_stubs(void) {
    data = code_memory(4096);
    for (size_t i = 0; i < sizeof data_master; ++i) data_master[i] = (uint8_t)next_random();
    memset(data_master + 64, 0, 4); /* a zero source for the memory forms */
    const IsaHostFeatures host = isa_host_features();
    const int native_all = (host.leaf1_ecx & bit_F16C) && (host.leaf1_ecx & bit_MOVBE) && (host.leaf7_ebx & bit_BMI) &&
                           (host.ext1_ecx & bit_LZCNT);
    if (!native_all) { printf("  [skip] the host lacks LZCNT, BMI1, MOVBE or F16C to compare with\n"); return; }
    unsigned runs = 0;
    for (size_t i = 0; i < sizeof snippets / sizeof *snippets; ++i) {
        run_snippet(&snippets[i], CPU_COMPAT_ALL, snippets[i].name);
        const unsigned kinds = cpu_compat_site_kinds(&(CpuCompatSite){0, 0, 0, {snippets[i].parts[0], snippets[i].parts[1],
                                                                                 snippets[i].parts[2], snippets[i].parts[3]}});
        /* Each kind alone, as on a processor that lacks only that one: the others run natively in the stub. */
        for (unsigned k = 1; k <= CPU_COMPAT_BMI1; k <<= 1)
            if (kinds & k) run_snippet(&snippets[i], k, snippets[i].name), ++runs;
        run_snippet(&snippets[i], 0, snippets[i].name);
        runs += 2;
    }
    printf("  %zu instruction forms, %u replaced and native runs of 24 states each: registers, flags, ymm, MXCSR, memory, rsp "
           "and red zone as the host's own instructions leave them\n", sizeof snippets / sizeof *snippets, runs);
    code_memory_free(data, 4096);
}

/* --- validation --------------------------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *image, *stubs, *copy, *stubs_copy;
    size_t size, stubs_size;
    CpuCompatSite sites[4];
    CpuCompatTable table;
} Fixture;

static void fixture_make(Fixture *x) {
    x->size = 8192;
    x->stubs_size = 8192;
    x->image = code_memory(x->size + x->stubs_size);
    x->stubs = x->image + x->size;
    x->copy = malloc(x->size);
    x->stubs_copy = malloc(x->stubs_size);
    for (size_t i = 0; i < x->size; ++i) x->image[i] = (uint8_t)next_random();
    memset(x->stubs, 0x5a, x->stubs_size);
    const size_t at[] = {100, 300, 700, 2000};
    const Snippet *pick[] = {&snippets[0], &snippets[6], &snippets[13], &snippets[10]};
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(x->image + at[i], pick[i]->bytes, snippet_length(pick[i]));
        x->sites[i] = (CpuCompatSite){(uint32_t)at[i], (uint8_t)snippet_length(pick[i]), 0, {0}};
        memcpy(x->sites[i].parts, pick[i]->parts, CPU_COMPAT_PARTS);
    }
    x->table = (CpuCompatTable){"fixture", x->size, cpu_compat_digest(x->image, x->size, x->sites, 4), x->sites, 4};
    memcpy(x->copy, x->image, x->size);
    memcpy(x->stubs_copy, x->stubs, x->stubs_size);
}
static int fixture_untouched(const Fixture *x) {
    return !memcmp(x->copy, x->image, x->size) && !memcmp(x->stubs_copy, x->stubs, x->stubs_size);
}
static void fixture_free(Fixture *x) {
    code_memory_free(x->image, x->size + x->stubs_size);
    free(x->copy);
    free(x->stubs_copy);
}

static void expect_refused(Fixture *x, CpuCompatStatus status, const char *what) {
    size_t replaced = 99;
    const CpuCompatStatus got = cpu_compat_apply(x->image, x->size, &x->table, CPU_COMPAT_ALL, x->stubs, x->stubs_size, &replaced);
    check(got == status && replaced == 0 && fixture_untouched(x), what, got, status);
}

static void test_validation(void) {
    Fixture x;
    unsigned mutations = 0;
    fixture_make(&x);
    CHECK(cpu_compat_check(x.image, x.size, &x.table) == CPU_COMPAT_OK, "fixture valid");
    size_t replaced = 1;
    CHECK(cpu_compat_apply(x.image, x.size, &x.table, 0, x.stubs, x.stubs_size, &replaced) == CPU_COMPAT_OK && !replaced &&
          fixture_untouched(&x), "nothing changes on a processor that has every instruction");
    fixture_free(&x);

    /* Every byte of every site, changed: the whole table is refused and nothing is written. */
    fixture_make(&x);
    for (unsigned s = 0; s < 4; ++s)
        for (unsigned b = 0; b < x.sites[s].length; ++b) {
            uint8_t *p = x.image + x.sites[s].offset + b;
            const uint8_t was = *p;
            *p ^= 0x10;
            x.copy[x.sites[s].offset + b] = *p;
            expect_refused(&x, CPU_COMPAT_BAD_SITE, "mutated site byte");
            *p = was;
            x.copy[x.sites[s].offset + b] = was;
            ++mutations;
        }
    /* The last site wrong: a partial rewrite of the first three would be worse than none. */
    x.image[x.sites[3].offset] = 0x90;
    x.copy[x.sites[3].offset] = 0x90;
    expect_refused(&x, CPU_COMPAT_BAD_SITE, "partial rewrite refused");
    fixture_free(&x);
    ++mutations;

    fixture_make(&x);
    x.table.size += 4096;
    expect_refused(&x, CPU_COMPAT_UNKNOWN_IMAGE, "image size of another build");
    x.table.size -= 4096;
    x.table.digest ^= 1;
    expect_refused(&x, CPU_COMPAT_BAD_SITE, "digest of another build");
    x.table.digest ^= 1;
    /* Each structural fault with a digest that matches it, so the structure check alone must refuse it. */
#define REFUSED_AS_IS(what) (x.table.digest = cpu_compat_digest(x.image, x.size, x.sites, 4), \
                             check(cpu_compat_check(x.image, x.size, &x.table) == CPU_COMPAT_BAD_SITE, what, 0, 0), \
                             expect_refused(&x, CPU_COMPAT_BAD_SITE, what))
    const CpuCompatSite saved0 = x.sites[0], saved1 = x.sites[1], saved2 = x.sites[2], saved3 = x.sites[3];
    x.sites[1] = x.sites[0];
    REFUSED_AS_IS("overlapping sites");
    x.sites[1] = saved1;
    x.sites[0] = saved3;
    x.sites[3] = saved0;
    REFUSED_AS_IS("unsorted sites");
    x.sites[0] = saved0;
    x.sites[3] = saved3;
    x.sites[2].length = 4;
    x.sites[2].parts[0] = CPU_COMPAT_PART(K_F16, 4);
    REFUSED_AS_IS("site shorter than a jump");
    x.sites[2] = saved2;
    x.sites[2].parts[0] = (uint8_t)(6 << 4 | 5);
    REFUSED_AS_IS("unknown kind");
    x.sites[2] = saved2;
    x.sites[2].parts[0] = CPU_COMPAT_PART(K_LZ, 5);
    REFUSED_AS_IS("part of the wrong kind");
    x.sites[2] = saved2;
    x.sites[2].parts[0] = CPU_COMPAT_PART(0, 5);
    REFUSED_AS_IS("site with nothing to replace");
    x.sites[2] = saved2;
    x.sites[2].parts[1] = CPU_COMPAT_PART(0, 1);
    REFUSED_AS_IS("parts longer than the site");
    x.sites[2] = saved2;
    x.sites[2].parts[2] = CPU_COMPAT_PART(0, 1);
    REFUSED_AS_IS("part after the end");
    x.sites[2] = saved2;
    x.sites[0].shield = 2;
    REFUSED_AS_IS("unknown shield value");
    x.sites[0] = saved0;
    x.sites[3].offset = (uint32_t)x.size - 4;
    REFUSED_AS_IS("site past the image");
    x.sites[3] = saved3;
#undef REFUSED_AS_IS
    x.table.digest = cpu_compat_digest(x.image, x.size, x.sites, 4);
    CHECK(cpu_compat_check(x.image, x.size, &x.table) == CPU_COMPAT_OK, "fixture restored");
    size_t got = 0;
    check(cpu_compat_apply(x.image, x.size, &x.table, CPU_COMPAT_ALL, x.stubs, 64, &got) == CPU_COMPAT_NO_ROOM && fixture_untouched(&x),
          "stub area too small", got, 0);
    check(cpu_compat_apply(x.image, x.size, &x.table, CPU_COMPAT_ALL, x.image + ((size_t)3 << 30), x.stubs_size, &got) ==
              CPU_COMPAT_NO_ROOM && fixture_untouched(&x),
          "stub area out of jump range", got, 0);
    CHECK(cpu_compat_apply(x.image, x.size, NULL, CPU_COMPAT_ALL, x.stubs, x.stubs_size, &got) == CPU_COMPAT_UNKNOWN_IMAGE &&
          fixture_untouched(&x), "no table");
    mutations += 15;
    fixture_free(&x);
    printf("  %u mutations of sites and tables refused with nothing written\n", mutations);

    CHECK(!cpu_compat_boot_table("CUSA57767-9.99") && !cpu_compat_boot_table("") && !cpu_compat_module_table("libfmod.prx.bak"),
          "no table for an unknown build or module");
    for (size_t t = 0; t < cpu_compat_table_count(); ++t) {
        const CpuCompatTable *table = cpu_compat_table(t);
        int sorted = 1;
        for (size_t i = 1; i < table->count; ++i)
            sorted &= table->sites[i - 1].offset + table->sites[i - 1].length <= table->sites[i].offset;
        check(sorted && table->count, "built-in table sorted and disjoint", t, table->count);
    }
    /* Every LZCNT, TZCNT and F16C the instruction inventory (tools\isa_tests.c) counts in the boot image's modules. */
    static const struct { const char *name; unsigned lzcnt, tzcnt, f16c; } covered[] = {
        {"CUSA57767-1.00", 231 + 322 + 35, 283 + 155 + 11, 240}, {"CUSA57767-1.01", 231 + 322 + 35, 283 + 155 + 11, 240},
        {"libfmod.prx", 2, 0, 0}, {"libfmodstudio.prx", 0, 5, 0},
    };
    for (size_t i = 0; i < sizeof covered / sizeof *covered; ++i) {
        const CpuCompatTable *table = cpu_compat_boot_table(covered[i].name);
        unsigned counts[6] = {0};
        for (size_t s = 0; table && s < table->count; ++s)
            for (unsigned p = 0; p < CPU_COMPAT_PARTS; ++p) ++counts[table->sites[s].parts[p] >> 4 < 6 ? table->sites[s].parts[p] >> 4 : 0];
        check(table && counts[K_LZ] == covered[i].lzcnt && counts[K_TZ] == covered[i].tzcnt && counts[K_F16] == covered[i].f16c,
              covered[i].name, counts[K_LZ], counts[K_TZ]);
    }
    printf("  built-in tables: CUSA57767 1.00 and 1.01 each cover 588 LZCNT, 449 TZCNT and 240 F16C; libfmod.prx 2 LZCNT; "
           "libfmodstudio.prx 5 TZCNT\n");
}

/* --- real game code ---------------------------------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    const long long n = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    uint8_t *d = malloc((size_t)n + 1);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *size = (uint64_t)n;
    return d;
}


static void run_real_sites(const char *label, const uint8_t *pristine, const CpuCompatTable *table);


/* The loader's and module loader's path: cpu_compat_plan for the host, cpu_compat_install on a fresh copy laid out as they
 * lay it out (stubs right after the image), for every processor profile and then with everything forced. */
static void install_profiles(const char *label, CpuCompatScope scope, const char *name, const uint8_t *pristine,
                             uint64_t size, uint8_t *work, size_t area) {
    const CpuCompatTable *table = cpu_compat_boot_table(name);
    unsigned table_kinds = 0;
    for (size_t i = 0; i < table->count; ++i) table_kinds |= cpu_compat_site_kinds(&table->sites[i]);
    const size_t profile_count = sizeof profiles / sizeof *profiles;
    unsigned normal = 0, compatible = 0, forced_sites = 0;
    size_t forced_replaced = 0;
    for (size_t i = 0; i <= profile_count; ++i) {
        const int forced = i == profile_count;
        const Profile *p = forced ? &profiles[0] : &profiles[i];
        char cause[160], advice[160];
        if (!forced && !cpu_compat_processor_usable(p->host, cause, sizeof cause, advice, sizeof advice)) continue;
        const CpuCompatPlan plan = cpu_compat_plan(scope, name, p->host, forced ? CPU_COMPAT_ALL : 0);
        const unsigned expect = forced ? CPU_COMPAT_ALL : p->compat;
        check(plan.kinds == expect && (!plan.kinds || plan.table == table), label, plan.kinds, expect);
        check(plan.wanted == (scope == CPU_COMPAT_BOOT_IMAGE ? expect : expect & table_kinds), label, plan.wanted, expect);
        check(plan.stub_bytes <= area, label, plan.stub_bytes, area);
        memcpy(work, pristine, (size_t)size);
        memset(work + size, STUB_FILL, area);
        const CpuCompatOutcome outcome = cpu_compat_install(&plan, work, size, work + size, plan.stub_bytes);
        check(outcome.status == CPU_COMPAT_OK && !outcome.fatal, label, outcome.status, (unsigned long long)i);
        unsigned sites = 0;
        check(installed_as_planned(table, plan.kinds, pristine, work, size, work + size, plan.stub_bytes, outcome.replaced,
                                   &sites),
              label, outcome.replaced, (unsigned long long)i);
        if (!plan.wanted) {
            check(!outcome.replaced && !outcome.message[0] && untouched_stubs(work + size, area), label, outcome.replaced, i);
            ++normal;
        } else {
            ++compatible;
        }
        if (forced) { forced_replaced = outcome.replaced; forced_sites = sites; }
        char native[64], fallback[64], trapped[64];
        cpu_compat_kind_names(CPU_COMPAT_ALL & ~plan.kinds, native, sizeof native);
        cpu_compat_kind_names(plan.kinds & table_kinds, fallback, sizeof fallback);
        cpu_compat_kind_names(plan.kinds & ~table_kinds & (CPU_COMPAT_MOVBE | CPU_COMPAT_BMI1), trapped, sizeof trapped);
        printf("  %-18s %-28s replaced at load: %-24s %5zu instructions in %4u sites; native: %s%s%s\n", label,
               forced ? "forced compatibility" : p->name, *fallback ? fallback : "none", outcome.replaced, sites,
               *native ? native : "none", *trapped ? "; emulated when run: " : "", trapped);
    }
    check(forced_replaced == table_parts(table, CPU_COMPAT_ALL) && forced_sites == table->count, label, forced_replaced,
          forced_sites);
    printf("  %-18s normal path PASS on %u profiles with nothing to replace; compatibility path PASS on %u (forced: %zu "
           "instructions in %u sites, every byte outside them unchanged)\n", label, normal, compatible, forced_replaced,
           forced_sites);
}

/* What fails closed: another build's name, an unknown build, one changed byte in the last site. */
static void install_refusals(const char *label, CpuCompatScope scope, const char *name, const char *other,
                             const uint8_t *pristine, uint64_t size, uint8_t *work, size_t area) {
    const IsaHostFeatures sandy = profiles[2].host;
    const char *names[] = {other, scope == CPU_COMPAT_BOOT_IMAGE ? "CUSA57767-9.99" : NULL};
    for (unsigned k = 0; k < 2; ++k) {
        if (!names[k]) continue;
        const CpuCompatPlan plan = cpu_compat_plan(scope, names[k], sandy, 0);
        memcpy(work, pristine, (size_t)size);
        memset(work + size, STUB_FILL, area);
        const CpuCompatOutcome outcome = cpu_compat_install(&plan, work, size, work + size, area);
        check(outcome.status != CPU_COMPAT_OK && outcome.fatal && !outcome.replaced && !memcmp(work, pristine, (size_t)size) &&
              untouched_stubs(work + size, area), label, outcome.status, k);
    }
    const CpuCompatTable *table = cpu_compat_boot_table(name);
    const CpuCompatPlan plan = cpu_compat_plan(scope, name, sandy, 0);
    memcpy(work, pristine, (size_t)size);
    memset(work + size, STUB_FILL, area);
    work[table->sites[table->count - 1].offset + table->sites[table->count - 1].length - 1] ^= 0x01;
    const uint8_t changed = work[table->sites[table->count - 1].offset + table->sites[table->count - 1].length - 1];
    const CpuCompatOutcome outcome = cpu_compat_install(&plan, work, size, work + size, area);
    work[table->sites[table->count - 1].offset + table->sites[table->count - 1].length - 1] ^= 0x01;
    check(outcome.status == CPU_COMPAT_BAD_SITE && outcome.fatal == (plan.essential != 0) && !outcome.replaced &&
          changed != pristine[table->sites[table->count - 1].offset + table->sites[table->count - 1].length - 1] &&
          !memcmp(work, pristine, (size_t)size) && untouched_stubs(work + size, area), label, outcome.status, 2);
    printf("  %-18s refused with nothing written: %s%sone changed byte in the last site\n", label,
           other ? (scope == CPU_COMPAT_BOOT_IMAGE ? "another build's table, " : "another module's table, ") : "",
           scope == CPU_COMPAT_BOOT_IMAGE ? "an unknown build, " : "");
}

/* A site whose registers can be given values that keep every memory operand inside `scratch`: no rsp, nothing
 * rip-relative, no base or index written before it is used, nothing that can fault or keeps state the harness does not
 * reset (division, x87, MMX). Each memory operand addresses the same aligned scratch byte; other registers stay random.
 * *defined gets the arithmetic flags the architecture defines after the site: an undefined one may differ between the
 * performance and efficiency cores of one processor. */
static int site_inputs(const uint8_t *site, size_t length, uint8_t *scratch, uint64_t *fixed_mask, uint64_t fixed[16],
                       unsigned *defined) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t written = 0;
    *fixed_mask = 0;
    *defined = FLAGS_ALL;
    for (size_t at = 0; at < length;) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, site + at, length - at, &in, ops))) return 0;
        if (in.mnemonic == ZYDIS_MNEMONIC_DIV || in.mnemonic == ZYDIS_MNEMONIC_IDIV || in.meta.isa_ext == ZYDIS_ISA_EXT_X87 ||
            in.meta.isa_ext == ZYDIS_ISA_EXT_MMX)
            return 0;
        if (in.cpu_flags) {
            const unsigned set = in.cpu_flags->modified | in.cpu_flags->set_0 | in.cpu_flags->set_1;
            /* A shift or rotate by cl leaves every flag as it was for a count of 0. */
            int by_cl = 0;
            for (unsigned k = 1; k < in.operand_count_visible; ++k)
                by_cl |= ops[k].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[k].reg.value == ZYDIS_REGISTER_CL &&
                         (in.meta.category == ZYDIS_CATEGORY_SHIFT || in.meta.category == ZYDIS_CATEGORY_ROTATE);
            if (by_cl) *defined &= ~in.cpu_flags->undefined;
            else *defined = ((*defined & ~(set | in.cpu_flags->undefined)) | (set & ~in.cpu_flags->undefined)) & FLAGS_ALL;
        }
        for (unsigned k = 0; k < in.operand_count; ++k) {
            const ZydisDecodedOperand *o = &ops[k];
            if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const ZydisRegister r = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, o->reg.value);
                if (r == ZYDIS_REGISTER_RSP || r == ZYDIS_REGISTER_RIP) return 0;
            }
            if (o->type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
            if (o->mem.base == ZYDIS_REGISTER_RIP || o->mem.base == ZYDIS_REGISTER_RSP || o->mem.segment == ZYDIS_REGISTER_FS ||
                o->mem.segment == ZYDIS_REGISTER_GS || o->mem.disp.value > (1 << 16) || o->mem.disp.value < -(1 << 16))
                return 0;
            const ZydisRegister registers[2] = {o->mem.base, o->mem.index};
            for (unsigned j = 0; j < 2; ++j) {
                if (registers[j] == ZYDIS_REGISTER_NONE) continue;
                const int n = ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, registers[j]));
                if (n < 0 || n > 15 || (written >> n & 1)) return 0;
                const uint64_t value = j == 0 ? (uint64_t)(uintptr_t)(scratch + (1 << 17)) - (uint64_t)o->mem.disp.value : 0;
                if ((*fixed_mask >> n & 1) && fixed[n] != value) return 0;
                fixed[n] = value;
                *fixed_mask |= 1ull << n;
            }
        }
        for (unsigned k = 0; k < in.operand_count; ++k)
            if (ops[k].type == ZYDIS_OPERAND_TYPE_REGISTER && (ops[k].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
                const int n = ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[k].reg.value));
                if (n >= 0 && n < 16) written |= 1ull << n;
            }
        at += in.length;
    }
    return 1;
}

/* Every site of the table that site_inputs accepts, copied with a ret into an image of its own and run natively, then
 * through its stub, with the same states. */
static void run_real_sites(const char *label, const uint8_t *pristine, const CpuCompatTable *table) {
    const IsaHostFeatures host = isa_host_features();
    if (!((host.leaf1_ecx & bit_F16C) && (host.leaf1_ecx & bit_MOVBE) && (host.leaf7_ebx & bit_BMI) && (host.ext1_ecx & bit_LZCNT))) {
        printf("  [skip] the host lacks LZCNT, BMI1, MOVBE or F16C to compare with\n");
        return;
    }
    enum { IMAGE = 4096, STUBS = 4096, SCRATCH = 1 << 18, STATES = 6 };
    uint8_t *image = code_memory(IMAGE + STUBS);
    uint8_t *scratch = code_memory(SCRATCH);
    uint8_t *expected = malloc(SCRATCH);
    unsigned ran = 0, skipped = 0;
    for (size_t i = 0; i < table->count; ++i) {
        const CpuCompatSite *s = &table->sites[i];
        uint64_t fixed[16] = {0}, mask = 0;
        unsigned defined = 0;
        if (!site_inputs(pristine + s->offset, s->length, scratch, &mask, fixed, &defined)) { ++skipped; continue; }
        memset(image, 0xcc, IMAGE + STUBS);
        memcpy(image + 64, pristine + s->offset, s->length);
        image[64 + s->length] = 0xc3;
        CpuCompatSite copy = *s;
        copy.offset = 64;
        const CpuCompatTable one = {"site", IMAGE, cpu_compat_digest(image, IMAGE, &copy, 1), &copy, 1};
        Machine native[STATES];
        const uint64_t seed = random_state;
        for (unsigned v = 0; v < STATES; ++v) {
            for (size_t b = 0; b < SCRATCH; b += 8) { const uint64_t x = (uint64_t)b * 0x9e3779b97f4a7c15ull; memcpy(scratch + b, &x, 8); }
            input_state(&native[v], v, &snippets[1]);
            for (unsigned r = 0; r < 16; ++r) if (mask >> r & 1) native[v].gpr[r] = fixed[r];
            run_code(image + 64, &native[v]);
            if (v == STATES - 1) memcpy(expected, scratch, SCRATCH);
        }
        size_t replaced = 0;
        const CpuCompatStatus status = cpu_compat_apply(image, IMAGE, &one, CPU_COMPAT_ALL, image + IMAGE, STUBS, &replaced);
        check(status == CPU_COMPAT_OK, label, s->offset, status);
        random_state = seed;
        for (unsigned v = 0; v < STATES && status == CPU_COMPAT_OK; ++v) {
            Machine m;
            for (size_t b = 0; b < SCRATCH; b += 8) { const uint64_t x = (uint64_t)b * 0x9e3779b97f4a7c15ull; memcpy(scratch + b, &x, 8); }
            input_state(&m, v, &snippets[1]);
            for (unsigned r = 0; r < 16; ++r) if (mask >> r & 1) m.gpr[r] = fixed[r];
            run_code(image + 64, &m);
            check(same_result(&native[v], &m, defined), label, s->offset, v);
            if (v == STATES - 1) check(!memcmp(expected, scratch, SCRATCH), label, s->offset, 0x100 + v);
        }
        ++ran;
    }
    printf("  %-22s %u real sites run natively and replaced with the same results (%u not runnable alone)\n", label, ran, skipped);
    code_memory_free(image, IMAGE + STUBS);
    code_memory_free(scratch, SCRATCH);
    free(expected);
}

/* A SOSBOOT1 file as src/loader.c reads it: the image and its segments. */
typedef struct {
    uint8_t *file;
    const uint8_t *image;
    uint64_t size, segment_count;
    uint64_t *segments;
} BootFile;

static int read_boot(const char *path, BootFile *b) {
    uint64_t file_size = 0;
    memset(b, 0, sizeof *b);
    b->file = read_file(path, &file_size);
    if (!b->file || file_size < 8 * 14 || memcmp(b->file, "SOSBOOT1", 8)) return 0;
    uint64_t header[16];
    memcpy(header, b->file + 8, sizeof header);
    b->size = header[0];
    b->segment_count = header[2];
    const uint8_t *p = b->file + 8 + 12 * 8 + header[11] * 7 * 8;
    uint64_t bindings;
    memcpy(&bindings, p, 8);
    p += 8 + bindings * 3 * 8;
    b->segments = malloc((size_t)b->segment_count * 3 * sizeof *b->segments);
    memcpy(b->segments, p, (size_t)b->segment_count * 3 * 8);
    b->image = b->file + file_size - b->size;
    return b->size < file_size;
}

#ifdef _WIN32
/* LZCNT, TZCNT and F16C instructions of the image's functions, decoded independently of the generator, that no site
 * replaces as their own kind. */
static size_t uncovered(const uint8_t *image, const RzaImage *functions, const CpuCompatSite *sites, size_t count,
                        size_t *found) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t missing = 0;
    *found = 0;
    for (size_t f = 0; f < functions->count; ++f)
        for (uint64_t at = functions->functions[f].begin; at < functions->functions[f].end;) {
            ZydisDecodedInstruction in;
            if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&decoder, NULL, image + at, functions->functions[f].end - at, &in)))
                break;
            const unsigned kind = in.mnemonic == ZYDIS_MNEMONIC_LZCNT ? K_LZ : in.mnemonic == ZYDIS_MNEMONIC_TZCNT ? K_TZ
                                : in.meta.isa_set == ZYDIS_ISA_SET_F16C ? K_F16 : 0;
            if (kind) {
                ++*found;
                size_t lo = 0, hi = count;
                while (lo < hi) {
                    const size_t mid = lo + (hi - lo) / 2;
                    if (sites[mid].offset <= at) lo = mid + 1; else hi = mid;
                }
                int covered = 0;
                if (lo) {
                    const CpuCompatSite *s = &sites[lo - 1];
                    uint64_t part_at = s->offset;
                    for (unsigned p = 0; p < CPU_COMPAT_PARTS && s->parts[p]; part_at += s->parts[p++] & 15)
                        covered |= part_at == at && (unsigned)(s->parts[p] >> 4) == kind && (s->parts[p] & 15) == in.length;
                }
                missing += !covered;
            }
            at += in.length;
        }
    return missing;
}

static void test_coverage(const char *key, const BootFile *b, const CpuCompatTable *table) {
    RzaImage functions;
    if (!rza_enumerate(b->image, b->size, b->segments, b->segment_count, &functions)) { check(0, key, 0, 0); return; }
    size_t found = 0;
    const size_t missing = uncovered(b->image, &functions, table->sites, table->count, &found);
    check(!missing && found == table_parts(table, CPU_COMPAT_LZCNT | CPU_COMPAT_TZCNT | CPU_COMPAT_F16C), key, missing, found);
    /* A table that lost one site of a kind, with a digest that matches it, still installs: this scan is what catches it. */
    CpuCompatSite *fewer = malloc(table->count * sizeof *fewer);
    for (unsigned kind = K_LZ; kind <= K_F16; ++kind) {
        size_t n = 0, dropped = 0;
        for (size_t i = 0; i < table->count; ++i) {
            const int has = (cpu_compat_site_kinds(&table->sites[i]) >> (kind - 1)) & 1;
            if (has && !dropped) { dropped = 1; continue; }
            fewer[n++] = table->sites[i];
        }
        size_t found_fewer = 0;
        check(dropped && uncovered(b->image, &functions, fewer, n, &found_fewer) > 0, "a missing site is found", kind, n);
    }
    free(fewer);
    rza_free(&functions);
    printf("  %-18s every LZCNT, TZCNT and F16C of the decoded functions is in a site: %zu instructions, %zu sites\n", key,
           found, table->count);
}

/* The red-zone analysis of the rewritten image (stubs at its end) against the original: every function keeps its flags
 * when the stub range is the image's own, and functions with a site would look patched without it. */
static void test_redzone_view(const char *key, const BootFile *b, const uint8_t *patched, size_t area) {
    const uint8_t *pristine = b->image;
    const uint64_t size = b->size, ns = b->segment_count, *segments = b->segments;
    RzaImage before, own, foreign;
    const int ok = rza_enumerate(pristine, size, segments, ns, &before) && rza_enumerate(patched, size, segments, ns, &own) &&
                   rza_enumerate(patched, size, segments, ns, &foreign);
    check(ok, "functions enumerated", 0, 0);
    if (ok) {
        own.own_begin = size;
        own.own_end = size + area;
        const unsigned workers = rza_workers(size);
        check(rza_scan(pristine, size, &before, workers, NULL, 0) >= 0 && rza_scan(patched, size, &own, workers, NULL, 0) >= 0 &&
              rza_scan(patched, size, &foreign, workers, NULL, 0) >= 0, "red-zone analysis ran", 0, 0);
        size_t differ = 0, patched_look = 0;
        for (size_t i = 0; i < before.count && before.count == own.count; ++i) {
            differ += before.functions[i].flags != own.functions[i].flags;
            patched_look += (foreign.functions[i].flags & RZA_HOOKED) && !(before.functions[i].flags & RZA_HOOKED);
        }
        check(before.count == own.count && !differ, key, differ, own.count);
        check(patched_look > 0, "the stub range is what keeps the functions with a site unpatched", patched_look, 0);
        printf("  %-22s red-zone analysis after the rewrite: %zu functions, %zu with other flags; without the stub range %zu "
               "would look patched\n", key, own.count, differ, patched_look);
        rza_free(&before);
        rza_free(&own);
        rza_free(&foreign);
    }
}
#endif

static size_t stub_area(const CpuCompatTable *table) {
    return (cpu_compat_stub_bytes(table, CPU_COMPAT_ALL) + 0xffff) & ~(size_t)0xffff;
}

/* The other table of the same scope, whose image is not this one. */
static const char *other_table(const CpuCompatTable *table) {
    for (size_t t = 0; t < cpu_compat_table_count(); ++t) {
        const CpuCompatTable *other = cpu_compat_table(t);
        if (other != table && !strncmp(other->name, "CUSA", 4) == !strncmp(table->name, "CUSA", 4)) return other->name;
    }
    return NULL;
}

static void test_image(const char *key, const char *path) {
    const CpuCompatTable *table = cpu_compat_boot_table(key);
    BootFile b;
    check(table && read_boot(path, &b) && b.size == table->size, "the build's boot image and table", 0, 0);
    if (!table || !b.image || b.size != table->size) { free(b.file); free(b.segments); return; }
    const size_t area = stub_area(table);
    uint8_t *work = code_memory((size_t)b.size + area);
    check(cpu_compat_check(b.image, b.size, table) == CPU_COMPAT_OK, key, 0, 0);
    run_real_sites(key, b.image, table);
    install_profiles(key, CPU_COMPAT_BOOT_IMAGE, key, b.image, b.size, work, area);
#ifdef _WIN32
    test_redzone_view(key, &b, work, area);
    test_coverage(key, &b, table);
#endif
    install_refusals(key, CPU_COMPAT_BOOT_IMAGE, key, other_table(table), b.image, b.size, work, area);
    code_memory_free(work, (size_t)b.size + area);
    free(b.file);
    free(b.segments);
}

static void test_module(const char *name, const char *directory) {
    const CpuCompatTable *table = cpu_compat_module_table(name);
    check(table != NULL, "a table for the module", 0, 0);
    if (!table) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.code", directory, name);
    uint64_t code_size = 0;
    uint8_t *code = read_file(path, &code_size);
    if (!code || code_size > table->size) { check(0, "module code", 0, 0); free(code); return; }
    uint8_t *pristine = calloc((size_t)table->size, 1);
    memcpy(pristine, code, (size_t)code_size);
    const size_t area = stub_area(table);
    uint8_t *work = code_memory((size_t)table->size + area);
    check(cpu_compat_check(pristine, table->size, table) == CPU_COMPAT_OK, name, 0, 0);
    run_real_sites(name, pristine, table);
    install_profiles(name, CPU_COMPAT_MODULE, name, pristine, table->size, work, area);
    install_refusals(name, CPU_COMPAT_MODULE, name, other_table(table), pristine, table->size, work, area);
    code_memory_free(work, (size_t)table->size + area);
    free(pristine);
    free(code);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 4 && !strcmp(argv[1], "--image")) {
        printf("Real boot image %s\n", argv[2]);
        test_image(argv[2], argv[3]);
    } else if (argc >= 4 && !strcmp(argv[1], "--module")) {
        printf("Real module %s\n", argv[2]);
        test_module(argv[2], argv[3]);
    } else {
        printf("Processor profiles\n");
        test_profiles();
        test_plans();
        printf("F16C conversions\n");
        test_conversions();
        printf("Stubs run natively\n");
        test_stubs();
        printf("Tables\n");
        test_validation();
    }
    printf("%u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
