/* Headless red-zone tests. No game, window or audio device is started.
 *   (no mode) boot.bin          install like the loader and print a digest of the final guest code and stubs, every stub
 *                               address normalized: two builds of the installer (or worker counts) must print the same
 *   --verify boot.bin [count]   parallel analysis against a sequential full-decode reference, at several worker counts
 *   --cache boot.bin dir        cache misses, hits and every rejection, each install compared with the uncached one
 *   --bench boot.bin            analysis phase medians
 *   --synthetic                 hand-made functions and malformed function tables
 * Built against the legacy installer (-DLEGACY_REDZONE) only the digest mode exists. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <Zydis/Zydis.h>

#include REDZONE_SOURCE

FILE *sos_stderr_stream(void) { return stdout; }

int runtime_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vprintf(format, args);
    va_end(args);
    return n;
}

static unsigned checks, failures;
static void check(int ok, const char *what) {
    ++checks;
    if (!ok && ++failures <= 40) printf("  FAIL %s\n", what);
}

typedef struct { uint64_t target, kind, value, addend; } BootReloc;
typedef struct {
    unsigned char *file, *pristine;
    uint64_t size, segment_count, reloc_count;
    uint64_t *segments;
    const BootReloc *relocs;
} Boot;

static uint64_t take64(const unsigned char **p) {
    uint64_t v;
    memcpy(&v, *p, 8);
    *p += 8;
    return v;
}

static int load_boot(const char *path, Boot *boot) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    const long long file_size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    boot->file = malloc((size_t)file_size);
    if (!boot->file || fread(boot->file, 1, (size_t)file_size, f) != (size_t)file_size) return 0;
    fclose(f);
    const unsigned char *p = boot->file + 8;
    boot->size = take64(&p);
    take64(&p);
    boot->segment_count = take64(&p);
    boot->reloc_count = take64(&p);
    const uint64_t imports = take64(&p);
    p += 8 * 6;
    p += take64(&p) * 7 * 8;
    p += take64(&p) * 3 * 8;
    boot->segments = calloc(boot->segment_count * 3, 8);
    for (uint64_t i = 0; i < boot->segment_count * 3; ++i) boot->segments[i] = take64(&p);
    p += imports * 128;
    boot->relocs = (const BootReloc *)p;
    p += boot->reloc_count * sizeof(BootReloc);
    boot->pristine = (unsigned char *)p;
    return (uint64_t)(boot->file + file_size - p) == boot->size;
}

static int executable(const Boot *boot, uint64_t at, uint64_t length) {
    for (uint64_t s = 0; s < boot->segment_count; ++s)
        if ((boot->segments[3 * s + 2] & 1) && at >= boot->segments[3 * s] &&
            at + length <= boot->segments[3 * s] + boot->segments[3 * s + 1])
            return 1;
    return 0;
}

/* The guest image as the loader hands it to the installer: relocated (imports at stand-in addresses), TCB loads
 * patched with `tcb`. With `note`, the run-dependent bytes are registered as the loader registers them. */
static unsigned char *prepare_image(const Boot *boot, uint32_t tcb, uint64_t import_base, int note) {
    const uint64_t bytes = (boot->size + 0xffff) & ~0xffffull;
    unsigned char *image = VirtualAlloc((void *)0x800000000ull, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) image = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) return NULL;
    memcpy(image, boot->pristine, boot->size);
    for (uint64_t i = 0; i < boot->reloc_count; ++i) {
        const BootReloc *r = &boot->relocs[i];
        const uint64_t value = r->kind ? import_base + 64 * r->value + r->addend : (uint64_t)(uintptr_t)image + r->value;
        memcpy(image + r->target, &value, 8);
#ifndef LEGACY_REDZONE
        if (note && executable(boot, r->target, 8)) runtime_redzone_add_volatile(r->target, 8);
#endif
    }
    static const unsigned char load[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    for (uint64_t s = 0; s < boot->segment_count; ++s) {
        if (!(boot->segments[3 * s + 2] & 1)) continue;
        unsigned char *at = image + boot->segments[3 * s], *end = at + boot->segments[3 * s + 1] - sizeof(load);
        for (; at <= end; ++at)
            if (at[0] == 0x65 && !memcmp(at, load, sizeof(load))) {
                memcpy(at + 5, &tcb, 4);
#ifndef LEGACY_REDZONE
                if (note) runtime_redzone_add_volatile((uint64_t)(at - image) + 5, 4);
#endif
                at += sizeof(load) - 1;
            }
    }
    (void)note;
    return image;
}

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static uint64_t normalized(uintptr_t address) {
    if (address - (uintptr_t)rz_image < rz_size) return 0x1000000000000000ull | (address - (uintptr_t)rz_image);
    for (size_t c = 0; c < rz_chunks; ++c)
        if (address - (uintptr_t)rz_chunk[c] < RZ_CHUNK)
            return 0x2000000000000000ull | (c * RZ_CHUNK + (address - (uintptr_t)rz_chunk[c]));
    return 0x3000000000000000ull | address;
}

