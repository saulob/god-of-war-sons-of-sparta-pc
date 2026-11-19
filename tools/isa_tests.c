/* Headless ISA compatibility test: the SSE4a emulation and INSERTQ rewrite against a bitwise software reference,
 * feature selection, and (--scan) the instruction boundaries of the real executable images. */
#include "isa_emulate.h"
#include <cpuid.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <Zydis/Zydis.h>

static unsigned checks, failures;

static void check(int ok, const char *what, unsigned long long a, unsigned long long b) {
    ++checks;
    if (ok) return;
    if (++failures <= 40) printf("  FAIL %s (%#llx, %#llx)\n", what, a, b);
}
#define CHECK(condition, what) check(!!(condition), what, 0, 0)

static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static uint64_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}
static void fill_random(void *p, size_t n) {
    uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)(next_random() >> 24);
}
static uint64_t load64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void store64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* Bit-at-a-time references; a field that crosses bit 63 is architecturally undefined and not tested. */
static uint64_t reference_insert(uint64_t destination, uint64_t source, unsigned length, unsigned index) {
    for (unsigned i = 0; i < length; ++i) {
        const uint64_t bit = 1ull << (index + i);
        destination = (source >> i & 1) ? destination | bit : destination & ~bit;
    }
    return destination;
}
static uint64_t reference_extract(uint64_t source, unsigned length, unsigned index) {
    uint64_t result = 0;
    for (unsigned i = 0; i < length; ++i) result |= (source >> (index + i) & 1) << i;
    return result;
}

static size_t rex_prefix(uint8_t *out, unsigned reg, unsigned rm) {
    const unsigned rex = (reg >> 3 << 2) | (rm >> 3);
    if (!rex) return 0;
    out[0] = (uint8_t)(0x40 | rex);
    return 1;
}
static size_t encode_insertq_imm(uint8_t *out, unsigned destination, unsigned source, unsigned length, unsigned index) {
    size_t n = 0;
    out[n++] = 0xf2;
    n += rex_prefix(out + n, destination, source);
    out[n++] = 0x0f; out[n++] = 0x78;
    out[n++] = (uint8_t)(0xc0 | (destination & 7) << 3 | (source & 7));
    out[n++] = (uint8_t)length; out[n++] = (uint8_t)index;
    return n;
}
static size_t encode_extrq_imm(uint8_t *out, unsigned reg, unsigned length, unsigned index) {
    size_t n = 0;
    out[n++] = 0x66;
    n += rex_prefix(out + n, 0, reg);
    out[n++] = 0x0f; out[n++] = 0x78;
    out[n++] = (uint8_t)(0xc0 | (reg & 7));
    out[n++] = (uint8_t)length; out[n++] = (uint8_t)index;
    return n;
}
static size_t encode_reg_form(uint8_t *out, int insert, unsigned destination, unsigned source) {
    size_t n = 0;
    out[n++] = insert ? 0xf2 : 0x66;
    n += rex_prefix(out + n, destination, source);
    out[n++] = 0x0f; out[n++] = 0x79;
    out[n++] = (uint8_t)(0xc0 | (destination & 7) << 3 | (source & 7));
    return n;
}

typedef struct {
    uint64_t gpr[16], flags;
    uint8_t xmm[16][16];
} State;

static void random_state_fill(State *s) {
    fill_random(s, sizeof *s);
    s->flags &= 0xff5;
}
/* All of dst except bytes 0-7 of xmm[destination] must be unchanged. */
static void expect_only_low_qword(const State *before, const State *after, unsigned destination, uint64_t expected,
                                  const char *what) {
    State want = *before;
    store64(want.xmm[destination], expected);
    check(!memcmp(&want, after, sizeof want), what, load64(after->xmm[destination]), expected);
}

