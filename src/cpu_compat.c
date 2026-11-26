#include "cpu_compat.h"

#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_compat_sites.inc"

enum { JMP_REL32 = 5, REL32_REACH = 0x7ff00000, EMULATED_MAX = 4096 };

typedef struct {
    uint8_t bytes[CPU_COMPAT_SITE_MAX];
    uint64_t rip;
} Emulated;

static Emulated emulated[EMULATED_MAX];
static unsigned emulated_count;

const char *cpu_compat_status_text(CpuCompatStatus status) {
    switch (status) {
    case CPU_COMPAT_OK: return "ok";
    case CPU_COMPAT_UNKNOWN_IMAGE: return "not a supported game image";
    case CPU_COMPAT_BAD_SITE: return "unexpected bytes at a site";
    case CPU_COMPAT_NO_ROOM: return "stub area too small or too far";
    }
    return "";
}

unsigned cpu_compat_needed(IsaHostFeatures host) {
    unsigned kinds = 0;
    if (!(host.ext1_ecx & bit_LZCNT)) kinds |= CPU_COMPAT_LZCNT;
    if (!(host.leaf7_ebx & bit_BMI)) kinds |= CPU_COMPAT_TZCNT | CPU_COMPAT_BMI1;
    if (!(host.leaf1_ecx & bit_F16C)) kinds |= CPU_COMPAT_F16C;
    if (!(host.leaf1_ecx & bit_MOVBE)) kinds |= CPU_COMPAT_MOVBE;
    return kinds;
}

unsigned cpu_compat_forced(void) {
    const char *value = getenv("SOS_CPU_COMPAT");
    return value && !strcmp(value, "force") ? CPU_COMPAT_ALL : 0;
}

size_t cpu_compat_table_count(void) { return sizeof cpu_compat_tables / sizeof *cpu_compat_tables; }
const CpuCompatTable *cpu_compat_table(size_t index) {
    return index < cpu_compat_table_count() ? &cpu_compat_tables[index] : NULL;
}

static const CpuCompatTable *named(const char *name) {
    for (size_t i = 0; name && *name && i < cpu_compat_table_count(); ++i)
        if (!strcmp(cpu_compat_tables[i].name, name)) return &cpu_compat_tables[i];
    return NULL;
}

const CpuCompatTable *cpu_compat_boot_table(const char *build_key) { return named(build_key); }
const CpuCompatTable *cpu_compat_module_table(const char *file_name) { return named(file_name); }

static unsigned part_kind(uint8_t part) { return part >> 4 ? 1u << ((part >> 4) - 1) : 0; }
static unsigned part_length(uint8_t part) { return part & 15; }

unsigned cpu_compat_site_kinds(const CpuCompatSite *site) {
    unsigned kinds = 0;
    for (unsigned p = 0; p < CPU_COMPAT_PARTS; ++p) kinds |= part_kind(site->parts[p]);
    return kinds;
}

uint64_t cpu_compat_digest(const uint8_t *image, uint64_t size, const CpuCompatSite *sites, size_t count) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < count; ++i) {
        const CpuCompatSite *s = &sites[i];
        const uint8_t head[] = {(uint8_t)s->offset, (uint8_t)(s->offset >> 8), (uint8_t)(s->offset >> 16),
                                (uint8_t)(s->offset >> 24), s->length, s->shield, s->parts[0], s->parts[1], s->parts[2],
                                s->parts[3]};
        for (size_t k = 0; k < sizeof head; ++k) hash = (hash ^ head[k]) * 0x100000001b3ull;
        for (size_t k = 0; k < sites[i].length && (uint64_t)sites[i].offset + k < size; ++k)
            hash = (hash ^ image[sites[i].offset + k]) * 0x100000001b3ull;
    }
    return hash;
}

/* Length of modrm, sib and displacement at p; *rip_relative and *base (the base register, 16 for none) describe it. */
static size_t modrm_length(const uint8_t *p, size_t available, int rex_b, int *rip_relative, unsigned *base) {
    if (!available) return 0;
    const unsigned mod = p[0] >> 6, rm = p[0] & 7;
    size_t n = 1;
    *rip_relative = 0;
    *base = 16;
    if (mod == 3) return 1;
    *base = rm | (rex_b ? 8u : 0u);
    if (rm == 4) {
        if (available < 2) return 0;
        const unsigned sib_base = p[1] & 7;
        *base = sib_base | (rex_b ? 8u : 0u);
        n = 2;
        if (mod == 0 && sib_base == 5) { *base = 16; n += 4; }
    } else if (mod == 0 && rm == 5) {
        *rip_relative = 1;
        *base = 16;
        n += 4;
    }
    n += mod == 1 ? 1 : mod == 2 ? 4 : 0;
    return n <= available ? n : 0;
}

