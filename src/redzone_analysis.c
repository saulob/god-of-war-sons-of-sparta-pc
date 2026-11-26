#include "redzone_analysis.h"

#ifdef _WIN32
#include "isa_emulate.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <Zydis/Zydis.h>

static uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t rd32s(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }

static int executable_at(const uint64_t *segs, uint64_t nseg, uint64_t at, uint64_t *segment_end) {
    for (uint64_t s = 0; s < nseg; ++s) {
        const uint64_t base = segs[3 * s], bytes = segs[3 * s + 1], flags = segs[3 * s + 2];
        if ((flags & 1) && at >= base && at - base < bytes) {
            if (segment_end) *segment_end = base + bytes;
            return 1;
        }
    }
    return 0;
}
/* An FDE's pc_begin (pc-relative sdata4), or -1 for a CIE or a bad entry. */
static int64_t fde_begin(const unsigned char *image, uint64_t size, uint64_t fde) {
    if (fde + 16 > size) return -1;
    const uint32_t length = rd32(image + fde);
    if (length < 12 || length == 0xffffffffu || !rd32(image + fde + 4)) return -1;
    return (int64_t)fde + 8 + rd32s(image + fde + 8);
}
/* The entry count of a .eh_frame_hdr at image offset `at` (version 1; eh_frame_ptr pcrel sdata4, fde_count udata4,
 * table datarel sdata4), 0 if this is not one: the table must be sorted, name code in an executable segment, and its
 * first FDEs must agree on where their functions start. */
static uint32_t eh_table(const unsigned char *image, uint64_t size, uint64_t at, const uint64_t *segs, uint64_t nseg) {
    const unsigned char *h = image + at;
    if (at + 12 > size || h[0] != 1 || h[1] != 0x1b || h[2] != 0x03 || h[3] != 0x3b) return 0;
    const uint32_t count = rd32(h + 8);
    if (count < 16 || count > (1u << 22) || at + 12 + (uint64_t)count * 8 > size) return 0;
    const int64_t frames = (int64_t)at + 4 + rd32s(h + 4);
    if (frames < 0 || (uint64_t)frames >= size) return 0;
    int64_t previous = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t pc = (int64_t)at + rd32s(h + 12 + 8 * (uint64_t)i);
        const int64_t fde = (int64_t)at + rd32s(h + 16 + 8 * (uint64_t)i);
        if (pc <= previous || !executable_at(segs, nseg, (uint64_t)pc, NULL) || fde < 0 || (uint64_t)fde + 16 > size)
            return 0;
        if (i < 64 && fde_begin(image, size, (uint64_t)fde) != pc) return 0;
        previous = pc;
    }
    return count;
}
static int compare_functions(const void *a, const void *b) {
    const RzaFunction *x = a, *y = b;
    return x->begin < y->begin ? -1 : x->begin > y->begin;
}