static uint64_t stub_digest(void) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t h = 0;
    for (size_t c = 0; c < rz_chunks; ++c) {
        const unsigned char *base = rz_chunk[c];
        for (size_t at = 0; at < RZ_CHUNK;) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, base + at, RZ_CHUNK - at, &in, ops))) {
                h = mix(h, base[at++]);
                continue;
            }
            unsigned char bytes[16];
            memcpy(bytes, base + at, in.length);
            for (unsigned k = 0; k < in.operand_count_visible; ++k) {
                ZyanU64 target;
                if (ops[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[k].imm.is_relative &&
                    ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in, &ops[k], (uintptr_t)(base + at), &target))) {
                    memset(bytes + in.raw.imm[0].offset, 0, in.raw.imm[0].size / 8);
                    h = mix(h, normalized((uintptr_t)target));
                } else if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[k].mem.base == ZYDIS_REGISTER_RIP &&
                           ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in, &ops[k], (uintptr_t)(base + at), &target))) {
                    memset(bytes + in.raw.disp.offset, 0, in.raw.disp.size / 8);
                    h = mix(h, normalized((uintptr_t)target));
                }
            }
            for (unsigned i = 0; i < in.length; ++i) h = mix(h, bytes[i]);
            at += in.length;
        }
    }
    return h;
}

static uint64_t image_digest(const unsigned char *image, uint64_t size) {
    uint64_t h = 0;
    size_t site = 0;
    for (uint64_t i = 0; i < size; ++i) {
        unsigned char b = image[i];
        while (site < rz_stub_count && rz_stubs[site].site + 5u <= i) ++site;
        for (size_t s = site; s < rz_stub_count && rz_stubs[s].site <= i; ++s)
            if (i > rz_stubs[s].site && i < rz_stubs[s].site + 5u) b = 0;
        h = mix(h, b);
    }
    return h;
}

/* Frees what an install keeps, so another can run in this process. */
static void reset_installer(unsigned char *image, uint64_t size) {
    for (size_t c = 0; c < rz_chunks; ++c) VirtualFree(rz_chunk[c], 0, MEM_RELEASE);
    rz_chunks = rz_chunk_used = rz_stub_bytes = 0;
    rz_no_space = 0;
    free(rz_skipped); rz_skipped = NULL; rz_skipped_count = rz_skipped_cap = 0;
    free((void *)rz_warned); rz_warned = NULL;
    free(rz_stubs); rz_stubs = NULL; rz_stub_count = rz_stub_cap = 0;
    free((void *)rz_stub_seen); rz_stub_seen = NULL;
    rz_rewritten = 0;
#ifndef LEGACY_REDZONE
    rz_volatile_count = 0;
    runtime_redzone_set_cache(NULL);
#endif
    if (image) VirtualFree(image, 0, MEM_RELEASE);
    (void)size;
}

typedef struct { uint64_t image, stubs; size_t sites; } Digest;

static Digest install_digest(const Boot *boot, const char *cache, uint32_t tcb, uint64_t import_base) {
    unsigned char *image = prepare_image(boot, tcb, import_base, 1);
#ifndef LEGACY_REDZONE
    runtime_redzone_set_cache(cache);
#else
    (void)cache;
#endif
    runtime_redzone_install(image, boot->size, boot->segments, boot->segment_count);
    const Digest d = {image_digest(image, boot->size), stub_digest(), rz_stub_count};
    reset_installer(image, boot->size);
    return d;
}

static double now_ms(void) {
    LARGE_INTEGER t, hz;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&hz);
    return (double)t.QuadPart * 1000.0 / (double)hz.QuadPart;
}

#ifndef LEGACY_REDZONE
/* The legacy sequential pass 1, every operand decoded. */
static void reference_scan(const unsigned char *image, uint64_t size, RzaImage *a) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    for (size_t i = 0; i < a->count; ++i) {
        RzaFunction *f = &a->functions[i];
        uint32_t flags = 0;
        for (uint64_t at = f->begin; at < f->end;) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, image + at, (ZyanUSize)(f->end - at), &in, ops))) {
                flags |= RZA_BAD;
                break;
            }
            uint64_t target = 0;
            int relative = 0;
            for (unsigned k = 0; k < in.operand_count_visible && !relative; ++k) {
                if (ops[k].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !ops[k].imm.is_relative) continue;
                ZyanU64 address = 0;
                if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(&in, &ops[k], (ZyanU64)(uintptr_t)(image + at), &address))) break;
                const uint64_t offset = (uint64_t)address - (uint64_t)(uintptr_t)image;
                target = offset < size ? offset : UINT64_MAX;
                relative = 1;
            }
            if (relative) {
                if (target == UINT64_MAX) flags |= RZA_HOOKED;
                else {
                    if (target < a->code_end) a->targets[target >> 3] |= (uint8_t)(1u << (target & 7));
                    const int start = target < a->code_end && ((a->starts[target >> 3] >> (target & 7)) & 1);
                    if ((target < f->begin || target >= f->end) && !start) flags |= RZA_HOOKED;
                }
            } else if (in.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                       !(ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_RIP &&
                         ops[0].mem.index == ZYDIS_REGISTER_NONE)) {
                flags |= RZA_INDIRECT;
            }
            unsigned char scratch[ISA_INSERTQ_REWRITE_MAX];
            if (in.mnemonic == ZYDIS_MNEMONIC_INSERTQ && isa_insertq_rewrite(image + at, scratch)) flags |= RZA_INSERTQ;
            for (unsigned k = 0; k < in.operand_count_visible; ++k)
                if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[k].mem.type != ZYDIS_MEMOP_TYPE_AGEN &&
                    ops[k].mem.base == ZYDIS_REGISTER_RSP && ops[k].mem.disp.value < 0)
                    flags |= RZA_RED;
            at += in.length;
        }
        f->flags = flags;
    }
}