typedef struct {
    size_t length, opcode_at, rep_at;
    unsigned bits, destination, memory, rsp_base;
} CountForm;

/* [66] F3 [REX] 0F BD|BC /r (LZCNT|TZCNT), in either prefix order, without a rip-relative operand. */
static int count_form(const uint8_t *p, size_t available, uint8_t opcode, CountForm *form) {
    size_t i = 0, rep = (size_t)-1;
    int operand16 = 0;
    for (; i < available && i < 2 && (p[i] == 0x66 || p[i] == 0xf3); ++i) {
        if (p[i] == 0x66) { if (operand16) return 0; operand16 = 1; }
        else { if (rep != (size_t)-1) return 0; rep = i; }
    }
    unsigned rex = 0;
    if (i < available && (p[i] & 0xf0) == 0x40) rex = p[i++];
    if (rep == (size_t)-1 || i + 3 > available || p[i] != 0x0f || p[i + 1] != opcode) return 0;
    form->opcode_at = i;
    int rip_relative;
    unsigned base;
    const size_t operand = modrm_length(p + i + 2, available - i - 2, rex & 1, &rip_relative, &base);
    if (!operand || rip_relative) return 0;
    form->length = i + 2 + operand;
    form->rep_at = rep;
    form->bits = rex & 8 ? 64 : operand16 ? 16 : 32;
    form->destination = (p[i + 2] >> 3 & 7) | (rex & 4 ? 8u : 0u);
    form->memory = p[i + 2] >> 6 != 3;
    form->rsp_base = form->memory && base == 4;
    return 1;
}

/* [66] [REX] 0F 38 F0|F1 /m, not loading rsp. */
static size_t movbe_form(const uint8_t *p, size_t available) {
    size_t i = 0;
    if (i < available && p[i] == 0x66) ++i;
    unsigned rex = 0;
    if (i < available && (p[i] & 0xf0) == 0x40) rex = p[i++];
    if (i + 4 > available || p[i] != 0x0f || p[i + 1] != 0x38 || (p[i + 2] != 0xf0 && p[i + 2] != 0xf1)) return 0;
    const uint8_t modrm = p[i + 3];
    const unsigned reg = (modrm >> 3 & 7) | (rex & 4 ? 8u : 0u);
    if (modrm >> 6 == 3 || (p[i + 2] == 0xf0 && reg == 4)) return 0;
    int rip_relative;
    unsigned base;
    const size_t operand = modrm_length(p + i + 3, available - i - 3, rex & 1, &rip_relative, &base);
    return operand ? i + 3 + operand : 0;
}

/* C4 VEX.LZ.0F38 F2 (ANDN), F7 (BEXTR), F3 /1-3 (BLSR, BLSMSK, BLSI), not writing rsp. */
static size_t bmi1_form(const uint8_t *p, size_t available) {
    if (available < 5 || p[0] != 0xc4 || (p[1] & 0x1f) != 2 || (p[2] & 7) != 0) return 0;
    const uint8_t opcode = p[3], modrm = p[4];
    const unsigned group = modrm >> 3 & 7;
    if (opcode != 0xf2 && opcode != 0xf7 && !(opcode == 0xf3 && group >= 1 && group <= 3)) return 0;
    const unsigned destination = opcode == 0xf3 ? (~p[2] >> 3 & 15) : group | (p[1] & 0x80 ? 0u : 8u);
    if (destination == 4) return 0;
    int rip_relative;
    unsigned base;
    const size_t operand = modrm_length(p + 4, available - 4, !(p[1] & 0x20), &rip_relative, &base);
    return operand ? 4 + operand : 0;
}