int rza_enumerate(const unsigned char *image, uint64_t size, const uint64_t *segs, uint64_t nseg, RzaImage *out) {
    memset(out, 0, sizeof(*out));
    size_t cap = 0;
    for (uint64_t s = 0; s < nseg; ++s) {
        const uint64_t base = segs[3 * s], end = segs[3 * s] + segs[3 * s + 1];
        for (uint64_t at = (base + 3) & ~UINT64_C(3); at + 12 <= end && at + 12 <= size; at += 4) {
            if (rd32(image + at) != 0x3b031b01u) continue;
            const uint32_t entries = eh_table(image, size, at, segs, nseg);
            if (!entries) continue;
            ++out->tables;
            for (uint32_t i = 0; i < entries; ++i) {
                const uint64_t pc = (uint64_t)((int64_t)at + rd32s(image + at + 12 + 8 * (uint64_t)i));
                const uint64_t fde = (uint64_t)((int64_t)at + rd32s(image + at + 16 + 8 * (uint64_t)i));
                uint64_t segment_end = 0;
                if (fde_begin(image, size, fde) != (int64_t)pc || !executable_at(segs, nseg, pc, &segment_end)) continue;
                const int32_t range = rd32s(image + fde + 12);
                if (range <= 0 || pc + (uint64_t)range > segment_end) continue;
                if (out->count == cap) {
                    cap = cap ? cap * 2 : 65536;
                    RzaFunction *grown = realloc(out->functions, cap * sizeof(*grown));
                    if (!grown) { rza_free(out); return 0; }
                    out->functions = grown;
                }
                out->functions[out->count++] = (RzaFunction){(uint32_t)pc, (uint32_t)(pc + (uint64_t)range), 0};
            }
            at += 12 + (uint64_t)entries * 8 - 4;
        }
    }
    if (!out->count) { rza_free(out); return 0; }
    for (size_t i = 1; i < out->count; ++i)
        if (out->functions[i].begin <= out->functions[i - 1].begin) {
            qsort(out->functions, out->count, sizeof(*out->functions), compare_functions);
            break;
        }
    for (size_t i = 0; i < out->count; ++i)
        if (out->functions[i].end > out->code_end) out->code_end = out->functions[i].end;
    out->starts = calloc((size_t)(out->code_end / 8 + 1), 1);
    out->targets = calloc((size_t)(out->code_end / 8 + 1), 1);
    if (!out->starts || !out->targets) { rza_free(out); return 0; }
    for (size_t i = 0; i < out->count; ++i) out->starts[out->functions[i].begin >> 3] |= (uint8_t)(1u << (out->functions[i].begin & 7));
    return 1;
}

void rza_free(RzaImage *analysis) {
    free(analysis->functions);
    free(analysis->starts);
    free(analysis->targets);
    memset(analysis, 0, sizeof(*analysis));
}

typedef struct {
    const unsigned char *image;
    uint64_t size;
    RzaImage *analysis;
    const RzaRange *ranges;
    size_t range_count;
    const size_t *chunks; /* first function of each task, then count */
    ZydisDecoder decoder;
    volatile LONG unsafe;
} ScanJob;

static int bit_get(const RzaImage *a, const uint8_t *map, uint64_t at) {
    return at < a->code_end && ((map[at >> 3] >> (at & 7)) & 1);
}
static void target_set(RzaImage *a, uint64_t at) {
    if (at >= a->code_end) return;
    uint8_t *byte = &a->targets[at >> 3];
    const uint8_t bit = (uint8_t)(1u << (at & 7));
    if (!(__atomic_load_n(byte, __ATOMIC_RELAXED) & bit)) __atomic_fetch_or(byte, bit, __ATOMIC_RELAXED);
}

/* Whether bytes [from, to) of the instruction at `at` are all inside its plain immediates, or inside the displacement of
 * its only memory operand when that is an fs/gs absolute address. */
static int value_only(const ScanJob *job, uint64_t at, unsigned length, uint64_t from, uint64_t to) {
    ZydisDecodedInstruction full;
    const ZydisDecodedInstruction *in = &full;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&job->decoder, job->image + at, length, &full, ops)) ||
        in->opcode_map == ZYDIS_OPCODE_MAP_0F0F || in->mnemonic == ZYDIS_MNEMONIC_INSERTQ ||
        in->mnemonic == ZYDIS_MNEMONIC_EXTRQ || (in->attributes & ZYDIS_ATTRIB_IS_RELATIVE))
        return 0;
    int memory = 0, absolute_tls = 0;
    for (unsigned k = 0; k < in->operand_count; ++k) {
        if (ops[k].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[k].encoding == ZYDIS_OPERAND_ENCODING_IS4) return 0;
        if (ops[k].type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
        ++memory;
        absolute_tls = (ops[k].mem.segment == ZYDIS_REGISTER_FS || ops[k].mem.segment == ZYDIS_REGISTER_GS) &&
                       ops[k].mem.base == ZYDIS_REGISTER_NONE && ops[k].mem.index == ZYDIS_REGISTER_NONE;
    }
    for (uint64_t b = from; b < to; ++b) {
        const unsigned offset = (unsigned)(b - at);
        int inside = 0;
        for (unsigned k = 0; k < 2; ++k)
            if (in->raw.imm[k].size && offset >= in->raw.imm[k].offset && offset < in->raw.imm[k].offset + in->raw.imm[k].size / 8u)
                inside = 1;
        if (!inside && memory == 1 && absolute_tls && in->raw.disp.size && offset >= in->raw.disp.offset &&
            offset < in->raw.disp.offset + in->raw.disp.size / 8u)
            inside = 1;
        if (!inside) return 0;
    }
    return 1;
}