static int same_analysis(const RzaImage *x, const RzaImage *y) {
    if (x->count != y->count || x->code_end != y->code_end || x->tables != y->tables) return 0;
    for (size_t i = 0; i < x->count; ++i)
        if (memcmp(&x->functions[i], &y->functions[i], sizeof(RzaFunction))) return 0;
    return !memcmp(x->targets, y->targets, (size_t)(x->code_end / 8 + 1)) &&
           !memcmp(x->starts, y->starts, (size_t)(x->code_end / 8 + 1));
}

static void normalize_and_copy(RzaRange **out, size_t *count) {
    normalize_volatile();
    *out = malloc((rz_volatile_count ? rz_volatile_count : 1) * sizeof(**out));
    memcpy(*out, rz_volatile, rz_volatile_count * sizeof(**out));
    *count = rz_volatile_count;
    rz_volatile_count = 0;
}

static int rz_isa_needed_for_test(void) {
    const IsaHostFeatures host = isa_host_features();
    return (isa_emulation_needed(host) & ISA_EMULATE_SSE4A) && (host.leaf1_ecx & bit_AVX);
}

/* After an install no INSERTQ is left in any function that had one, patched and jump-table functions included, and each
 * one became a jmp into stub space. */
static void check_insertq_replaced(const Boot *boot, const RzRange *functions, size_t count) {
    unsigned char *image = prepare_image(boot, 0x1480, 0x7ff700000000ull, 1);
    runtime_redzone_install(image, boot->size, boot->segments, boot->segment_count);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t left = 0, jumps = 0;
    for (size_t i = 0; i < count; ++i)
        for (uint64_t at = functions[i].begin; at < functions[i].end;) {
            ZydisDecodedInstruction in;
            if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&decoder, NULL, image + at, functions[i].end - at, &in))) {
                ++at;
                continue;
            }
            left += in.mnemonic == ZYDIS_MNEMONIC_INSERTQ;
            if (in.opcode == 0xe9 && in.length == 5) {
                int32_t rel;
                memcpy(&rel, image + at + 1, 4);
                jumps += in_stubs((uintptr_t)(image + at + 5) + (intptr_t)rel);
            }
            at += in.length;
        }
    printf("  INSERTQ: %" PRIu64 " rewritten in %zu functions, %zu left, %zu jumps into stubs there\n", rz_rewritten, count,
           left, jumps);
    check(left == 0, "no INSERTQ left after the install");
    check(rz_rewritten == 63, "all 63 INSERTQ of CUSA57767 1.00 rewritten");
    reset_installer(image, boot->size);
}