/* The two VEX.128 F16C forms isa_emulate_f16c carries out. */
static size_t f16c_form(const uint8_t *p, size_t available) {
    if (available < 5 || p[0] != 0xc4 || p[2] != 0x79) return 0;
    const int widen = (p[1] & 0x1f) == 2 && p[3] == 0x13, narrow = (p[1] & 0x1f) == 3 && p[3] == 0x1d;
    if (!widen && !narrow) return 0;
    int rip_relative;
    unsigned base;
    const size_t operand = modrm_length(p + 4, available - 4, !(p[1] & 0x20), &rip_relative, &base);
    if (!operand || 4 + operand + (narrow ? 1u : 0u) > available) return 0;
    return 4 + operand + (narrow ? 1u : 0u);
}

size_t cpu_compat_instruction(const uint8_t *code, size_t available, unsigned kind) {
    if (available > CPU_COMPAT_SITE_MAX) available = CPU_COMPAT_SITE_MAX;
    CountForm form;
    switch (kind) {
    case CPU_COMPAT_LZCNT: return count_form(code, available, 0xbd, &form) ? form.length : 0;
    case CPU_COMPAT_TZCNT: return count_form(code, available, 0xbc, &form) ? form.length : 0;
    case CPU_COMPAT_F16C: return f16c_form(code, available);
    case CPU_COMPAT_MOVBE: return movbe_form(code, available);
    case CPU_COMPAT_BMI1: return bmi1_form(code, available);
    default: return 0;
    }
}

static int emulated_kind(unsigned kind) { return kind == CPU_COMPAT_F16C || kind == CPU_COMPAT_MOVBE || kind == CPU_COMPAT_BMI1; }

/* Emulated parts a site runs through the shared routine when `kinds` are replaced. */
static unsigned emulated_parts(const CpuCompatSite *site, unsigned kinds) {
    unsigned n = 0;
    for (unsigned p = 0; p < CPU_COMPAT_PARTS; ++p) n += (part_kind(site->parts[p]) & kinds) && emulated_kind(part_kind(site->parts[p]));
    return n;
}

size_t cpu_compat_stub_bytes(const CpuCompatTable *table, unsigned kinds) {
    size_t sites = 0;
    int common = 0;
    for (size_t i = 0; table && i < table->count; ++i)
        if (cpu_compat_site_kinds(&table->sites[i]) & kinds) {
            ++sites;
            common |= emulated_parts(&table->sites[i], kinds) != 0;
        }
    return sites * CPU_COMPAT_STUB_STRIDE + (common ? CPU_COMPAT_COMMON_BYTES : 0);
}

typedef struct {
    uint8_t *bytes;
    size_t length, capacity;
} Code;

static void put(Code *c, const void *data, size_t length) {
    if (c->length + length <= c->capacity) memcpy(c->bytes + c->length, data, length);
    c->length += length;
}
static void put_byte(Code *c, unsigned value) {
    const uint8_t byte = (uint8_t)value;
    put(c, &byte, 1);
}
static void put_u32(Code *c, uint32_t value) { put(c, &value, 4); }
static void put_rel32(Code *c, const uint8_t *at, const uint8_t *target) {
    put_u32(c, (uint32_t)(int32_t)((intptr_t)target - (intptr_t)(at + c->length + 4)));
}
static int reaches(const uint8_t *from, const uint8_t *to) {
    const int64_t distance = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from;
    return distance <= REL32_REACH && distance >= -REL32_REACH;
}
static void put_operand_prefixes(Code *c, unsigned bits, unsigned reg, unsigned rm) {
    if (bits == 16) put_byte(c, 0x66);
    const unsigned rex = (bits == 64 ? 8u : 0u) | (reg >= 8 ? 4u : 0u) | (rm >= 8 ? 1u : 0u);
    if (rex) put_byte(c, 0x40 | rex);
}

static const uint8_t RSP_DOWN[] = {0x48, 0x8d, 0x64, 0x24, 0x80};                   /* lea rsp,[rsp-128] */
static const uint8_t RSP_UP[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};   /* lea rsp,[rsp+128] */

/* BSR or BSF with the same operands, then the result LZCNT or TZCNT gives: (width - 1) ^ index or the index, with CF
 * clear and ZF for a zero result; width with CF set for a zero source. A memory source that is not the stack runs
 * below the red zone, where a page fault's frame cannot reach the function's data. */
