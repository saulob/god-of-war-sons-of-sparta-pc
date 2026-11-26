/* Generates src/cpu_compat_sites.inc from real game code: the boot images (SOSBOOT1) of the supported builds and the
 * modules the game loads at run time (NAME.code, NAME.fn and NAME.size from tools\export_code_ranges.py).
 *
 * Every LZCNT, TZCNT and F16C instruction of a decoded function becomes a site, and so does every MOVBE and BMI1
 * instruction of a module function that keeps data below rsp (a module gets no red-zone pass, and a #UD frame would land
 * on that data). A site shorter than a jmp rel32 takes in neighbouring instructions that are register-only, never a
 * branch target and position independent. Branch targets are every direct branch's and call's, plus every entry of a
 * 32-bit jump table found from a rip-relative lea in the function; a function with an indirect jump and no such table
 * gets no merged site. A site that cannot be built is an error: no table is written.
 *
 *   cpu_compat_gen --boot KEY boot.bin ... --module NAME DIR ... (--out FILE | --check) */
#include <Zydis/Zydis.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_compat.h"
#include "redzone_analysis.h"

typedef struct {
    char name[64];
    uint8_t *code;
    uint64_t size, code_end;
    RzaFunction *functions;
    size_t function_count;
    uint8_t *relocated; /* bytes some relocation writes, or NULL */
    int module;
} Unit;

typedef struct { CpuCompatSite *items; size_t count, cap; } Sites;

static ZydisDecoder decoder;
static unsigned errors;

static uint8_t *read_file(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
    rewind(f);
    uint8_t *data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data) { data[n] = 0; *size = (uint64_t)n; }
    return data;
}

static uint64_t u64_at(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* The image, its executable segments and relocation targets from a SOSBOOT1 file (src/loader.c reads the same). */
static int load_boot(const char *key, const char *path, Unit *u) {
    uint64_t file_size = 0;
    uint8_t *file = read_file(path, &file_size);
    if (!file || file_size < 8 * 14 || memcmp(file, "SOSBOOT1", 8)) { fprintf(stderr, "%s: not a boot image\n", path); return 0; }
    const uint8_t *p = file + 8;
    const uint64_t size = u64_at(p), ns = u64_at(p + 16), nr = u64_at(p + 24), imports = u64_at(p + 32);
    p += 8 * 6 + 8 + 8 * 4; /* size .. capabilities, procparam, main TLS */
    const uint64_t modules = u64_at(p);
    p += 8 + modules * 7 * 8;
    const uint64_t bindings = u64_at(p);
    p += 8 + bindings * 3 * 8;
    const uint8_t *segments = p;
    p += ns * 3 * 8 + imports * 128;
    const uint8_t *relocs = p;
    p += nr * 32;
    if ((uint64_t)(p - file) + size != file_size) { fprintf(stderr, "%s: unexpected layout\n", path); return 0; }
    snprintf(u->name, sizeof u->name, "%s", key);
    u->code = malloc((size_t)size);
    memcpy(u->code, p, (size_t)size);
    u->size = size;
    uint64_t *triples = malloc((size_t)ns * 3 * sizeof *triples);
    for (uint64_t i = 0; i < ns * 3; ++i) triples[i] = u64_at(segments + 8 * i);
    RzaImage analysis;
    if (!rza_enumerate(u->code, size, triples, ns, &analysis)) { fprintf(stderr, "%s: no function tables\n", path); return 0; }
    u->functions = analysis.functions;
    u->function_count = analysis.count;
    u->code_end = analysis.code_end;
    free(analysis.starts);
    free(analysis.targets);
    u->relocated = calloc((size_t)size, 1);
    for (uint64_t i = 0; i < nr; ++i) {
        const uint64_t target = u64_at(relocs + 32 * i);
        for (uint64_t k = 0; k < 8 && target + k < size; ++k) u->relocated[target + k] = 1;
    }
    free(triples);
    free(file);
    return 1;
}

static int load_module(const char *name, const char *directory, Unit *u) {
    char path[1024];
    uint64_t fn_size = 0, size_text_size = 0;
    snprintf(path, sizeof path, "%s/%s.code", directory, name);
    u->code = read_file(path, &u->code_end);
    snprintf(path, sizeof path, "%s/%s.fn", directory, name);
    char *fn = (char *)read_file(path, &fn_size);
    snprintf(path, sizeof path, "%s/%s.size", directory, name);
    char *size_text = (char *)read_file(path, &size_text_size);
    if (!u->code || !fn || !size_text) { fprintf(stderr, "%s: not exported in %s\n", name, directory); return 0; }
    snprintf(u->name, sizeof u->name, "%s", name);
    u->size = strtoull(size_text, NULL, 10);
    u->module = 1;
    size_t cap = 1024;
    u->functions = malloc(cap * sizeof *u->functions);
    for (char *line = fn; *line;) {
        char *end;
        const uint64_t begin = strtoull(line, &end, 16), finish = strtoull(end, &line, 16);
        while (*line == '\r' || *line == '\n') ++line;
        if (finish > u->code_end || begin >= finish) continue;
        if (u->function_count == cap) u->functions = realloc(u->functions, (cap *= 2) * sizeof *u->functions);
        u->functions[u->function_count++] = (RzaFunction){(uint32_t)begin, (uint32_t)finish, 0};
    }
    free(fn);
    free(size_text);
    return 1;
}

static int decode(const Unit *u, uint64_t at, uint64_t end, ZydisDecodedInstruction *in, ZydisDecodedOperand *ops) {
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, u->code + at, (ZyanUSize)(end - at), in, ops));
}