static void test_emulation(void) {
    static const struct { unsigned length, index; } fields[] = {
        {16, 48}, {8, 8}, {24, 0}, {1, 0}, {1, 63}, {64, 0}, {63, 1}, {32, 32}, {7, 57}, {13, 20}, {2, 62}, {5, 0},
    };
    State before, after;
    uint8_t code[16];
    for (unsigned destination = 0; destination < 16; ++destination)
        for (unsigned source = 0; source < 16; source += destination < 3 ? 1 : 5)
            for (size_t f = 0; f < sizeof fields / sizeof *fields; ++f) {
                random_state_fill(&before);
                if (f == 0) memset(before.xmm[source], 0xff, 8);
                if (f == 1) memset(before.xmm[destination], 0, 16);
                const size_t length = encode_insertq_imm(code, destination, source, fields[f].length & 63, fields[f].index);
                after = before;
                const size_t done = isa_emulate(code, 0x1000, after.gpr, &after.flags, after.xmm);
                CHECK(done == length, "INSERTQ imm length");
                expect_only_low_qword(&before, &after, destination,
                                      reference_insert(load64(before.xmm[destination]), load64(before.xmm[source]),
                                                       fields[f].length, fields[f].index), "INSERTQ imm");
            }
    for (unsigned reg = 0; reg < 16; ++reg)
        for (unsigned length = 1; length <= 64; ++length)
            for (unsigned index = 0; index + length <= 64; index += length < 8 ? 1 : 7) {
                random_state_fill(&before);
                if (length == 64) store64(before.xmm[reg], ~0ull);
                const size_t size = encode_extrq_imm(code, reg, length & 63, index);
                after = before;
                CHECK(isa_emulate(code, 0x1000, after.gpr, &after.flags, after.xmm) == size, "EXTRQ imm length");
                expect_only_low_qword(&before, &after, reg, reference_extract(load64(before.xmm[reg]), length, index), "EXTRQ imm");
            }
    for (unsigned destination = 0; destination < 16; destination += 3)
        for (unsigned source = 0; source < 16; source += 2)
            for (unsigned length = 1; length <= 64; length += 3)
                for (unsigned index = 0; index + length <= 64; index += 9) {
                    random_state_fill(&before);
                    const uint64_t control = ((uint64_t)index << 8 | (length & 63)) | (next_random() & ~0x3f3full);
                    const uint64_t source_value = load64(before.xmm[source]);
                    /* The same register as source and control when they coincide. */
                    if (source != destination) {
                        store64(before.xmm[source] + 8, control);
                        after = before;
                        size_t size = encode_reg_form(code, 1, destination, source);
                        CHECK(isa_emulate(code, 0x1000, after.gpr, &after.flags, after.xmm) == size, "INSERTQ reg length");
                        expect_only_low_qword(&before, &after, destination,
                                              reference_insert(load64(before.xmm[destination]), source_value, length, index),
                                              "INSERTQ reg");
                        store64(before.xmm[source], control);
                        after = before;
                        size = encode_reg_form(code, 0, destination, source);
                        CHECK(isa_emulate(code, 0x1000, after.gpr, &after.flags, after.xmm) == size, "EXTRQ reg length");
                        expect_only_low_qword(&before, &after, destination, reference_extract(load64(before.xmm[destination]), length, index),
                                              "EXTRQ reg");
                    }
                }

    /* Zero and all-ones values. */
    memset(&before, 0, sizeof before);
    memset(before.xmm[3], 0xff, 16);
    encode_insertq_imm(code, 3, 0, 16, 48);
    after = before;
    isa_emulate(code, 0, after.gpr, &after.flags, after.xmm);
    expect_only_low_qword(&before, &after, 3, 0x0000ffffffffffffull, "INSERTQ zero into ones");
    memset(&before, 0xff, sizeof before);
    memset(before.xmm[2], 0, 16);
    encode_insertq_imm(code, 4, 2, 16, 48);
    after = before;
    isa_emulate(code, 0, after.gpr, &after.flags, after.xmm);
    expect_only_low_qword(&before, &after, 4, 0x0000ffffffffffffull, "INSERTQ zero source");
}