static int verify(const Boot *boot, size_t expected_functions) {
    unsigned char *image = prepare_image(boot, 0x1480, 0x7ff700000000ull, 1);
    RzaRange *ranges;
    size_t range_count;
    normalize_and_copy(&ranges, &range_count);
    RzaImage reference;
    check(rza_enumerate(image, boot->size, boot->segments, boot->segment_count, &reference), "enumerate");
    reference_scan(image, boot->size, &reference);
    size_t flagged[5] = {0}, insertq_left = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < reference.count; ++i) {
        for (int b = 0; b < 5; ++b) flagged[b] += (reference.functions[i].flags >> b) & 1;
        insertq_left += (reference.functions[i].flags & RZA_INSERTQ) && (reference.functions[i].flags & (RZA_HOOKED | RZA_INDIRECT));
        bytes += reference.functions[i].end - reference.functions[i].begin;
    }
    printf("  %u tables, %zu functions, %.1f MiB of code; red %zu, patched %zu, undecodable %zu, indirect jump %zu, "
           "INSERTQ %zu (%zu of them patched or with an indirect jump); %zu run-dependent ranges\n", reference.tables,
           reference.count, (double)bytes / 1048576.0, flagged[0], flagged[1], flagged[2], flagged[3], flagged[4],
           insertq_left, range_count);
    if (expected_functions) check(reference.count == expected_functions, "the expected function count");
    const unsigned counts[] = {1, 2, 3, 4, 8, 16, (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 64};
    for (size_t c = 0; c < sizeof(counts) / sizeof(*counts); ++c) {
        RzaImage parallel;
        rza_enumerate(image, boot->size, boot->segments, boot->segment_count, &parallel);
        const int stable = rza_scan(image, boot->size, &parallel, counts[c], ranges, range_count);
        char what[96];
        snprintf(what, sizeof(what), "%u workers: same flags and branch targets as the reference", counts[c]);
        check(same_analysis(&reference, &parallel), what);
        snprintf(what, sizeof(what), "%u workers: run-dependent bytes are values only", counts[c]);
        check(stable == 1, what);
        rza_free(&parallel);
    }
    RzRange *insertq = malloc(flagged[4] ? flagged[4] * sizeof(*insertq) : 1);
    size_t insertq_count = 0;
    for (size_t i = 0; insertq && i < reference.count; ++i)
        if (reference.functions[i].flags & RZA_INSERTQ)
            insertq[insertq_count++] = (RzRange){reference.functions[i].begin, reference.functions[i].end};
    rza_free(&reference);
    free(ranges);
    VirtualFree(image, 0, MEM_RELEASE);
    check(insertq != NULL, "INSERTQ function list");
    if (insertq && rz_isa_needed_for_test()) check_insertq_replaced(boot, insertq, insertq_count);
    free(insertq);
    const char *forced = getenv("SOS_RED_ZONE_WORKERS");
    Digest digests[4];
    const char *workers[] = {"1", "2", "4", "32"};
    for (int i = 0; i < 4; ++i) {
        SetEnvironmentVariableA("SOS_RED_ZONE_WORKERS", workers[i]);
        _putenv_s("SOS_RED_ZONE_WORKERS", workers[i]);
        digests[i] = install_digest(boot, NULL, 0x1480, 0x7ff700000000ull);
        char what[96];
        snprintf(what, sizeof(what), "install with %s workers equals 1 worker", workers[i]);
        check(!memcmp(&digests[i], &digests[0], sizeof(Digest)), what);
    }
    _putenv_s("SOS_RED_ZONE_WORKERS", forced ? forced : "");
    return 1;
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, size, f) == size;
    return !fclose(f) && ok;
}
static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    *size = (size_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    unsigned char *data = malloc(*size ? *size : 1);
    if (data && fread(data, 1, *size, f) != *size) { free(data); data = NULL; }
    fclose(f);
    return data;
}