static void mark(uint8_t *bits, uint64_t at, uint64_t end) {
    if (at < end) bits[at >> 3] |= (uint8_t)(1u << (at & 7));
}
static int marked(const uint8_t *bits, uint64_t at, uint64_t end) { return at < end && (bits[at >> 3] >> (at & 7) & 1); }

static int control_flow(const ZydisDecodedInstruction *in) {
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSCALL: case ZYDIS_CATEGORY_SYSRET: case ZYDIS_CATEGORY_SYSTEM:
        return 1;
    default:
        return 0;
    }
}

enum { MOVABLE = 1, HEAP = 2, STACK = 4 };

static int is_rsp(ZydisRegister r) { return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r) == ZYDIS_REGISTER_RSP; }

/* MOVABLE: a stub can run it unchanged with the same result (no control flow, nothing rip-relative or thread-local, no
 * 64-bit immediate a relocation could own, rsp left as it is). HEAP: it accesses memory other than the stack, which a
 * page fault can stop. STACK: it reads rsp. */
static unsigned neighbour(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops) {
    if (in->mnemonic == ZYDIS_MNEMONIC_NOP) return MOVABLE;
    if (control_flow(in) || (in->attributes & (ZYDIS_ATTRIB_IS_RELATIVE | ZYDIS_ATTRIB_HAS_LOCK))) return 0;
    switch (in->meta.isa_set) {
    case ZYDIS_ISA_SET_LZCNT: case ZYDIS_ISA_SET_BMI1: case ZYDIS_ISA_SET_BMI2: case ZYDIS_ISA_SET_MOVBE:
    case ZYDIS_ISA_SET_F16C: case ZYDIS_ISA_SET_SSE4A: case ZYDIS_ISA_SET_FMA: case ZYDIS_ISA_SET_FMA4:
    case ZYDIS_ISA_SET_AVX2: case ZYDIS_ISA_SET_AVX2GATHER: case ZYDIS_ISA_SET_XOP: case ZYDIS_ISA_SET_TBM:
        return 0;
    default:
        break;
    }
    for (unsigned k = 0; k < 2; ++k)
        if (in->raw.imm[k].size == 64) return 0;
    unsigned result = MOVABLE;
    for (unsigned k = 0; k < in->operand_count; ++k) {
        const ZydisDecodedOperand *o = &ops[k];
        if (o->type == ZYDIS_OPERAND_TYPE_POINTER) return 0;
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER && is_rsp(o->reg.value)) {
            if (o->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) return 0;
            result |= STACK;
        }
        if (o->type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
        if (o->mem.segment == ZYDIS_REGISTER_FS || o->mem.segment == ZYDIS_REGISTER_GS || o->mem.base == ZYDIS_REGISTER_RIP)
            return 0;
        if (is_rsp(o->mem.base) || is_rsp(o->mem.index)) result |= STACK;
        else if (o->mem.type != ZYDIS_MEMOP_TYPE_AGEN) result |= HEAP;
    }
    return result;
}

/* The CPU_COMPAT_* kind an instruction can have as part of a site, 0 for none. */
static unsigned part_kind_of(const Unit *u, const ZydisDecodedInstruction *in, uint64_t at) {
    unsigned kind = 0;
    if (in->mnemonic == ZYDIS_MNEMONIC_LZCNT) kind = CPU_COMPAT_LZCNT;
    else if (in->mnemonic == ZYDIS_MNEMONIC_TZCNT) kind = CPU_COMPAT_TZCNT;
    else if (in->meta.isa_set == ZYDIS_ISA_SET_F16C) kind = CPU_COMPAT_F16C;
    else if (in->mnemonic == ZYDIS_MNEMONIC_MOVBE) kind = CPU_COMPAT_MOVBE;
    else if (in->meta.isa_set == ZYDIS_ISA_SET_BMI1) kind = CPU_COMPAT_BMI1;
    return kind && cpu_compat_instruction(u->code + at, in->length, kind) == in->length ? kind : 0;
}

