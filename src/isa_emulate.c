#include "isa_emulate.h"
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

enum { MAX_INSTRUCTION = 15 };
enum { FLAG_CF = 1u << 0, FLAG_ZF = 1u << 6, FLAG_SF = 1u << 7, FLAG_OF = 1u << 11 };

typedef struct {
    const uint8_t *code;
    size_t length;
} Cursor;

typedef struct {
    int is_register;
    unsigned reg;
    uint64_t address;
} Operand;

static int take(Cursor *c, uint8_t *out) {
    if (c->length >= MAX_INSTRUCTION) return 0;
    *out = c->code[c->length++];
    return 1;
}

static int take_signed(Cursor *c, unsigned bytes, int64_t *out) {
    if (c->length + bytes > MAX_INSTRUCTION) return 0;
    if (bytes == 1) *out = (int8_t)c->code[c->length];
    else {
        int32_t v;
        memcpy(&v, c->code + c->length, 4);
        *out = v;
    }
    c->length += bytes;
    return 1;
}

/* The r/m operand of modrm with 64-bit addressing; `trailing` immediate bytes follow it. */
static int decode_rm_before(Cursor *c, uint8_t modrm, int rex_x, int rex_b, uint64_t rip, const uint64_t gpr[16],
                            unsigned trailing, Operand *out) {
    const unsigned mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) {
        *out = (Operand){1, rm | (rex_b ? 8 : 0), 0};
        return 1;
    }
    int has_base = 1, rip_relative = 0, has_index = 0;
    unsigned base = rm | (rex_b ? 8 : 0), index = 0, scale = 1;
    if (rm == 4) {
        uint8_t sib;
        if (!take(c, &sib)) return 0;
        scale = 1u << (sib >> 6);
        index = (sib >> 3 & 7) | (rex_x ? 8 : 0);
        has_index = index != 4;
        base = (sib & 7) | (rex_b ? 8 : 0);
        if ((sib & 7) == 5 && mod == 0) has_base = 0;
    } else if (rm == 5 && mod == 0) {
        has_base = 0;
        rip_relative = 1;
    }
    int64_t displacement = 0;
    if (mod == 1) { if (!take_signed(c, 1, &displacement)) return 0; }
    else if (mod == 2 || !has_base) { if (!take_signed(c, 4, &displacement)) return 0; }

    uint64_t address = (uint64_t)displacement;
    if (rip_relative) address += rip + c->length + trailing;
    if (has_base) address += gpr[base];
    if (has_index) address += gpr[index] * scale;
    *out = (Operand){0, 0, address};
    return 1;
}

static int decode_rm(Cursor *c, uint8_t modrm, int rex_x, int rex_b, uint64_t rip, const uint64_t gpr[16], Operand *out) {
    return decode_rm_before(c, modrm, rex_x, rex_b, rip, gpr, 0, out);
}

static uint64_t swap(uint64_t value, unsigned bits) {
    if (bits == 16) return __builtin_bswap16((uint16_t)value);
    if (bits == 32) return __builtin_bswap32((uint32_t)value);
    return __builtin_bswap64(value);
}

size_t isa_emulate_movbe(const uint8_t *code, uint64_t rip, uint64_t gpr[16]) {
    Cursor c = {code, 0};
    uint8_t byte;
    int operand16 = 0;
    if (!take(&c, &byte)) return 0;
    if (byte == 0x66) {
        operand16 = 1;
        if (!take(&c, &byte)) return 0;
    }
    unsigned rex = 0;
    if ((byte & 0xf0) == 0x40) {
        rex = byte;
        if (!take(&c, &byte)) return 0;
    }
    if (byte != 0x0f || !take(&c, &byte) || byte != 0x38 || !take(&c, &byte)) return 0;
    const int store = byte == 0xf1;
    if (!store && byte != 0xf0) return 0;
    uint8_t modrm;
    Operand rm;
    if (!take(&c, &modrm) || modrm >> 6 == 3 || !decode_rm(&c, modrm, rex & 2, rex & 1, rip, gpr, &rm)) return 0;

    const unsigned reg = (modrm >> 3 & 7) | (rex & 4 ? 8 : 0);
    const unsigned bits = rex & 8 ? 64 : operand16 ? 16 : 32;
    uint8_t *memory = (uint8_t *)(uintptr_t)rm.address;

    if (store) {
        const uint64_t value = swap(gpr[reg], bits);
        memcpy(memory, &value, bits / 8);
    } else {
        uint64_t value = 0;
        memcpy(&value, memory, bits / 8);
        value = swap(value, bits);
        gpr[reg] = bits == 16 ? (gpr[reg] & ~0xffffull) | value : value;
    }
    return c.length;
}