static int count_files(const char *directory) {
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s/*", directory);
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    int n = 0;
    if (search == INVALID_HANDLE_VALUE) return 0;
    do n += !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY); while (FindNextFileA(search, &found));
    FindClose(search);
    return n;
}

/* Rewrites the header checksum after a payload change, so the deeper checks are reached. */
static void reseal(unsigned char *data, size_t size) {
    RzcHeader h;
    memcpy(&h, data, sizeof(h));
    h.checksum = rzc_checksum(data + sizeof(h), size - sizeof(h));
    memcpy(data, &h, sizeof(h));
}

typedef struct {
    const Boot *boot;
    unsigned char *image;
    RzaRange *ranges;
    size_t range_count;
    RzcKey key;
    RzaImage reference;
} CacheFixture;

static int load_into_fresh(CacheFixture *x, const char *directory, RzaImage *out) {
    rza_enumerate(x->image, x->boot->size, x->boot->segments, x->boot->segment_count, out);
    return rzc_load(directory, x->key, out);
}

static void expect_rejected(CacheFixture *x, const char *directory, const char *path, const unsigned char *data, size_t size,
                            const char *what) {
    write_file(path, data, size);
    RzaImage a;
    const int loaded = load_into_fresh(x, directory, &a);
    int untouched = 1;
    for (size_t i = 0; i < a.count; ++i) untouched &= a.functions[i].flags == 0;
    for (size_t i = 0; untouched && i <= a.code_end / 8; ++i) untouched &= a.targets[i] == 0;
    char text[160];
    snprintf(text, sizeof(text), "rejected, analysis untouched: %s", what);
    check(!loaded && untouched, text);
    rza_free(&a);
}

static int cache_tests(const Boot *boot, const char *directory) {
    char command[700];
    snprintf(command, sizeof(command), "%s/*", directory);
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(command, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char path[700];
            snprintf(path, sizeof(path), "%s/%s", directory, found.cFileName);
            DeleteFileA(path);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    const Digest uncached = install_digest(boot, NULL, 0x1480, 0x7ff700000000ull);
    const Digest cold = install_digest(boot, directory, 0x1480, 0x7ff700000000ull);
    check(!memcmp(&cold, &uncached, sizeof(Digest)), "cold miss installs what an uncached run installs");
    check(count_files(directory) == 1, "cold miss writes one cache file");
    const Digest warm = install_digest(boot, directory, 0x1480, 0x7ff700000000ull);
    check(!memcmp(&warm, &uncached, sizeof(Digest)), "cache hit installs what an uncached run installs");
    const Digest other_slot = install_digest(boot, directory, 0x1488, 0x7ff600000000ull);
    check(count_files(directory) == 1, "another TCB slot and other import addresses reuse the same file");
    check(other_slot.sites == uncached.sites && other_slot.stubs == uncached.stubs,
          "another TCB slot and other import addresses: same stubs");

    CacheFixture x = {boot, prepare_image(boot, 0x1480, 0x7ff700000000ull, 1), NULL, 0, {0, 0}, {0}};
    normalize_and_copy(&x.ranges, &x.range_count);
    rza_enumerate(x.image, boot->size, boot->segments, boot->segment_count, &x.reference);
    x.key = rzc_fingerprint(x.image, &x.reference, x.ranges, x.range_count, 8);
    check(x.key.lo == rzc_fingerprint(x.image, &x.reference, x.ranges, x.range_count, 1).lo, "fingerprint independent of workers");
    rza_scan(x.image, boot->size, &x.reference, 8, x.ranges, x.range_count);
    char path[700];
    rzc_path(directory, x.key, path, sizeof(path));
    size_t size = 0;
    unsigned char *valid = read_file(path, &size);
    check(valid != NULL, "cache file at the keyed path");
    if (!valid) return 0;
    RzaImage hit;
    check(load_into_fresh(&x, directory, &hit), "valid file loads");
    int same = 1;
    for (size_t i = 0; i < hit.count; ++i)
        same &= (hit.functions[i].flags & (RZA_RED | RZA_INSERTQ)) ?
                hit.functions[i].flags == x.reference.functions[i].flags : hit.functions[i].flags == 0;
    check(same, "hit restores the flags of every red-zone or INSERTQ function");
    rza_free(&hit);

    RzcHeader h;
    memcpy(&h, valid, sizeof(h));
    unsigned char *bad = malloc(size + 64);
    RzcFunction *functions = (RzcFunction *)(bad + sizeof(h));
    uint32_t *targets = (uint32_t *)(bad + sizeof(h) + (size_t)h.flagged * sizeof(RzcFunction));
#define MUTATE(what, edit, length, sealed) do { memcpy(bad, valid, size); edit; if (sealed) reseal(bad, length); \
        expect_rejected(&x, directory, path, bad, length, what); } while (0)
    RzcHeader *bh = (RzcHeader *)bad;
    MUTATE("bad magic", bad[0] ^= 1, size, 0);
    MUTATE("bad format version", bh->format += 1, size, 0);
    MUTATE("algorithm version mismatch", bh->algorithm += 1, size, 0);
    MUTATE("decoder version mismatch", bh->decoder ^= 1, size, 0);
    MUTATE("wrong key (another boot image or patch profile)", bh->key_lo ^= 1, size, 0);
    MUTATE("function count mismatch", bh->functions += 1, size, 0);
    MUTATE("table count mismatch", bh->tables += 1, size, 0);
    MUTATE("truncated file", (void)0, size - 4, 0);
    MUTATE("truncated header", (void)0, sizeof(h) - 1, 0);
    MUTATE("empty file", (void)0, 0, 0);
    MUTATE("trailing bytes", memset(bad + size, 0, 4), size + 4, 0);
    MUTATE("oversized count", bh->flagged = 0x40000000u, size, 0);
    MUTATE("oversized target count", bh->targets += 1, size, 0);
    MUTATE("corrupt payload", bad[size - 1] ^= 0x40, size, 0);
    if (h.flagged >= 2 && h.targets >= 2) {
        MUTATE("out-of-range function index", functions[0].index = (uint32_t)boot->size, size, 1);
        MUTATE("function range does not match the image", functions[0].end += 1, size, 1);
        MUTATE("duplicate function", functions[1] = functions[0], size, 1);
        MUTATE("unsorted functions", { RzcFunction t = functions[0]; functions[0] = functions[1]; functions[1] = t; }, size, 1);
        MUTATE("unknown flag", functions[0].flags |= 0x100, size, 1);
        MUTATE("flags without red zone or INSERTQ", functions[0].flags = RZA_HOOKED, size, 1);
        MUTATE("duplicate target", targets[1] = targets[0], size, 1);
        MUTATE("unsorted targets", { uint32_t t = targets[0]; targets[0] = targets[1]; targets[1] = t; }, size, 1);
        MUTATE("target outside every flagged function", targets[0] = 0, size, 1);
        MUTATE("out-of-range target", targets[h.targets - 1] = 0xfffffff0u, size, 1);
    }
    write_file(path, valid, size);
    RzaImage again;
    check(load_into_fresh(&x, directory, &again), "restored file loads again");
    rza_free(&again);

    /* A patch that changes how code decodes: another key, a miss. */
    unsigned char saved = x.image[x.reference.functions[1000].begin];
    x.image[x.reference.functions[1000].begin] ^= 0xff;
    const RzcKey changed = rzc_fingerprint(x.image, &x.reference, x.ranges, x.range_count, 8);
    check(changed.lo != x.key.lo || changed.hi != x.key.hi, "changed code byte changes the key");
    x.image[x.reference.functions[1000].begin] = saved;
    RzaRange *fewer = x.range_count ? x.ranges + 1 : x.ranges;
    const RzcKey other_mask = rzc_fingerprint(x.image, &x.reference, fewer, x.range_count ? x.range_count - 1 : 0, 8);
    check(other_mask.lo != x.key.lo || other_mask.hi != x.key.hi, "changed run-dependent ranges change the key");

    /* A range that is not a value: the analysis reports it, nothing is cached. */
    RzaRange opcode = {x.reference.functions[2000].begin, 1};
    RzaImage unstable;
    rza_enumerate(x.image, boot->size, boot->segments, boot->segment_count, &unstable);
    check(rza_scan(x.image, boot->size, &unstable, 4, &opcode, 1) == 0, "an opcode byte is not a run-dependent value");
    rza_free(&unstable);

    /* Atomic replacement: a leftover temporary is never read and is removed by the next save. */
    char temporary[760];
    snprintf(temporary, sizeof(temporary), "%s.tmp999", path);
    write_file(temporary, "partial", 7);
    RzaImage still;
    check(load_into_fresh(&x, directory, &still), "the valid file wins over a leftover temporary");
    rza_free(&still);
    check(rzc_save(directory, x.key, &x.reference), "save replaces the file");
    check(GetFileAttributesA(temporary) == INVALID_FILE_ATTRIBUTES, "save removes leftover temporaries");
    size_t resaved_size = 0;
    unsigned char *resaved = read_file(path, &resaved_size);
    check(resaved && resaved_size == size && !memcmp(resaved, valid, size), "save is deterministic");
    free(resaved);
    for (uint64_t k = 0; k < 6; ++k) {
        RzcKey other = {x.key.lo ^ (k + 1), x.key.hi};
        rzc_save(directory, other, &x.reference);
    }
    check(count_files(directory) == RZC_KEPT, "only the newest files are kept");
    free(bad);
    free(valid);
    free(x.ranges);
    rza_free(&x.reference);
    VirtualFree(x.image, 0, MEM_RELEASE);
    const Digest after = install_digest(boot, directory, 0x1480, 0x7ff700000000ull);
    check(!memcmp(&after, &uncached, sizeof(Digest)), "evicted entry: analyzed again, same install");
    return 1;
}

static int compare_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void bench(const Boot *boot, const char *directory) {
    enum { RUNS = 7 };
    unsigned char *image = prepare_image(boot, 0x1480, 0x7ff700000000ull, 1);
    RzaRange *ranges;
    size_t range_count;
    normalize_and_copy(&ranges, &range_count);
    double t[RUNS];
    for (int r = 0; r < RUNS; ++r) {
        RzaImage a;
        const double start = now_ms();
        rza_enumerate(image, boot->size, boot->segments, boot->segment_count, &a);
        t[r] = now_ms() - start;
        rza_free(&a);
    }
    qsort(t, RUNS, sizeof(*t), compare_double);
    printf("  enumerate            %7.1f ms\n", t[RUNS / 2]);
    RzaImage a;
    rza_enumerate(image, boot->size, boot->segments, boot->segment_count, &a);
    uint64_t bytes = 0;
    for (size_t i = 0; i < a.count; ++i) bytes += a.functions[i].end - a.functions[i].begin;
    printf("  default workers      %u\n", rza_workers(bytes));
    const unsigned counts[] = {1, 2, 4, 8, 12, 16, 24, 32};
    for (size_t c = 0; c < sizeof(counts) / sizeof(*counts); ++c) {
        for (int r = 0; r < RUNS; ++r) {
            const double start = now_ms();
            const RzcKey key = rzc_fingerprint(image, &a, ranges, range_count, counts[c]);
            t[r] = now_ms() - start;
            (void)key;
        }
        qsort(t, RUNS, sizeof(*t), compare_double);
        const double fingerprint = t[RUNS / 2];
        for (int r = 0; r < (counts[c] < 4 ? 3 : RUNS); ++r) {
            memset(a.targets, 0, (size_t)(a.code_end / 8 + 1));
            const double start = now_ms();
            rza_scan(image, boot->size, &a, counts[c], ranges, range_count);
            t[r] = now_ms() - start;
        }
        const int runs = counts[c] < 4 ? 3 : RUNS;
        qsort(t, (size_t)runs, sizeof(*t), compare_double);
        printf("  %2u workers: scan %7.1f ms, fingerprint %5.1f ms\n", counts[c], t[runs / 2], fingerprint);
    }
    const RzcKey key = rzc_fingerprint(image, &a, ranges, range_count, rza_workers(bytes));
    for (int r = 0; r < RUNS; ++r) {
        const double start = now_ms();
        rzc_save(directory, key, &a);
        t[r] = now_ms() - start;
    }
    qsort(t, RUNS, sizeof(*t), compare_double);
    printf("  cache save           %7.1f ms\n", t[RUNS / 2]);
    for (int r = 0; r < RUNS; ++r) {
        RzaImage b;
        rza_enumerate(image, boot->size, boot->segments, boot->segment_count, &b);
        const double start = now_ms();
        rzc_load(directory, key, &b);
        t[r] = now_ms() - start;
        rza_free(&b);
    }
    qsort(t, RUNS, sizeof(*t), compare_double);
    printf("  cache load           %7.1f ms\n", t[RUNS / 2]);
    rza_free(&a);
    free(ranges);
    VirtualFree(image, 0, MEM_RELEASE);
    for (int mode = 0; mode < 2; ++mode) {
        for (int r = 0; r < 5; ++r) {
            unsigned char *installed = prepare_image(boot, 0x1480, 0x7ff700000000ull, 1);
            runtime_redzone_set_cache(mode ? directory : NULL);
            if (mode) runtime_redzone_set_cache(directory);
            const double start = now_ms();
            runtime_redzone_install(installed, boot->size, boot->segments, boot->segment_count);
            t[r] = now_ms() - start;
            reset_installer(installed, boot->size);
        }
        qsort(t, 5, sizeof(*t), compare_double);
        printf("  install, %-11s %7.1f ms (median of 5)\n", mode ? "cache hit" : "no cache", t[2]);
    }
}

/* A tiny image: one executable segment of code, one data segment holding a .eh_frame_hdr and its FDEs. */
typedef struct { unsigned char bytes[0x4000]; uint64_t segments[6]; } Tiny;

static void tiny_function(Tiny *t, unsigned index, uint32_t pc, int32_t length, int fde_ok) {
    const uint32_t header = 0x2000, fde = 0x3000 + index * 32;
    const int32_t pc_rel = (int32_t)pc - (int32_t)header, fde_rel = (int32_t)fde - (int32_t)header;
    memcpy(t->bytes + header + 12 + 8 * index, &pc_rel, 4);
    memcpy(t->bytes + header + 16 + 8 * index, &fde_rel, 4);
    const uint32_t fde_length = 28, cie_pointer = 1;
    const int32_t begin = (int32_t)pc - (int32_t)(fde + 8) + (fde_ok ? 0 : 1);
    memcpy(t->bytes + fde, &fde_length, 4);
    memcpy(t->bytes + fde + 4, &cie_pointer, 4);
    memcpy(t->bytes + fde + 8, &begin, 4);
    memcpy(t->bytes + fde + 12, &length, 4);
}

static void synthetic(void) {
    static Tiny t;
    memset(&t, 0, sizeof(t));
    const uint64_t segments[6] = {0x0, 0x1000, 5, 0x2000, 0x2000, 6};
    memcpy(t.segments, segments, sizeof(segments));
    static const unsigned char code[][24] = {
        {0x48, 0x89, 0x44, 0x24, 0xf8, 0xc3},                                /* mov [rsp-8],rax: red zone */
        {0xe9, 0x00, 0x10, 0x00, 0x00},                                      /* jmp out of the image: patched */
        {0xff, 0xe0},                                                        /* jmp rax: indirect */
        {0xf2, 0x0f, 0x78, 0xc2, 0x10, 0x30, 0xc3},                          /* insertq xmm0,xmm2,16,48 */
        {0x0f, 0xff, 0xff, 0xff},                                            /* undecodable */
        {0x48, 0x8d, 0x44, 0x24, 0xf8, 0xeb, 0x00, 0xc3},                    /* lea [rsp-8] only, jmp to next */
        {0x65, 0x48, 0x8b, 0x04, 0x25, 0x80, 0x14, 0x00, 0x00, 0xc3},        /* TCB load */
    };
    static const unsigned char sizes[] = {6, 5, 2, 7, 4, 8, 10};
    static const uint32_t expected[] = {RZA_RED, RZA_HOOKED, RZA_INDIRECT, RZA_INSERTQ, RZA_BAD, 0, 0};
    enum { N = 7, FILLERS = 6 };
    unsigned index = 0;
    for (unsigned i = 0; i < N; ++i, ++index) {
        memcpy(t.bytes + 0x100 * (i + 1), code[i], sizeof(code[i]));
        tiny_function(&t, index, 0x100 * (i + 1), sizes[i], 1);
    }
    /* Ranges the enumeration skips one by one: empty, negative, past the end of the segment. */
    tiny_function(&t, index++, 0x900, 0, 1);
    tiny_function(&t, index++, 0x910, -4, 1);
    for (unsigned i = 0; i < FILLERS; ++i) {
        memset(t.bytes + 0x930 + 0x10 * i, 0x90, 4);
        tiny_function(&t, index++, 0x930 + 0x10 * i, 4, 1);
    }
    tiny_function(&t, index++, 0xff0, 0x40, 1);
    const uint32_t count = index;
    static const unsigned char head[] = {1, 0x1b, 0x03, 0x3b};
    memcpy(t.bytes + 0x2000, head, 4);
    memcpy(t.bytes + 0x2008, &count, 4);
    RzaImage reference;
    check(rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &reference), "tiny image: table found");
    check(reference.count == N + FILLERS, "tiny image: malformed ranges skipped");
    reference_scan(t.bytes, sizeof(t.bytes), &reference);
    for (unsigned i = 0; i < N && reference.count == N + FILLERS; ++i) {
        char what[64];
        snprintf(what, sizeof(what), "tiny function %u classified", i);
        check(reference.functions[i].flags == expected[i], what);
    }
    const RzaRange values[] = {{0x700 + 5, 4}};
    for (unsigned workers = 1; workers <= 8; workers *= 2) {
        RzaImage parallel;
        rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &parallel);
        check(rza_scan(t.bytes, sizeof(t.bytes), &parallel, workers, values, 1) == 1, "tiny: TCB displacement is a value");
        check(same_analysis(&reference, &parallel), "tiny: parallel equals reference");
        rza_free(&parallel);
    }
    const RzaRange spans_two[] = {{0x700 + 8, 4}};
    RzaImage x;
    rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &x);
    check(rza_scan(t.bytes, sizeof(t.bytes), &x, 2, spans_two, 1) == 0, "tiny: a range across instructions is refused");
    rza_free(&x);
    const RzaRange in_bad[] = {{0x500 + 2, 1}};
    rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &x);
    check(rza_scan(t.bytes, sizeof(t.bytes), &x, 2, in_bad, 1) == 0, "tiny: a range in an undecodable function is refused");
    rza_free(&x);
    unsigned char saved[8];
    memcpy(saved, t.bytes + 0x2000 + 12, 8);
    memcpy(t.bytes + 0x2000 + 12, t.bytes + 0x2000 + 20, 8);
    check(!rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &x), "tiny: an unsorted table is refused whole");
    memcpy(t.bytes + 0x2000 + 12, saved, 8);
    const uint32_t not_code = (uint32_t)(0x2100 - 0x2000);
    memcpy(saved, t.bytes + 0x2000 + 12, 4);
    memcpy(t.bytes + 0x2000 + 12, &not_code, 4);
    check(!rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &x), "tiny: a table naming data is refused whole");
    memcpy(t.bytes + 0x2000 + 12, saved, 4);
    memset(t.bytes + 0x2000, 0, 4);
    check(!rza_enumerate(t.bytes, sizeof(t.bytes), t.segments, 2, &x), "tiny: no table, no functions");
    rza_free(&reference);
}
#endif