static unsigned kind_index(unsigned kind) {
    unsigned n = 1;
    while (kind > 1) { kind >>= 1; ++n; }
    return n;
}

static unsigned kind_of(const Unit *u, const ZydisDecodedInstruction *in, int red) {
    if (in->mnemonic == ZYDIS_MNEMONIC_LZCNT) return CPU_COMPAT_LZCNT;
    if (in->mnemonic == ZYDIS_MNEMONIC_TZCNT) return CPU_COMPAT_TZCNT;
    if (in->meta.isa_set == ZYDIS_ISA_SET_F16C) return CPU_COMPAT_F16C;
    if (!u->module || !red) return 0;
    if (in->mnemonic == ZYDIS_MNEMONIC_MOVBE) return CPU_COMPAT_MOVBE;
    if (in->mnemonic == ZYDIS_MNEMONIC_ANDN || in->mnemonic == ZYDIS_MNEMONIC_BEXTR || in->mnemonic == ZYDIS_MNEMONIC_BLSI ||
        in->mnemonic == ZYDIS_MNEMONIC_BLSMSK || in->mnemonic == ZYDIS_MNEMONIC_BLSR)
        return CPU_COMPAT_BMI1;
    return 0;
}

typedef struct { uint64_t at; uint8_t length, neighbour, stack; unsigned kind, part; } Op;

static int uses_stack(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops) {
    for (unsigned k = 0; k < in->operand_count; ++k) {
        if (ops[k].type == ZYDIS_OPERAND_TYPE_REGISTER && is_rsp(ops[k].reg.value)) return 1;
        if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY && (is_rsp(ops[k].mem.base) || is_rsp(ops[k].mem.index))) return 1;
    }
    return 0;
}

static void add_site(Sites *s, CpuCompatSite site) {
    if (s->count == s->cap) s->items = realloc(s->items, (s->cap = s->cap ? s->cap * 2 : 1024) * sizeof *s->items);
    s->items[s->count++] = site;
}