static size_t emulate_bmi1(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint64_t *rflags) {
    Cursor c = {code, 0};
    uint8_t vex, p1, p2, opcode, modrm;
    if (!take(&c, &vex) || vex != 0xc4 || !take(&c, &p1) || !take(&c, &p2) || !take(&c, &opcode) || !take(&c, &modrm))
        return 0;
    /* VEX.LZ.0F38 without an implied prefix; the other pp values of these opcodes are BMI2. */
    if ((p1 & 0x1f) != 2 || (p2 & 7) != 0) return 0;
    const unsigned group = modrm >> 3 & 7;
    if (opcode != 0xf2 && opcode != 0xf7 && !(opcode == 0xf3 && group >= 1 && group <= 3)) return 0;
    Operand rm;
    if (!decode_rm(&c, modrm, !(p1 & 0x40), !(p1 & 0x20), rip, gpr, &rm)) return 0;

    const unsigned bits = p2 & 0x80 ? 64 : 32;
    const uint64_t mask = bits == 64 ? ~0ull : 0xffffffffull;
    const unsigned reg = group | (p1 & 0x80 ? 0 : 8), vvvv = ~p2 >> 3 & 15;
    uint64_t source = 0;
    if (rm.is_register) source = gpr[rm.reg] & mask;
    else memcpy(&source, (const void *)(uintptr_t)rm.address, bits / 8);
    const uint64_t other = gpr[vvvv] & mask;

    uint64_t result;
    unsigned destination = reg;
    int carry = 0;
    if (opcode == 0xf2) {
        result = ~other & source;
    } else if (opcode == 0xf7) {
        const unsigned start = other & 0xff, length = other >> 8 & 0xff;
        result = start < bits ? source >> start : 0;
        if (length < bits) result &= (1ull << length) - 1;
    } else {
        destination = vvvv;
        result = group == 1 ? source & (source - 1) : group == 2 ? source ^ (source - 1) : source & -source;
        carry = group == 3 ? source != 0 : source == 0;
    }
    result &= mask;

    uint64_t flags = *rflags & ~(uint64_t)(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    if (carry) flags |= FLAG_CF;
    if (!result) flags |= FLAG_ZF;
    if (result >> (bits - 1)) flags |= FLAG_SF;
    gpr[destination] = result;
    *rflags = flags;
    return c.length;
}

typedef struct {
    uint8_t prefix, opcode, modrm;
    unsigned rex;
} Sse4aHead;

/* F2, F3 or 66, an optional REX, 0F, opcode 78, 79 or 2B, modrm. */
static int parse_sse4a(Cursor *c, Sse4aHead *h) {
    uint8_t byte;
    if (!take(c, &h->prefix) || (h->prefix != 0x66 && h->prefix != 0xf2 && h->prefix != 0xf3) || !take(c, &byte)) return 0;
    h->rex = 0;
    if ((byte & 0xf0) == 0x40) {
        h->rex = byte;
        if (!take(c, &byte)) return 0;
    }
    return byte == 0x0f && take(c, &h->opcode) && take(c, &h->modrm) &&
           (h->opcode == 0x78 || h->opcode == 0x79 || h->opcode == 0x2b);
}

static uint64_t load64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static uint64_t field_mask(unsigned length) { return length == 64 ? ~0ull : (1ull << length) - 1; }
/* A length field of 0 means 64. */
static unsigned field_length(unsigned value) { return value & 63 ? value & 63 : 64; }

static size_t emulate_sse4a(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint8_t xmm[16][16]) {
    Cursor c = {code, 0};
    Sse4aHead h;
    if (!xmm || !parse_sse4a(&c, &h)) return 0;
    const unsigned reg = (h.modrm >> 3 & 7) | (h.rex & 4 ? 8 : 0), rm = (h.modrm & 7) | (h.rex & 1 ? 8 : 0);
    if (h.opcode == 0x2b) {
        Operand memory;
        if (h.prefix == 0x66 || h.modrm >> 6 == 3 || !decode_rm(&c, h.modrm, h.rex & 2, h.rex & 1, rip, gpr, &memory))
            return 0;
        memcpy((void *)(uintptr_t)memory.address, xmm[reg], h.prefix == 0xf2 ? 8 : 4);
        return c.length;
    }
    if (h.prefix == 0xf3 || h.modrm >> 6 != 3) return 0;
    const int insert = h.prefix == 0xf2;
    unsigned destination = reg, length, index;
    if (h.opcode == 0x78) {
        uint8_t first, second;
        if ((!insert && (h.modrm >> 3 & 7)) || !take(&c, &first) || !take(&c, &second)) return 0;
        length = field_length(first);
        index = second & 63;
        if (!insert) destination = rm;
    } else {
        const uint64_t control = load64(xmm[rm] + (insert ? 8 : 0));
        length = field_length((unsigned)control);
        index = (unsigned)(control >> 8) & 63;
    }
    const uint64_t mask = field_mask(length) << index, old = load64(xmm[destination]);
    const uint64_t value = insert ? (old & ~mask) | ((load64(xmm[rm]) << index) & mask) : (old >> index) & field_mask(length);
    memcpy(xmm[destination], &value, 8);
    return c.length;
}

size_t isa_emulate(const uint8_t *code, uint64_t rip, uint64_t gpr[16], uint64_t *rflags, uint8_t xmm[16][16]) {
    size_t length = isa_emulate_movbe(code, rip, gpr);
    if (!length) length = emulate_bmi1(code, rip, gpr, rflags);
    return length ? length : emulate_sse4a(code, rip, gpr, xmm);
}

uint32_t isa_half_to_float(uint16_t half) {
    const uint32_t sign = (uint32_t)(half & 0x8000) << 16, mantissa = half & 0x3ff;
    int exponent = half >> 10 & 0x1f;
    if (exponent == 0x1f) return sign | 0x7f800000u | mantissa << 13 | (mantissa ? 0x400000u : 0);
    if (!exponent) {
        if (!mantissa) return sign;
        uint32_t m = mantissa;
        exponent = 1;
        while (!(m & 0x400)) { m <<= 1; --exponent; }
        return sign | (uint32_t)(exponent + 112) << 23 | (m & 0x3ff) << 13;
    }
    return sign | (uint32_t)(exponent + 112) << 23 | mantissa << 13;
}

uint16_t isa_float_to_half(uint32_t single, unsigned rounding, int denormals_are_zero) {
    const uint16_t sign = (uint16_t)(single >> 16 & 0x8000);
    const unsigned biased = single >> 23 & 0xff;
    uint32_t mantissa = single & 0x7fffff;
    if (biased == 0xff) return mantissa ? (uint16_t)(sign | 0x7e00 | mantissa >> 13) : (uint16_t)(sign | 0x7c00);
    if (!biased && denormals_are_zero) mantissa = 0;
    if (!biased && !mantissa) return sign;
    const int negative = sign != 0;
    int exponent = biased ? (int)biased - 127 : -126;
    uint64_t m = mantissa | (biased ? 0x800000u : 0);
    while (!(m & 0x800000)) { m <<= 1; --exponent; }
    if (exponent > 15) {
        const int infinite = rounding == 0 || (rounding == 2 && !negative) || (rounding == 1 && negative);
        return (uint16_t)(sign | (infinite ? 0x7c00 : 0x7bff));
    }
    /* Past 30 bits every one of the 24 is dropped and the result no longer depends on the shift. */
    unsigned shift = exponent >= -14 ? 13u : 13u + (unsigned)(-14 - exponent);
    if (shift > 30) shift = 30;
    const uint64_t kept = m >> shift, dropped = m & ((UINT64_C(1) << shift) - 1), halfway = UINT64_C(1) << (shift - 1);
    int up = 0;
    if (rounding == 0) up = dropped > halfway || (dropped == halfway && (kept & 1));
    else if (rounding == 1) up = negative && dropped;
    else if (rounding == 2) up = !negative && dropped;
    const uint64_t value = kept + (uint64_t)up;
    const uint64_t bits = exponent >= -14 ? ((uint64_t)(exponent + 15) << 10) + value - 0x400 : value;
    return (uint16_t)(sign | bits);
}

/* C4 [RXB 00010] [0 1111 0 01] 13 /r: VCVTPH2PS xmm, xmm/m64; C4 [RXB 00011] [0 1111 0 01] 1D /r ib: VCVTPS2PH
 * xmm/m64, xmm, imm8. */
size_t isa_emulate_f16c(const uint8_t *code, uint64_t rip, const uint64_t gpr[16], uint8_t ymm[16][32], uint32_t mxcsr) {
    Cursor c = {code, 0};
    uint8_t vex, p1, p2, opcode, modrm;
    if (!ymm || !take(&c, &vex) || vex != 0xc4 || !take(&c, &p1) || !take(&c, &p2) || !take(&c, &opcode) ||
        !take(&c, &modrm) || p2 != 0x79)
        return 0;
    const int widen = (p1 & 0x1f) == 2 && opcode == 0x13, narrow = (p1 & 0x1f) == 3 && opcode == 0x1d;
    if (!widen && !narrow) return 0;
    const unsigned reg = (modrm >> 3 & 7) | (p1 & 0x80 ? 0 : 8);
    Operand rm;
    if (!decode_rm_before(&c, modrm, !(p1 & 0x40), !(p1 & 0x20), rip, gpr, narrow ? 1u : 0u, &rm)) return 0;
    uint8_t halves[8], floats[16];
    if (widen) {
        memcpy(halves, rm.is_register ? ymm[rm.reg] : (const uint8_t *)(uintptr_t)rm.address, sizeof halves);
        for (unsigned i = 0; i < 4; ++i) {
            uint16_t h;
            memcpy(&h, halves + 2 * i, 2);
            const uint32_t f = isa_half_to_float(h);
            memcpy(floats + 4 * i, &f, 4);
        }
        memset(ymm[reg], 0, 32);
        memcpy(ymm[reg], floats, sizeof floats);
        return c.length;
    }
    uint8_t control;
    if (!take(&c, &control)) return 0;
    const unsigned rounding = control & 4 ? mxcsr >> 13 & 3 : control & 3;
    memcpy(floats, ymm[reg], sizeof floats);
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t f;
        memcpy(&f, floats + 4 * i, 4);
        const uint16_t h = isa_float_to_half(f, rounding, (mxcsr & 0x40) != 0);
        memcpy(halves + 2 * i, &h, 2);
    }
    if (rm.is_register) {
        memset(ymm[rm.reg], 0, 32);
        memcpy(ymm[rm.reg], halves, sizeof halves);
    } else {
        memcpy((void *)(uintptr_t)rm.address, halves, sizeof halves);
    }
    return c.length;
}