static void put_count(Code *c, const uint8_t *instruction, const CountForm *form, int leading) {
    const int shield = form->memory && !form->rsp_base;
    if (shield) put(c, RSP_DOWN, sizeof RSP_DOWN);
    for (size_t i = 0; i < form->length; ++i)
        if (i != form->rep_at) put_byte(c, instruction[i]);
    if (shield) put(c, RSP_UP, sizeof RSP_UP);

    const unsigned bits = form->bits, dst = form->destination;
    uint8_t nonzero_bytes[8], zero_bytes[16];
    Code nonzero = {nonzero_bytes, 0, sizeof nonzero_bytes}, zero = {zero_bytes, 0, sizeof zero_bytes};
    if (leading) {
        put_operand_prefixes(&nonzero, bits, 0, dst);
        put_byte(&nonzero, 0x83);
        put_byte(&nonzero, 0xf0 | (dst & 7));
        put_byte(&nonzero, bits - 1);
    } else {
        put_operand_prefixes(&nonzero, bits, dst, dst);
        put_byte(&nonzero, 0x85);
        put_byte(&nonzero, 0xc0 | (dst & 7) << 3 | (dst & 7));
    }
    put_operand_prefixes(&zero, bits, 0, dst);
    if (bits == 64) {
        put_byte(&zero, 0xc7);
        put_byte(&zero, 0xc0 | (dst & 7));
        put_u32(&zero, 64);
    } else {
        put_byte(&zero, 0xb8 | (dst & 7));
        if (bits == 16) { const uint16_t width = 16; put(&zero, &width, 2); }
        else put_u32(&zero, 32);
    }
    put_operand_prefixes(&zero, bits, dst, dst);
    put_byte(&zero, 0x85);
    put_byte(&zero, 0xc0 | (dst & 7) << 3 | (dst & 7));
    put_byte(&zero, 0xf9);

    put_byte(c, 0x74);
    put_byte(c, (unsigned)nonzero.length + 2);
    put(c, nonzero.bytes, nonzero.length);
    put_byte(c, 0xeb);
    put_byte(c, (unsigned)zero.length);
    put(c, zero.bytes, zero.length);
}

/* One site's stub at `stub`: its instructions in order, those of `kinds` replaced, then a jump back. `common` is the
 * shared emulation routine and `slot` the next free entry of `emulated`, advanced past the site's emulated parts. */
static int build_stub(const uint8_t *site, const CpuCompatSite *s, unsigned kinds, const uint8_t *stub,
                      const uint8_t *common, unsigned *slot, Code *c) {
    if (!reaches(site + JMP_REL32, stub) || !reaches(stub, site + s->length)) return 0;
    if (s->shield) put(c, RSP_DOWN, sizeof RSP_DOWN);
    size_t at = 0;
    for (unsigned p = 0; p < CPU_COMPAT_PARTS && s->parts[p]; ++p) {
        const unsigned kind = part_kind(s->parts[p]), length = part_length(s->parts[p]);
        const uint8_t *instruction = site + at;
        if (!length || at + length > s->length) return 0;
        at += length;
        if (!(kind & kinds)) {
            put(c, instruction, length);
        } else if (kind == CPU_COMPAT_LZCNT || kind == CPU_COMPAT_TZCNT) {
            CountForm form;
            if (!count_form(instruction, length, kind == CPU_COMPAT_LZCNT ? 0xbd : 0xbc, &form) || form.length != length ||
                (s->shield && form.rsp_base))
                return 0;
            put_count(c, instruction, &form, kind == CPU_COMPAT_LZCNT);
        } else {
            if (!common || !reaches(stub, common)) return 0;
            put(c, RSP_DOWN, sizeof RSP_DOWN);
            put_byte(c, 0x68);
            put_u32(c, (*slot)++);
            put_byte(c, 0xe8);
            put_rel32(c, stub, common);
            static const uint8_t restore[] = {0x48, 0x8d, 0xa4, 0x24, 0x88, 0x00, 0x00, 0x00}; /* lea rsp,[rsp+136] */
            put(c, restore, sizeof restore);
        }
    }
    if (at != s->length) return 0;
    if (s->shield) put(c, RSP_UP, sizeof RSP_UP);
    put_byte(c, 0xe9);
    put_rel32(c, stub, site + s->length);
    return c->length <= c->capacity;
}

/* The guest thread's registers and flags as the shared routine saved them (rax..r15 in encoding order, rsp as the
 * guest had it, rflags, then the routine's return address and the site's slot), and its 16 ymm registers. */