static void generate(Unit *u, Sites *out) {
    const uint64_t end = u->code_end;
    uint8_t *targets = calloc((size_t)(end / 8 + 1), 1), *taken = calloc((size_t)(end / 8 + 1), 1);
    uint8_t *flags = calloc(u->function_count, 1); /* 1 indirect jump, 2 jump table found, 4 data below rsp */
    for (size_t f = 0; f < u->function_count; ++f) {
        const uint64_t b = u->functions[f].begin, e = u->functions[f].end;
        for (uint64_t at = b; at < e;) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode(u, at, e, &in, ops)) { fprintf(stderr, "%s: undecodable function %#" PRIx64 "\n", u->name, b); ++errors; break; }
            for (unsigned k = 0; k < in.operand_count_visible; ++k) {
                const ZydisDecodedOperand *o = &ops[k];
                if (o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && o->imm.is_relative) mark(targets, at + in.length + (uint64_t)o->imm.value.s, end);
                if (o->type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
                if (o->mem.base == ZYDIS_REGISTER_RSP && o->mem.disp.value < 0 && o->mem.type != ZYDIS_MEMOP_TYPE_AGEN) flags[f] |= 4;
                if (in.mnemonic == ZYDIS_MNEMONIC_LEA && o->mem.base == ZYDIS_REGISTER_RIP && o->mem.index == ZYDIS_REGISTER_NONE) {
                    const uint64_t table = at + in.length + (uint64_t)o->mem.disp.value;
                    if (table >= b && table < e) continue;
                    for (uint64_t entry = 0; entry < 4096 && table + 4 * entry + 4 <= u->code_end; ++entry) {
                        int32_t relative;
                        memcpy(&relative, u->code + table + 4 * entry, 4);
                        const uint64_t to = table + (uint64_t)(int64_t)relative;
                        if (to < b || to >= e) break;
                        mark(targets, to, end);
                        flags[f] |= 2;
                    }
                }
            }
            if (in.meta.category == ZYDIS_CATEGORY_UNCOND_BR && ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) flags[f] |= 1;
            at += in.length;
        }
    }
    size_t cap = 4096;
    Op *list = malloc(cap * sizeof *list);
    unsigned merged = 0, shielded = 0, by_kind[32] = {0};
    for (size_t f = 0; f < u->function_count; ++f) {
        const uint64_t b = u->functions[f].begin, e = u->functions[f].end;
        size_t n = 0, wanted = 0, pending[64], pending_count = 0;
        for (uint64_t at = b; at < e;) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!decode(u, at, e, &in, ops)) break;
            if (n == cap) list = realloc(list, (cap *= 2) * sizeof *list);
            list[n] = (Op){at, in.length, (uint8_t)neighbour(&in, ops), (uint8_t)uses_stack(&in, ops),
                           kind_of(u, &in, flags[f] & 4), part_kind_of(u, &in, at)};
            wanted += list[n].kind != 0;
            ++n;
            at += in.length;
        }
        for (size_t i = 0; wanted && i < n; ++i) {
            const Op *op = &list[i];
            if (!op->kind || marked(taken, op->at, end)) continue;
            if (op->part != op->kind) {
                fprintf(stderr, "%s: unsupported form at %#" PRIx64 "\n", u->name, op->at);
                ++errors;
                continue;
            }
            size_t best_back = 0, best_forward = 0;
            int found = 0, shield = 0;
            const int mergeable = !(flags[f] & 1) || (flags[f] & 2);
            for (size_t total = 0; total < CPU_COMPAT_PARTS && !found; ++total)
                for (size_t forward = total + 1; forward-- > 0 && !found;) {
                    const size_t back = total - forward;
                    if (back > i || i + forward >= n || (total && !mergeable)) continue;
                    unsigned length = 0;
                    int ok = 1, heap = 0, stack = 0;
                    for (size_t k = i - back; k <= i + forward && ok; ++k) {
                        length += list[k].length;
                        if (k != i && !list[k].part && !(list[k].neighbour & MOVABLE)) ok = 0;
                        if (k != i - back && marked(targets, list[k].at, end)) ok = 0;
                        if (!list[k].part) heap |= (list[k].neighbour & HEAP) != 0;
                        stack |= list[k].stack;
                        for (uint64_t x = list[k].at; x < list[k].at + list[k].length && ok; ++x)
                            if (marked(taken, x, end) || (u->relocated && u->relocated[x])) ok = 0;
                    }
                    if (ok && !(heap && stack) && length >= 5 && length <= CPU_COMPAT_SITE_MAX) {
                        found = 1;
                        shield = heap;
                        best_back = back;
                        best_forward = forward;
                    }
                }
            if (!found) {
                /* A later site may still take it in. */
                if (pending_count < sizeof pending / sizeof *pending) pending[pending_count++] = i;
                continue;
            }
            CpuCompatSite site = {(uint32_t)list[i - best_back].at, 0, (uint8_t)shield, {0}};
            for (size_t k = i - best_back, p = 0; k <= i + best_forward; ++k, ++p) {
                const unsigned kind = k == i ? op->kind : list[k].part;
                site.parts[p] = CPU_COMPAT_PART(kind ? kind_index(kind) : 0, list[k].length);
                site.length = (uint8_t)(site.length + list[k].length);
                if (kind) ++by_kind[kind];
                for (uint64_t x = list[k].at; x < list[k].at + list[k].length; ++x) mark(taken, x, end);
            }
            add_site(out, site);
            merged += best_back + best_forward != 0;
            shielded += shield;
        }
        for (size_t p = 0; p < pending_count; ++p) {
            const size_t i = pending[p];
            if (marked(taken, list[i].at, end)) continue;
            fprintf(stderr, "%s: no room for a jump at %#" PRIx64 " (function %#" PRIx64 "):", u->name, list[i].at, b);
            for (size_t k = i >= 2 ? i - 2 : 0; k < n && k <= i + 2; ++k) {
                ZydisDecodedInstruction in;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                decode(u, list[k].at, e, &in, ops);
                fprintf(stderr, " %s%s%s%s%s", k == i ? "[" : "", ZydisMnemonicGetString(in.mnemonic),
                        !list[k].part && !(list[k].neighbour & MOVABLE) ? "(fixed)" : "",
                        marked(targets, list[k].at, end) ? "(target)" : "", k == i ? "]" : "");
            }
            fprintf(stderr, "\n");
            ++errors;
        }
        if (pending_count == sizeof pending / sizeof *pending) { fprintf(stderr, "%s: too many failures\n", u->name); ++errors; }
    }
    printf("  %-26s %7zu functions: LZCNT %u, TZCNT %u, F16C %u, MOVBE %u, BMI1 %u; %u sites take in neighbours, %u of them "
           "shielded\n", u->name, u->function_count, by_kind[CPU_COMPAT_LZCNT], by_kind[CPU_COMPAT_TZCNT],
           by_kind[CPU_COMPAT_F16C], by_kind[CPU_COMPAT_MOVBE], by_kind[CPU_COMPAT_BMI1], merged, shielded);
    free(list);
    free(targets);
    free(taken);
    free(flags);
}