/* The masks, 16-byte aligned, come first, jumped over; then
 *   lea rsp,[rsp-32]; vmovdqu [rsp],ymm_t; movdqa t,src; psllq t,index; pand t,take; pand dst,keep; por dst,t;
 *   vmovdqu ymm_t,[rsp]; lea rsp,[rsp+32]
 * with t one of xmm15, xmm14 and xmm13 that is neither operand. Legacy SSE encodings keep the destination's upper
 * ymm half, as INSERTQ does. */
size_t isa_insertq_rewrite(const uint8_t *code, uint8_t *out) {
    Cursor c = {code, 0};
    Sse4aHead h;
    uint8_t first, second;
    if (!parse_sse4a(&c, &h) || h.prefix != 0xf2 || h.opcode != 0x78 || h.modrm >> 6 != 3 || !take(&c, &first) ||
        !take(&c, &second))
        return 0;
    const unsigned destination = (h.modrm >> 3 & 7) | (h.rex & 4 ? 8 : 0), source = (h.modrm & 7) | (h.rex & 1 ? 8 : 0);
    const unsigned length = field_length(first), index = second & 63;
    if (length + index > 64) return 0;
    unsigned t = 15;
    while (t == destination || t == source) --t;
    const uint64_t mask = field_mask(length) << index;
    const uint64_t masks[4] = {mask, 0, ~mask, ~0ull};

    const size_t pool = (((uintptr_t)out + 2 + 15) & ~(uintptr_t)15) - (uintptr_t)out;
    size_t n = 0;
    out[n++] = 0xeb;
    out[n++] = (uint8_t)(pool - 2 + 32);
    while (n < pool) out[n++] = 0xcc;
    memcpy(out + n, masks, sizeof masks);
    n += sizeof masks;

#define EMIT(...) do { const uint8_t bytes_[] = {__VA_ARGS__}; memcpy(out + n, bytes_, sizeof bytes_); n += sizeof bytes_; } while (0)
#define REX(reg, rm) do { const unsigned rex_ = ((reg) >> 3 << 2) | ((rm) >> 3); if (rex_) EMIT((uint8_t)(0x40 | rex_)); } while (0)
    /* A legacy SSE instruction on two registers. */
#define SSE_REGISTERS(opcode, reg, rm) do { EMIT(0x66); REX(reg, rm); EMIT(0x0f, opcode, (uint8_t)(0xc0 | ((reg) & 7) << 3 | ((rm) & 7))); } while (0)
    /* pand reg,[rip+...] on the mask at `at` bytes into the pool. */
#define PAND_MASK(reg, at) do { EMIT(0x66); REX(reg, 0); EMIT(0x0f, 0xdb, (uint8_t)(((reg) & 7) << 3 | 5)); \
        const int32_t disp_ = (int32_t)((int64_t)(pool + (at)) - (int64_t)(n + 4)); memcpy(out + n, &disp_, 4); n += 4; } while (0)
    EMIT(0x48, 0x8d, 0x64, 0x24, 0xe0);
    EMIT(0xc4, 0x61, 0x7e, 0x7f, (uint8_t)((t & 7) << 3 | 4), 0x24);
    SSE_REGISTERS(0x6f, t, source);
    if (index) { EMIT(0x66); REX(0, t); EMIT(0x0f, 0x73, (uint8_t)(0xf0 | (t & 7)), (uint8_t)index); }
    PAND_MASK(t, 0);
    PAND_MASK(destination, 16);
    SSE_REGISTERS(0xeb, destination, t);
    EMIT(0xc4, 0x61, 0x7e, 0x6f, (uint8_t)((t & 7) << 3 | 4), 0x24);
    EMIT(0x48, 0x8d, 0x64, 0x24, 0x20);