/* 1 for a relative branch; *target = its image offset, or UINT64_MAX outside the image. */
static int branch_target(const ScanJob *job, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops,
                         uint64_t at, uint64_t *target) {
    for (unsigned k = 0; k < in->operand_count_visible; ++k) {
        if (ops[k].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !ops[k].imm.is_relative) continue;
        ZyanU64 address = 0;
        if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(in, &ops[k], (ZyanU64)(uintptr_t)(job->image + at), &address))) return 0;
        const uint64_t offset = (uint64_t)address - (uint64_t)(uintptr_t)job->image;
        *target = offset < job->size ? offset : UINT64_MAX;
        return 1;
    }
    return 0;
}

/* A jmp into the CPU compatibility stubs: it replaces whole instructions and comes back after them. */
static int own_branch(const ScanJob *job, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, uint64_t at) {
    const RzaImage *a = job->analysis;
    ZyanU64 address = 0;
    if (a->own_begin >= a->own_end || in->mnemonic != ZYDIS_MNEMONIC_JMP || in->length != 5 ||
        ZYAN_FAILED(ZydisCalcAbsoluteAddress(in, &ops[0], (ZyanU64)(uintptr_t)(job->image + at), &address)))
        return 0;
    const uint64_t offset = (uint64_t)address - (uint64_t)(uintptr_t)job->image;
    return offset >= a->own_begin && offset < a->own_end;
}

static void scan_function(ScanJob *job, RzaFunction *f) {
    const unsigned char *image = job->image;
    RzaImage *a = job->analysis;
    size_t lo = 0, hi = job->range_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if ((uint64_t)job->ranges[mid].offset + job->ranges[mid].length <= f->begin) lo = mid + 1; else hi = mid;
    }
    size_t r = lo;
    uint32_t flags = 0;
    for (uint64_t at = f->begin; at < f->end;) {
        ZydisDecoderContext context;
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT_VISIBLE];
        if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&job->decoder, &context, image + at, (ZyanUSize)(f->end - at), &in))) {
            flags |= RZA_BAD;
            if (r < job->range_count && job->ranges[r].offset < f->end) job->unsafe = 1;
            break;
        }
        const uint64_t next = at + in.length;
        for (; r < job->range_count && job->ranges[r].offset < next; ++r) {
            const uint64_t from = job->ranges[r].offset, to = from + job->ranges[r].length;
            if (from < at || to > next || !value_only(job, at, in.length, from, to)) job->unsafe = 1;
            if (to > next) break;
        }
        /* Operands only where they can matter: a relative branch, an unconditional jump, an rsp-based negative offset. */
        const int sib_below = (in.attributes & ZYDIS_ATTRIB_HAS_SIB) && in.raw.disp.size && in.raw.disp.value < 0;
        if ((in.attributes & ZYDIS_ATTRIB_IS_RELATIVE) || in.meta.category == ZYDIS_CATEGORY_UNCOND_BR || sib_below) {
            if (ZYAN_FAILED(ZydisDecoderDecodeOperands(&job->decoder, &context, &in, ops, in.operand_count_visible))) {
                flags |= RZA_BAD;
                break;
            }
            uint64_t target;
            if (branch_target(job, &in, ops, at, &target)) {
                if (target == UINT64_MAX) {
                    if (!own_branch(job, &in, ops, at)) flags |= RZA_HOOKED;
                } else {
                    target_set(a, target);
                    /* Code only enters another function at its start; anything else is a patch. */
                    if ((target < f->begin || target >= f->end) && !bit_get(a, a->starts, target)) flags |= RZA_HOOKED;
                }
            } else if (in.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                       !(ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_RIP &&
                         ops[0].mem.index == ZYDIS_REGISTER_NONE)) {
                flags |= RZA_INDIRECT;
            }
            for (unsigned k = 0; sib_below && k < in.operand_count_visible; ++k)
                if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[k].mem.type != ZYDIS_MEMOP_TYPE_AGEN &&
                    ops[k].mem.base == ZYDIS_REGISTER_RSP && ops[k].mem.disp.value < 0)
                    flags |= RZA_RED;
        }
        if (in.mnemonic == ZYDIS_MNEMONIC_INSERTQ) {
            unsigned char scratch[ISA_INSERTQ_REWRITE_MAX];
            if (isa_insertq_rewrite(image + at, scratch)) flags |= RZA_INSERTQ;
        }
        at = next;
    }
    f->flags = flags;
}