static void test_stores(void) {
    State s;
    uint8_t buffer[96];
    for (int variant = 0; variant < 5; ++variant)
        for (int dword = 0; dword < 2; ++dword) {
            random_state_fill(&s);
            memset(buffer, 0xcc, sizeof buffer);
            uint8_t code[16], *at = buffer + 17;
            const uint8_t prefix = dword ? 0xf3 : 0xf2;
            size_t length = 0;
            unsigned reg = 0;
            uint64_t rip = (uintptr_t)buffer;
            if (variant == 0) {
                const uint8_t b[] = {prefix, 0x0f, 0x2b, 0x00};
                memcpy(code, b, sizeof b), length = sizeof b;
                s.gpr[0] = (uintptr_t)at;
            } else if (variant == 1) {
                const uint8_t b[] = {prefix, 0x0f, 0x2b, 0x4b, 0x08};
                memcpy(code, b, sizeof b), length = sizeof b, reg = 1;
                s.gpr[3] = (uintptr_t)(at - 8);
            } else if (variant == 2) {
                const uint8_t b[] = {prefix, 0x47, 0x0f, 0x2b, 0xbc, 0xac, 0x10, 0x00, 0x00, 0x00};
                memcpy(code, b, sizeof b), length = sizeof b, reg = 15;
                s.gpr[12] = (uintptr_t)at - 16 - 4 * 3;
                s.gpr[13] = 3;
            } else if (variant == 3) {
                const uint8_t b[] = {prefix, 0x0f, 0x2b, 0x05, 0, 0, 0, 0};
                memcpy(code, b, sizeof b), length = sizeof b;
                const int32_t disp = (int32_t)((int64_t)(uintptr_t)at - (int64_t)(rip + length));
                memcpy(code + 4, &disp, 4);
            } else {
                const uint8_t b[] = {prefix, 0x44, 0x0f, 0x2b, 0x45, 0xf8};
                memcpy(code, b, sizeof b), length = sizeof b, reg = 8;
                s.gpr[5] = (uintptr_t)at + 8;
            }
            State after = s;
            const size_t done = isa_emulate(code, rip, after.gpr, &after.flags, after.xmm);
            check(done == length, "MOVNTS length", done, length);
            const size_t bytes = dword ? 4 : 8;
            uint8_t expected[96];
            memset(expected, 0xcc, sizeof expected);
            memcpy(expected + 17, s.xmm[reg], bytes);
            check(!memcmp(expected, buffer, sizeof buffer), "MOVNTS memory", (unsigned long long)variant, (unsigned long long)dword);
            check(!memcmp(&s, &after, sizeof s), "MOVNTS registers", (unsigned long long)variant, 0);
        }
}