#undef EMIT
#undef REX
#undef SSE_REGISTERS
#undef PAND_MASK
    return n;
}

IsaHostFeatures isa_host_features(void) {
    IsaHostFeatures host = {0, 0, 0};
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        host.leaf1_ecx = c;
        /* AVX and F16C count only when the OS saves the YMM state (OSXSAVE and XCR0 bits 1-2). */
        unsigned xcr0 = 0, high = 0;
        if (c & bit_OSXSAVE) __asm__ volatile("xgetbv" : "=a"(xcr0), "=d"(high) : "c"(0));
        if ((xcr0 & 6) != 6) host.leaf1_ecx &= ~(uint32_t)(bit_AVX | bit_F16C);
    }
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) host.leaf7_ebx = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) host.ext1_ecx = c;
    return host;
}

unsigned isa_emulation_needed(IsaHostFeatures host) {
    return (host.leaf1_ecx & bit_MOVBE ? 0u : ISA_EMULATE_MOVBE) | (host.leaf7_ebx & bit_BMI ? 0u : ISA_EMULATE_BMI1) |
           (host.ext1_ecx & bit_SSE4a ? 0u : ISA_EMULATE_SSE4A);
}

/* The game's SSE4.1 and SSE4.2 instructions are all VEX encoded, which AVX covers. */
static const struct { const char *name; int handled, word; uint32_t bit; } isa_sets[] = {
    {"AVX", 0, 0, bit_AVX},     {"POPCNT", 0, 0, bit_POPCNT}, {"CMPXCHG16B", 0, 0, bit_CMPXCHG16B},
    {"MOVBE", 1, 0, bit_MOVBE}, {"BMI1", 1, 1, bit_BMI},      {"LZCNT", 1, 2, bit_LZCNT},
    {"F16C", 1, 0, bit_F16C},   {"SSE4a", 1, 2, bit_SSE4a},
};