__attribute__((sysv_abi)) static void emulate_site(uint64_t *frame, uint8_t (*ymm)[32]) {
    const unsigned index = (unsigned)frame[18];
    const Emulated *e = index < EMULATED_MAX ? &emulated[index] : NULL;
    size_t length = 0;
    if (e) {
        length = isa_emulate(e->bytes, e->rip, frame, &frame[16], NULL);
        if (!length) length = isa_emulate_f16c(e->bytes, e->rip, frame, ymm, __builtin_ia32_stmxcsr());
    }
    if (!length) {
        fprintf(stderr, "CPU compatibility: instruction at %#llx not carried out\n", e ? (unsigned long long)e->rip : 0ull);
        abort();
    }
}

/* Saves every register, calls emulate_site with the System V convention, restores them all (as emulate_site left
 * them) and returns. Entered by a site's stub with its slot pushed, below the guest's red zone. */
static size_t build_common(uint8_t *out, size_t capacity) {
    Code c = {out, 0, capacity};
    static const uint8_t save[] = {
        0x9c, 0xfc,                                                       /* pushfq; cld */
        0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x41, 0x53, 0x41, 0x52, 0x41, 0x51, 0x41, 0x50, /* r15..r8 */
        0x57, 0x56, 0x55, 0x50, 0x53, 0x52, 0x51, 0x50,                   /* rdi rsi rbp (rsp) rbx rdx rcx rax */
        0x48, 0x8d, 0x84, 0x24, 0x18, 0x01, 0x00, 0x00,                   /* lea rax,[rsp+280]: the guest's rsp */
        0x48, 0x89, 0x44, 0x24, 0x20,                                     /* mov [rsp+32],rax */
        0x48, 0x89, 0xe3,                                                 /* mov rbx,rsp */
        0x48, 0x83, 0xe4, 0xe0,                                           /* and rsp,-32 */
        0x48, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00,                         /* sub rsp,512 */
    };
    put(&c, save, sizeof save);
    for (unsigned r = 0; r < 16; ++r) {                                   /* vmovdqu [rsp+32r],ymm_r */
        put_byte(&c, 0xc5); put_byte(&c, r < 8 ? 0xfe : 0x7e); put_byte(&c, 0x7f);
        put_byte(&c, 0x84 | (r & 7) << 3); put_byte(&c, 0x24); put_u32(&c, 32 * r);
    }
    static const uint8_t call[] = {0x48, 0x89, 0xdf, 0x48, 0x89, 0xe6, 0x48, 0xb8}; /* mov rdi,rbx; mov rsi,rsp; mov rax, */
    put(&c, call, sizeof call);
    const uint64_t helper = (uint64_t)(uintptr_t)&emulate_site;
    put(&c, &helper, 8);
    put_byte(&c, 0xff); put_byte(&c, 0xd0);                               /* call rax */
    for (unsigned r = 0; r < 16; ++r) {                                   /* vmovdqu ymm_r,[rsp+32r] */
        put_byte(&c, 0xc5); put_byte(&c, r < 8 ? 0xfe : 0x7e); put_byte(&c, 0x6f);
        put_byte(&c, 0x84 | (r & 7) << 3); put_byte(&c, 0x24); put_u32(&c, 32 * r);
    }
    static const uint8_t restore[] = {
        0x48, 0x89, 0xdc,                                                 /* mov rsp,rbx */
        0x58, 0x59, 0x5a, 0x5b, 0x48, 0x8d, 0x64, 0x24, 0x08, 0x5d, 0x5e, 0x5f, /* rax rcx rdx rbx, skip rsp, rbp rsi rdi */
        0x41, 0x58, 0x41, 0x59, 0x41, 0x5a, 0x41, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, /* r8..r15 */
        0x9d, 0xc3,                                                       /* popfq; ret */
    };
    put(&c, restore, sizeof restore);
    return c.length <= capacity ? c.length : 0;
}

static int sorted_and_disjoint(const CpuCompatTable *table) {
    for (size_t i = 1; i < table->count; ++i)
        if ((uint64_t)table->sites[i - 1].offset + table->sites[i - 1].length > table->sites[i].offset) return 0;
    return 1;
}