static void scan_task(void *context, size_t task) {
    ScanJob *job = context;
    for (size_t i = job->chunks[task]; i < job->chunks[task + 1]; ++i) scan_function(job, &job->analysis->functions[i]);
}

int rza_scan(const unsigned char *image, uint64_t size, RzaImage *analysis, unsigned workers,
             const RzaRange *volatile_ranges, size_t volatile_count) {
    ScanJob job = {image, size, analysis, volatile_ranges, volatile_count, NULL, {0}, 0};
    if (ZYAN_FAILED(ZydisDecoderInit(&job.decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return -1;
    enum { CHUNK_BYTES = 64 << 10 };
    size_t *chunks = malloc((analysis->count + 1) * sizeof(*chunks));
    if (!chunks) return -1;
    size_t tasks = 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i < analysis->count; ++i) {
        if (!i || bytes >= CHUNK_BYTES) { chunks[tasks++] = i; bytes = 0; }
        bytes += analysis->functions[i].end - analysis->functions[i].begin;
    }
    chunks[tasks] = analysis->count;
    job.chunks = chunks;
    rza_parallel(workers, tasks, scan_task, &job);
    free(chunks);
    return !job.unsafe;
}

unsigned rza_workers(uint64_t bytes) {
    const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    uint64_t n = bytes / (2u << 20);
    if (n > logical - (logical > 2)) n = logical - (logical > 2);
    if (n > 24) n = 24;
    return n ? (unsigned)n : 1;
}

typedef struct {
    void (*run)(void *, size_t);
    void *context;
    size_t tasks;
    volatile LONG64 next;
} Pool;

static DWORD WINAPI pool_worker(LPVOID parameter) {
    Pool *pool = parameter;
    for (;;) {
        const LONG64 task = InterlockedIncrement64(&pool->next) - 1;
        if ((uint64_t)task >= pool->tasks) return 0;
        pool->run(pool->context, (size_t)task);
    }
}

void rza_parallel(unsigned workers, size_t tasks, void (*run)(void *context, size_t task), void *context) {
    Pool pool = {run, context, tasks, 0};
    enum { MAX_WORKERS = 64 };
    HANDLE threads[MAX_WORKERS];
    unsigned started = 0;
    if (workers > tasks) workers = (unsigned)tasks;
    for (unsigned i = 1; i < workers && i < MAX_WORKERS; ++i) {
        threads[started] = CreateThread(NULL, 256 << 10, pool_worker, &pool, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (threads[started]) ++started;
    }
    pool_worker(&pool);
    if (started) WaitForMultipleObjects(started, threads, TRUE, INFINITE);
    for (unsigned i = 0; i < started; ++i) CloseHandle(threads[i]);
}
#endif