static int list_sets(IsaHostFeatures host, int present, int handled, char *out, size_t size) {
    const uint32_t words[] = {host.leaf1_ecx, host.leaf7_ebx, host.ext1_ecx};
    int listed = 0;
    size_t used = 0;
    if (size) out[0] = 0;
    for (size_t i = 0; i < sizeof isa_sets / sizeof *isa_sets; ++i) {
        const int has = (words[isa_sets[i].word] & isa_sets[i].bit) != 0;
        if (has != present || (!present && isa_sets[i].handled != handled)) continue;
        if (used < size) {
            const int n = snprintf(out + used, size - used, "%s%s", listed ? ", " : "", isa_sets[i].name);
            used = n < 0 || (size_t)n >= size - used ? size : used + (size_t)n;
        }
        ++listed;
    }
    return listed;
}

int isa_missing_sets(IsaHostFeatures host, int handled, char *out, size_t size) {
    return list_sets(host, 0, !!handled, out, size);
}

int isa_present_sets(IsaHostFeatures host, char *out, size_t size) { return list_sets(host, 1, 0, out, size); }

void isa_host_name(char *out, size_t size) {
    unsigned regs[12] = {0}, high = __get_cpuid_max(0x80000000, NULL);
    for (unsigned i = 0; high >= 0x80000004 && i < 3; ++i)
        __get_cpuid(0x80000002 + i, &regs[4 * i], &regs[4 * i + 1], &regs[4 * i + 2], &regs[4 * i + 3]);
    char brand[49] = {0};
    memcpy(brand, regs, 48);
    const char *name = brand;
    while (*name == ' ') ++name;
    snprintf(out, size, "%s", *name ? name : "unknown processor");
}