CpuCompatStatus cpu_compat_check(const uint8_t *image, uint64_t size, const CpuCompatTable *table) {
    if (!table || table->size != size) return CPU_COMPAT_UNKNOWN_IMAGE;
    if (!sorted_and_disjoint(table)) return CPU_COMPAT_BAD_SITE;
    for (size_t i = 0; i < table->count; ++i) {
        const CpuCompatSite *s = &table->sites[i];
        if (s->length < JMP_REL32 || s->length > CPU_COMPAT_SITE_MAX || (uint64_t)s->offset + s->length > size ||
            !cpu_compat_site_kinds(s) || s->shield > 1)
            return CPU_COMPAT_BAD_SITE;
        size_t at = 0;
        unsigned p = 0;
        for (; p < CPU_COMPAT_PARTS && s->parts[p]; ++p) {
            const unsigned kind = part_kind(s->parts[p]), length = part_length(s->parts[p]);
            if (!length || (s->parts[p] >> 4) > 5 || at + length > s->length ||
                (kind && cpu_compat_instruction(image + s->offset + at, length, kind) != length))
                return CPU_COMPAT_BAD_SITE;
            at += length;
        }
        for (unsigned rest = p; rest < CPU_COMPAT_PARTS; ++rest)
            if (s->parts[rest]) return CPU_COMPAT_BAD_SITE;
        if (at != s->length) return CPU_COMPAT_BAD_SITE;
    }
    return cpu_compat_digest(image, size, table->sites, table->count) == table->digest ? CPU_COMPAT_OK : CPU_COMPAT_BAD_SITE;
}

CpuCompatStatus cpu_compat_apply(uint8_t *image, uint64_t size, const CpuCompatTable *table, unsigned kinds,
                                 uint8_t *stubs, size_t stubs_size, size_t *replaced) {
    if (replaced) *replaced = 0;
    const CpuCompatStatus status = cpu_compat_check(image, size, table);
    if (status != CPU_COMPAT_OK) return status;
    const size_t needed = cpu_compat_stub_bytes(table, kinds);
    if (!needed) return CPU_COMPAT_OK;
    if (needed > stubs_size || !reaches(image, stubs + stubs_size) || !reaches(image + size, stubs))
        return CPU_COMPAT_NO_ROOM;

    size_t emulated_sites = 0;
    for (size_t i = 0; i < table->count; ++i) emulated_sites += emulated_parts(&table->sites[i], kinds);
    const unsigned first = emulated_count;
    if (emulated_sites > EMULATED_MAX - first) return CPU_COMPAT_NO_ROOM;
    uint8_t *common = NULL, *next = stubs;
    if (emulated_sites) {
        uint8_t scratch[CPU_COMPAT_COMMON_BYTES];
        if (!build_common(scratch, sizeof scratch)) return CPU_COMPAT_NO_ROOM;
        common = stubs;
        next = stubs + CPU_COMPAT_COMMON_BYTES;
    }
    /* Every stub is built once into scratch space before anything is written. */
    uint8_t *place = next;
    unsigned slot = first;
    for (size_t i = 0; i < table->count; ++i) {
        const CpuCompatSite *s = &table->sites[i];
        if (!(cpu_compat_site_kinds(s) & kinds)) continue;
        uint8_t scratch[CPU_COMPAT_STUB_STRIDE];
        Code code = {scratch, 0, sizeof scratch};
        if (!build_stub(image + s->offset, s, kinds, place, common, &slot, &code)) return CPU_COMPAT_BAD_SITE;
        place += CPU_COMPAT_STUB_STRIDE;
    }

    if (common) {
        memset(common, 0xcc, CPU_COMPAT_COMMON_BYTES);
        build_common(common, CPU_COMPAT_COMMON_BYTES);
    }
    place = next;
    slot = first;
    size_t count = 0;
    for (size_t i = 0; i < table->count; ++i) {
        const CpuCompatSite *s = &table->sites[i];
        const unsigned site_kinds = cpu_compat_site_kinds(s) & kinds;
        if (!site_kinds) continue;
        uint8_t *site = image + s->offset;
        unsigned record = slot;
        for (unsigned p = 0, at = 0; p < CPU_COMPAT_PARTS && s->parts[p]; at += part_length(s->parts[p++])) {
            const unsigned kind = part_kind(s->parts[p]);
            if (!(kind & kinds) || !emulated_kind(kind)) continue;
            memset(emulated[record].bytes, 0, sizeof emulated[record].bytes);
            memcpy(emulated[record].bytes, site + at, part_length(s->parts[p]));
            emulated[record].rip = (uint64_t)(uintptr_t)(site + at);
            ++record;
        }
        memset(place, 0xcc, CPU_COMPAT_STUB_STRIDE);
        Code code = {place, 0, CPU_COMPAT_STUB_STRIDE};
        build_stub(site, s, kinds, place, common, &slot, &code);
        site[0] = 0xe9;
        const int32_t rel = (int32_t)((intptr_t)place - (intptr_t)(site + JMP_REL32));
        memcpy(site + 1, &rel, 4);
        memset(site + JMP_REL32, 0xcc, s->length - JMP_REL32);
        place += CPU_COMPAT_STUB_STRIDE;
        for (unsigned p = 0; p < CPU_COMPAT_PARTS; ++p) count += (part_kind(s->parts[p]) & kinds) != 0;
    }
    emulated_count = slot;
    if (replaced) *replaced = count;
    return CPU_COMPAT_OK;
}