static int compare_sites(const void *a, const void *b) {
    const CpuCompatSite *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

typedef struct { char *text; size_t length, cap; } Text;

__attribute__((format(printf, 2, 3))) static void append(Text *t, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (t->length + (size_t)n + 1 > t->cap) {
        while (t->length + (size_t)n + 1 > t->cap) t->cap = t->cap ? t->cap * 2 : 1 << 16;
        t->text = realloc(t->text, t->cap);
    }
    va_start(args, format);
    vsnprintf(t->text + t->length, t->cap - t->length, format, args);
    va_end(args);
    t->length += (size_t)n;
}

static void emit(Text *out, const Unit *units, const Sites *sites, size_t count) {
    append(out, "/* Generated by tools/cpu_compat_gen.c from the boot images of the supported builds and the modules the game loads\n"
                " * at run time (tools\\test_cpu_compat.ps1 -Generate); do not edit. A row is {offset, length, shield, parts}. */\n");
    for (size_t t = 0; t < count; ++t) {
        append(out, "static const CpuCompatSite cpu_compat_sites_%zu[] = {", t);
        for (size_t i = 0; i < sites[t].count; ++i) {
            const CpuCompatSite *s = &sites[t].items[i];
            append(out, "%s{0x%" PRIx32 ", %u, %u, {", i % 4 ? " " : "\n    ", s->offset, s->length, s->shield);
            for (unsigned p = 0; p < CPU_COMPAT_PARTS && s->parts[p]; ++p) append(out, "%s0x%02x", p ? ", " : "", s->parts[p]);
            append(out, "}},");
        }
        append(out, "\n};\n");
    }
    append(out, "static const CpuCompatTable cpu_compat_tables[] = {\n");
    for (size_t t = 0; t < count; ++t)
        append(out, "    {\"%s\", %" PRIu64 "u, 0x%016" PRIx64 "ull, cpu_compat_sites_%zu, %zu},\n", units[t].name, units[t].size,
               cpu_compat_digest(units[t].code, units[t].module ? units[t].code_end : units[t].size, sites[t].items, sites[t].count),
               t, sites[t].count);
    append(out, "};\n");
}

int main(int argc, char **argv) {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    Unit units[16];
    Sites sites[16];
    size_t count = 0;
    const char *out_path = NULL;
    int check = 0;
    memset(units, 0, sizeof units);
    memset(sites, 0, sizeof sites);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--boot") && i + 2 < argc && count < 16) {
            if (!load_boot(argv[i + 1], argv[i + 2], &units[count++])) return 2;
            i += 2;
        } else if (!strcmp(argv[i], "--module") && i + 2 < argc && count < 16) {
            if (!load_module(argv[i + 1], argv[i + 2], &units[count++])) return 2;
            i += 2;
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--check")) {
            check = 1;
        } else {
            fprintf(stderr, "usage: %s --boot KEY boot.bin ... --module NAME DIR ... (--out FILE | --check)\n", argv[0]);
            return 2;
        }
    }
    for (size_t t = 0; t < count; ++t) {
        generate(&units[t], &sites[t]);
        qsort(sites[t].items, sites[t].count, sizeof *sites[t].items, compare_sites);
    }
    if (errors) {
        fprintf(stderr, "%u sites cannot be rewritten; no table written\n", errors);
        return 1;
    }
    if (out_path) {
        Text generated = {NULL, 0, 0};
        emit(&generated, units, sites, count);
        FILE *f = fopen(out_path, "wb");
        if (!f || fwrite(generated.text, 1, generated.length, f) != generated.length) return 2;
        fclose(f);
        printf("written %s\n", out_path);
    }
    int mismatches = 0;
    for (size_t t = 0; check && t < count; ++t) {
        const CpuCompatTable *table = cpu_compat_boot_table(units[t].name);
        const uint64_t digest = cpu_compat_digest(units[t].code, units[t].module ? units[t].code_end : units[t].size,
                                                  sites[t].items, sites[t].count);
        const int same = table && table->size == units[t].size && table->digest == digest && table->count == sites[t].count &&
                         !memcmp(table->sites, sites[t].items, sites[t].count * sizeof *sites[t].items);
        printf("  %-26s the built-in table %s\n", units[t].name, same ? "matches" : "DOES NOT MATCH; regenerate it");
        mismatches += !same;
    }
    return mismatches ? 1 : 0;
}