int main(int argc, char **argv) {
    const char *mode = argc > 1 && argv[1][0] == '-' ? argv[1] : "";
    const char *boot_path = *mode ? (argc > 2 ? argv[2] : NULL) : (argc > 1 ? argv[1] : NULL);
    Boot boot;
#ifndef LEGACY_REDZONE
    if (!strcmp(mode, "--synthetic")) {
        synthetic();
        printf("Red zone synthetic: %u checks, %u failures\n", checks, failures);
        return failures != 0;
    }
#endif
    if (!boot_path || !load_boot(boot_path, &boot)) {
        fprintf(stderr, "usage: redzone_tests [--verify|--cache dir|--bench dir|--synthetic] boot.bin\n");
        return 2;
    }
    if (!*mode) {
        const double start = now_ms();
        const Digest d = install_digest(&boot, NULL, 0x1480, 0x7ff700000000ull);
        printf("DIGEST image=%016" PRIx64 " stubs=%016" PRIx64 " sites=%zu\nTIME %.1f\n", d.image, d.stubs, d.sites,
               now_ms() - start);
        return 0;
    }
#ifndef LEGACY_REDZONE
    if (!strcmp(mode, "--verify")) {
        verify(&boot, argc > 3 ? (size_t)strtoull(argv[3], NULL, 10) : 0);
        printf("Red zone verify: %u checks, %u failures\n", checks, failures);
        return failures != 0;
    }
    if (!strcmp(mode, "--cache") && argc > 3) {
        cache_tests(&boot, argv[3]);
        printf("Red zone cache: %u checks, %u failures\n", checks, failures);
        return failures != 0;
    }
    if (!strcmp(mode, "--bench") && argc > 3) {
        bench(&boot, argv[3]);
        return 0;
    }
#endif
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