int cpu_compat_processor_usable(IsaHostFeatures host, char *cause, size_t cause_size, char *advice, size_t advice_size) {
    char missing[96];
    if (!isa_missing_sets(host, 0, missing, sizeof missing)) return 1;
    snprintf(cause, cause_size, "This processor lacks %s, which the game's PS4 code needs.", missing);
    snprintf(advice, advice_size, "The game needs a processor with AVX: Intel Core 2nd generation (Sandy Bridge), AMD FX "
             "or newer.");
    return 0;
}

enum { ESSENTIAL = CPU_COMPAT_LZCNT | CPU_COMPAT_TZCNT | CPU_COMPAT_F16C };

CpuCompatPlan cpu_compat_plan(CpuCompatScope scope, const char *name, IsaHostFeatures host, unsigned forced) {
    CpuCompatPlan plan = {cpu_compat_needed(host) | forced, 0, 0, forced, NULL, 0};
    if (!plan.kinds) return plan;
    plan.table = named(name);
    if (scope == CPU_COMPAT_BOOT_IMAGE) {
        plan.wanted = plan.kinds;
    } else {
        for (size_t i = 0; plan.table && i < plan.table->count; ++i)
            plan.wanted |= cpu_compat_site_kinds(&plan.table->sites[i]) & plan.kinds;
    }
    plan.essential = plan.wanted & ESSENTIAL;
    plan.stub_bytes = cpu_compat_stub_bytes(plan.table, plan.kinds);
    return plan;
}

CpuCompatOutcome cpu_compat_install(const CpuCompatPlan *plan, uint8_t *image, uint64_t size, uint8_t *stubs,
                                    size_t stubs_size) {
    CpuCompatOutcome out = {CPU_COMPAT_OK, 0, 0, ""};
    if (!plan->wanted) return out;
    const char *name = plan->table ? plan->table->name : "this image";
    out.status = plan->table ? cpu_compat_apply(image, size, plan->table, plan->kinds, stubs, stubs_size, &out.replaced)
                             : CPU_COMPAT_UNKNOWN_IMAGE;
    char names[64];
    if (out.status != CPU_COMPAT_OK) {
        out.fatal = plan->essential != 0;
        cpu_compat_kind_names(plan->essential ? plan->essential : plan->wanted, names, sizeof names);
        snprintf(out.message, sizeof out.message, "%s cannot be adapted for %s: %s%s", name, names,
                 cpu_compat_status_text(out.status), out.fatal ? "" : "; those instructions are emulated as they run");
        return out;
    }
    if (!out.replaced) return out;
    unsigned replaced_kinds = 0;
    for (size_t i = 0; i < plan->table->count; ++i) replaced_kinds |= cpu_compat_site_kinds(&plan->table->sites[i]) & plan->kinds;
    cpu_compat_kind_names(replaced_kinds, names, sizeof names);
    snprintf(out.message, sizeof out.message, "%zu instructions (%s) replaced at load for %s%s", out.replaced, names, name,
             plan->forced ? ", forced" : "");
    return out;
}

void cpu_compat_kind_names(unsigned kinds, char *out, size_t size) {
    static const char *const names[] = {"LZCNT", "TZCNT", "F16C", "MOVBE", "BMI1"};
    size_t used = 0;
    if (size) out[0] = 0;
    for (unsigned k = 0; k < 5 && used < size; ++k) {
        if (!(kinds & (1u << k))) continue;
        const int n = snprintf(out + used, size - used, "%s%s", used ? ", " : "", names[k]);
        used = n < 0 || (size_t)n >= size - used ? size : used + (size_t)n;
    }
}