static void test_rejections(void) {
    State s;
    random_state_fill(&s);
    State copy = s;
    static const uint8_t cases[][8] = {
        {0xf3, 0x0f, 0x78, 0xc2, 0x10, 0x30},   /* F3 0F 78 is not SSE4a */
        {0xf2, 0x0f, 0x78, 0x02, 0x10, 0x30},   /* memory operand */
        {0x66, 0x0f, 0x78, 0xca, 0x10, 0x30},   /* EXTRQ is /0 */
        {0x66, 0x0f, 0x2b, 0x00},               /* MOVNTPD */
        {0xf2, 0x0f, 0x2b, 0xc0},               /* register operand */
        {0xf2, 0x0f, 0x10, 0xc2},               /* MOVSD */
        {0x90},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        check(!isa_emulate(cases[i], 0, s.gpr, &s.flags, s.xmm) && !memcmp(&s, &copy, sizeof s), "rejected", i, 0);
    static const uint8_t insertq[] = {0xf2, 0x0f, 0x78, 0xc2, 0x10, 0x30};
    check(!isa_emulate(insertq, 0, s.gpr, &s.flags, NULL), "no xmm state", 0, 0);

    /* The other emulated sets are unaffected. */
    uint8_t memory[4] = {0x11, 0x22, 0x33, 0x44};
    s.gpr[0] = (uintptr_t)memory;
    static const uint8_t movbe[] = {0x0f, 0x38, 0xf0, 0x00};
    CHECK(isa_emulate(movbe, 0, s.gpr, &s.flags, s.xmm) == 4 && (uint32_t)s.gpr[0] == 0x11223344u, "MOVBE");
    s.gpr[1] = 0xb0;
    static const uint8_t blsr[] = {0xc4, 0xe2, 0x78, 0xf3, 0xc9};
    CHECK(isa_emulate(blsr, 0, s.gpr, &s.flags, s.xmm) == 5 && (uint32_t)s.gpr[0] == 0xa0u, "BLSR");
}

/* Runs generated code between a prologue that loads all sixteen ymm registers and flags and an epilogue that stores
 * them, inside the lea rsp,[rsp-128] bracket the rewriter puts every stub in. */
typedef struct {
    uint8_t in[16][32], out[16][32];
    uint64_t flags_in, flags_out, rsp_before, rsp_after, canary_ok;
} Harness;

__asm__(
    ".text\n.intel_syntax noprefix\n.p2align 4\n"
    ".globl isa_harness_head\nisa_harness_head:\n"
    "mov r10, rcx\n"
    ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\nvmovdqu ymm\\i, [r10+32*\\i]\n.endr\n"
    "push qword ptr [r10+1024]\npopfq\n"
    "mov [r10+1040], rsp\n"
    "lea rdi, [rsp-128]\nmov rax, 0xA5A5A5A5A5A5A5A5\nmov ecx, 15\nrep stosq\n"
    "lea rsp, [rsp-128]\n"
    ".globl isa_harness_slot\nisa_harness_slot:\n"
    ".globl isa_harness_tail\nisa_harness_tail:\n"
    "lea rsp, [rsp+128]\n"
    "pushfq\npop qword ptr [r10+1032]\n"
    "mov [r10+1048], rsp\n"
    "lea rdi, [rsp-128]\nmov rax, 0xA5A5A5A5A5A5A5A5\nmov ecx, 15\nmov edx, 1\n"
    "1: cmp [rdi], rax\nje 2f\nxor edx, edx\n2: add rdi, 8\ndec ecx\njnz 1b\n"
    "mov [r10+1056], rdx\n"
    ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\nvmovdqu [r10+512+32*\\i], ymm\\i\n.endr\n"
    "ret\n"
    ".globl isa_harness_end\nisa_harness_end:\n"
    ".att_syntax prefix\n");
extern const char isa_harness_head[], isa_harness_slot[], isa_harness_tail[], isa_harness_end[];

static void run_harness(const void *entry, Harness *h) {
    __asm__ volatile("call *%1"
                     : "+c"(h)
                     : "r"(entry)
                     : "memory", "cc", "rax", "rdx", "rdi", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                       "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static uint8_t *harness_code;
static size_t harness_stub_size;

static int build_harness(const uint8_t *instruction) {
    const size_t head = (size_t)(isa_harness_slot - isa_harness_head), tail = (size_t)(isa_harness_end - isa_harness_tail);
    memcpy(harness_code, isa_harness_head, head);
    harness_stub_size = isa_insertq_rewrite(instruction, harness_code + head);
    if (!harness_stub_size) return 0;
    memcpy(harness_code + head + harness_stub_size, isa_harness_tail, tail);
    return 1;
}

static void check_rewrite(unsigned destination, unsigned source, unsigned length_field, unsigned index, uint64_t flags) {
    uint8_t code[16];
    const size_t size = encode_insertq_imm(code, destination, source, length_field, index);
    (void)size;
    CHECK(build_harness(code), "rewrite accepted");
    CHECK(harness_stub_size <= ISA_INSERTQ_REWRITE_MAX, "rewrite size");
    Harness h;
    fill_random(&h, sizeof h);
    h.flags_in = flags;
    Harness expected = h;
    run_harness(harness_code, &h);
    const unsigned length = length_field & 63 ? length_field & 63 : 64;
    memcpy(expected.out, expected.in, sizeof expected.in);
    store64(expected.out[destination], reference_insert(load64(h.in[destination]), load64(h.in[source]), length, index));
    check(!memcmp(expected.out, h.out, sizeof h.out), "rewrite registers", destination, source);
    check((h.flags_out & 0x8d5) == (flags & 0x8d5), "rewrite flags", h.flags_out, flags);
    check(h.rsp_after == h.rsp_before, "rewrite rsp", h.rsp_after, h.rsp_before);
    check(h.canary_ok == 1, "rewrite red zone", 0, 0);
}

static void test_rewrite(void) {
    harness_code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    CHECK(harness_code, "executable buffer");
    if (!harness_code) return;
    static const struct { unsigned length, index; } fields[] = {
        {16, 48}, {8, 8}, {24, 0}, {1, 0}, {1, 63}, {0, 0}, {63, 1}, {32, 32}, {7, 57}, {13, 20}, {2, 62}, {0x50, 48},
    };
    for (unsigned destination = 0; destination < 16; ++destination)
        for (unsigned source = 0; source < 16; ++source)
            for (size_t f = 0; f < sizeof fields / sizeof *fields; ++f)
                check_rewrite(destination, source, fields[f].length, fields[f].index, (destination + source) & 1 ? 0x8d7 : 0x202);
    static const unsigned pairs[][2] = {{0, 2}, {0, 0}, {8, 9}, {15, 14}, {14, 15}, {13, 15}, {15, 15}, {7, 14}, {14, 14}};
    for (size_t p = 0; p < sizeof pairs / sizeof *pairs; ++p)
        for (unsigned length = 1; length <= 64; ++length)
            for (unsigned index = 0; index + length <= 64; ++index)
                check_rewrite(pairs[p][0], pairs[p][1], length & 63, index, 0x8d7);

    uint8_t code[16];
    encode_insertq_imm(code, 0, 2, 17, 48);
    CHECK(!isa_insertq_rewrite(code, harness_code), "field past bit 63 is not rewritten");
    encode_insertq_imm(code, 0, 2, 0, 1);
    CHECK(!isa_insertq_rewrite(code, harness_code), "64-bit field at index 1 is not rewritten");
    encode_extrq_imm(code, 1, 8, 8);
    CHECK(!isa_insertq_rewrite(code, harness_code), "EXTRQ is not rewritten");
    encode_reg_form(code, 1, 0, 1);
    CHECK(!isa_insertq_rewrite(code, harness_code), "INSERTQ reg form is not rewritten");
    static const uint8_t memory_form[] = {0xf2, 0x0f, 0x78, 0x02, 0x10, 0x30};
    CHECK(!isa_insertq_rewrite(memory_form, harness_code), "memory operand is not rewritten");
    VirtualFree(harness_code, 0, MEM_RELEASE);
}

static void test_features(void) {
    IsaHostFeatures host = {0, 0, 0};
    CHECK(isa_emulation_needed(host) == (ISA_EMULATE_MOVBE | ISA_EMULATE_BMI1 | ISA_EMULATE_SSE4A), "synthetic host without");
    host.ext1_ecx = bit_SSE4a;
    CHECK(isa_emulation_needed(host) == (ISA_EMULATE_MOVBE | ISA_EMULATE_BMI1), "synthetic host with SSE4a");
    host.ext1_ecx = 0;
    host.leaf1_ecx = bit_MOVBE;
    host.leaf7_ebx = bit_BMI;
    CHECK(isa_emulation_needed(host) == ISA_EMULATE_SSE4A, "synthetic host with MOVBE and BMI1 only");
    host.ext1_ecx = bit_SSE4a;
    CHECK(isa_emulation_needed(host) == 0, "synthetic host with all three");

    char sets[96];
    host = (IsaHostFeatures){bit_MOVBE, bit_BMI, 0};
    CHECK(isa_missing_sets(host, 1, sets, sizeof sets) == 1 && !strcmp(sets, "SSE4a"), "missing set names SSE4a");
    host.ext1_ecx = bit_SSE4a;
    CHECK(isa_missing_sets(host, 1, sets, sizeof sets) == 0, "no emulated sets missing");

    const IsaHostFeatures real = isa_host_features();
    CHECK(!(isa_emulation_needed(real) & ISA_EMULATE_SSE4A) == !!(real.ext1_ecx & bit_SSE4a), "host SSE4a decides emulation");
    char name[64];
    isa_host_name(name, sizeof name);
    printf("  host: %s, SSE4a %s, AVX %s: %s\n", name, real.ext1_ecx & bit_SSE4a ? "native" : "emulated",
           real.leaf1_ecx & bit_AVX ? "yes" : "no", isa_emulation_needed(real) & ISA_EMULATE_SSE4A ? "rewrite/emulate" : "native");
}

/* --scan: <dir> holds NAME.code (an image's executable bytes) and NAME.fn (its functions, hex begin and end per line),
 * written by tools\export_code_ranges.py. */
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data) { data[n] = 0; *size = (size_t)n; }
    return data;
}

static int sse4a_mnemonic(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_INSERTQ || m == ZYDIS_MNEMONIC_EXTRQ || m == ZYDIS_MNEMONIC_MOVNTSD || m == ZYDIS_MNEMONIC_MOVNTSS;
}

static int scan_module(const char *directory, const char *name, uint64_t expect_insertq, const uint64_t *required, size_t required_count) {
    char path[600];
    size_t code_size = 0, fn_size = 0;
    snprintf(path, sizeof path, "%s\\%s.code", directory, name);
    uint8_t *code = read_file(path, &code_size);
    snprintf(path, sizeof path, "%s\\%s.fn", directory, name);
    uint8_t *fn = read_file(path, &fn_size);
    if (!code || !fn) {
        if (expect_insertq) check(0, "cannot read the module", 0, 0);
        else printf("  %-34s [skip] not exported\n", name);
        free(code); free(fn);
        return 0;
    }

    uint8_t *starts = calloc(code_size / 8 + 1, 1);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t functions = 0, undecodable = 0, insertq = 0, extrq = 0, movnt = 0, rewritten = 0, emulated = 0;
    uint64_t *sites = calloc(1 << 16, sizeof *sites);
    size_t site_count = 0;
    for (char *line = (char *)fn; *line;) {
        char *end;
        const uint64_t begin = strtoull(line, &end, 16), finish = strtoull(end, &line, 16);
        while (*line == '\r' || *line == '\n') ++line;
        if (finish > code_size || begin >= finish) continue;
        ++functions;
        for (uint64_t at = begin; at < finish;) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, code + at, (ZyanUSize)(finish - at), &in, ops))) { ++undecodable; break; }
            starts[at >> 3] |= (uint8_t)(1u << (at & 7));
            if (sse4a_mnemonic(in.mnemonic)) {
                if (site_count < (1 << 16)) sites[site_count++] = at;
                insertq += in.mnemonic == ZYDIS_MNEMONIC_INSERTQ;
                extrq += in.mnemonic == ZYDIS_MNEMONIC_EXTRQ;
                movnt += in.mnemonic == ZYDIS_MNEMONIC_MOVNTSD || in.mnemonic == ZYDIS_MNEMONIC_MOVNTSS;
                if (in.mnemonic == ZYDIS_MNEMONIC_INSERTQ) {
                    uint8_t out[ISA_INSERTQ_REWRITE_MAX + 16];
                    const int immediate = in.raw.imm[0].size == 8 && in.raw.imm[1].size == 8;
                    rewritten += isa_insertq_rewrite(code + at, out) != 0;
                    if (immediate && in.length != 6) check(0, "INSERTQ length", at, in.length);
                }
                if (in.mnemonic != ZYDIS_MNEMONIC_MOVNTSD && in.mnemonic != ZYDIS_MNEMONIC_MOVNTSS) {
                    State s;
                    memset(&s, 0, sizeof s);
                    emulated += isa_emulate(code + at, 0, s.gpr, &s.flags, s.xmm) == in.length;
                }
            }
            at += in.length;
        }
    }

    /* Every byte pattern that is SSE4a must be an instruction Zydis decoded at that very byte (a prefix may precede it). */
    uint64_t patterns = 0, off_boundary = 0;
    for (size_t at = 0; at + 6 <= code_size; ++at) {
        const uint8_t prefix = code[at];
        if (prefix != 0xf2 && prefix != 0xf3 && prefix != 0x66) continue;
        size_t p = at + 1;
        if ((code[p] & 0xf0) == 0x40) ++p;
        if (code[p] != 0x0f) continue;
        const uint8_t opcode = code[p + 1], modrm = code[p + 2];
        const int sse4a = ((opcode == 0x78 || opcode == 0x79) && prefix != 0xf3 && modrm >> 6 == 3 &&
                           (opcode == 0x79 || prefix == 0xf2 || !(modrm >> 3 & 7))) ||
                          (opcode == 0x2b && prefix != 0x66 && modrm >> 6 != 3);
        if (!sse4a) continue;
        ++patterns;
        if (!(starts[at >> 3] >> (at & 7) & 1)) ++off_boundary;
    }
    printf("  %-34s %6" PRIu64 " functions, INSERTQ %" PRIu64 ", EXTRQ %" PRIu64 ", MOVNTSx %" PRIu64
           ", rewritable %" PRIu64 ", emulated %" PRIu64 ", byte patterns %" PRIu64 ", off an instruction boundary %" PRIu64
           ", undecodable functions %" PRIu64 "\n",
           name, functions, insertq, extrq, movnt, rewritten, emulated, patterns, off_boundary, undecodable);
    check(insertq == expect_insertq, "INSERTQ count", insertq, expect_insertq);
    check(rewritten == insertq && emulated == insertq + extrq, "every site is rewritable and emulated", rewritten, insertq);
    check(patterns == insertq + extrq + movnt && !off_boundary, "byte patterns agree with the decoder", patterns, insertq + extrq + movnt);
    for (size_t r = 0; r < required_count; ++r) {
        size_t i = 0;
        while (i < site_count && sites[i] != required[r]) ++i;
        check(i < site_count, "required site decoded", required[r], 0);
        if (i < site_count) {
            static const uint8_t expected[] = {0xf2, 0x0f, 0x78, 0xc2, 0x10, 0x30};
            check(!memcmp(code + required[r], expected, sizeof expected), "required site bytes", required[r], 0);
        }
    }
    free(starts); free(sites); free(code); free(fn);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "--scan")) {
        const char *directory = argv[2];
        const uint64_t required[] = {0x216da70};
        printf("Real binary scan\n");
        scan_module(directory, "eboot.bin", 51, NULL, 0);
        scan_module(directory, "Il2cppUserAssemblies.prx", 12, required, 1);
        const char *none[] = {"PS4Util.prx", "libc.prx", "libSceFios2.prx", "libfmod.prx", "libfmodstudio.prx",
                              "libresonanceaudio.prx", "lib_burst_generated.prx", "SaveData.prx", "CommonDialog.prx",
                              "UnityNpToolkit2.prx", "libSceNpToolkit2.prx", "libSceJobManager.prx"};
        for (size_t i = 0; i < sizeof none / sizeof *none; ++i) scan_module(directory, none[i], 0, NULL, 0);
    } else {
        printf("SSE4a emulation\n");
        test_emulation();
        test_stores();
        test_rejections();
        printf("INSERTQ rewrite\n");
        if (isa_host_features().leaf1_ecx & bit_AVX) test_rewrite();
        else printf("  [skip] the host has no AVX\n");
        printf("Feature selection\n");
        test_features();
    }
    printf("%u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
