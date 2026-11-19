#include "redzone_cache.h"

#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <Zydis/Zydis.h>

static const char kMagic[8] = {'S', 'O', 'S', 'R', 'Z', 'C', 'A', '1'};

#define P1 UINT64_C(0x9E3779B185EBCA87)
#define P2 UINT64_C(0xC2B2AE3D27D4EB4F)
#define P3 UINT64_C(0x165667B19E3779F9)

typedef struct {
    uint64_t lane[8]; /* two independent sets of four */
    unsigned char tail[32];
    size_t tail_size;
    uint64_t total;
} Hasher;

static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t round64(uint64_t acc, uint64_t word) { return rotl(acc + word * P2, 31) * P1; }
static uint64_t avalanche(uint64_t h) {
    h ^= h >> 33; h *= P2;
    h ^= h >> 29; h *= P3;
    return h ^ (h >> 32);
}
static void hasher_init(Hasher *h, uint64_t seed) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < 8; ++i) h->lane[i] = seed + (uint64_t)(i + 1) * P1 + (i >= 4 ? P3 : 0);
}
static void stripe(Hasher *h, const unsigned char *p) {
    for (int i = 0; i < 4; ++i) {
        uint64_t w;
        memcpy(&w, p + 8 * i, 8);
        h->lane[i] = round64(h->lane[i], w);
        h->lane[4 + i] = round64(h->lane[4 + i], w ^ P3);
    }
}
static void hasher_update(Hasher *h, const void *data, size_t size) {
    const unsigned char *p = data;
    h->total += size;
    if (h->tail_size) {
        const size_t take = size < 32 - h->tail_size ? size : 32 - h->tail_size;
        memcpy(h->tail + h->tail_size, p, take);
        h->tail_size += take; p += take; size -= take;
        if (h->tail_size < 32) return;
        stripe(h, h->tail);
        h->tail_size = 0;
    }
    for (; size >= 32; p += 32, size -= 32) stripe(h, p);
    memcpy(h->tail, p, size);
    h->tail_size = size;
}
static RzcKey hasher_final(const Hasher *h) {
    uint64_t out[2];
    for (int set = 0; set < 2; ++set) {
        const uint64_t *v = h->lane + 4 * set;
        uint64_t r = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
        for (int i = 0; i < 4; ++i) r = (r ^ round64(0, v[i])) * P1 + P3;
        r ^= h->total;
        for (size_t i = 0; i < h->tail_size; ++i) r = rotl(r ^ (h->tail[i] * P3), 11) * P1;
        out[set] = avalanche(r + (uint64_t)set);
    }
    return (RzcKey){out[0], out[1]};
}

uint64_t rzc_checksum(const void *data, size_t size) {
    Hasher h;
    hasher_init(&h, 0x52435A53u);
    hasher_update(&h, data, size);
    return hasher_final(&h).lo;
}

typedef struct { uint64_t offset, length; } Span;
typedef struct {
    const unsigned char *image;
    const Span *pieces;
    const size_t *tasks; /* first piece of each task, then the piece count */
    const RzaRange *ranges;
    size_t range_count;
    RzcKey *digests;
} FingerprintJob;

static void fingerprint_task(void *context, size_t task) {
    const FingerprintJob *job = context;
    const size_t first = job->tasks[task], last = job->tasks[task + 1];
    size_t r = 0, hi = job->range_count;
    while (r < hi) {
        const size_t mid = r + (hi - r) / 2;
        if ((uint64_t)job->ranges[mid].offset + job->ranges[mid].length <= job->pieces[first].offset) r = mid + 1; else hi = mid;
    }
    Hasher h;
    hasher_init(&h, task);
    unsigned char buffer[16 << 10];
    for (size_t p = first; p < last; ++p) {
        for (uint64_t at = job->pieces[p].offset, end = at + job->pieces[p].length; at < end;) {
            const size_t n = (size_t)(end - at < sizeof(buffer) ? end - at : sizeof(buffer));
            memcpy(buffer, job->image + at, n);
            while (r < job->range_count && (uint64_t)job->ranges[r].offset + job->ranges[r].length <= at) ++r;
            for (size_t k = r; k < job->range_count && job->ranges[k].offset < at + n; ++k) {
                const uint64_t from = job->ranges[k].offset > at ? job->ranges[k].offset : at;
                const uint64_t range_end = (uint64_t)job->ranges[k].offset + job->ranges[k].length;
                const uint64_t to = range_end < at + n ? range_end : at + n;
                if (from < to) memset(buffer + (from - at), 0, (size_t)(to - from));
            }
            hasher_update(&h, buffer, n);
            at += n;
        }
    }
    job->digests[task] = hasher_final(&h);
}

RzcKey rzc_fingerprint(const unsigned char *image, const RzaImage *analysis, const RzaRange *volatile_ranges,
                       size_t volatile_count, unsigned workers) {
    enum { TASK_BYTES = 1 << 20 };
    /* The functions' bytes as merged spans, cut into pieces of at most TASK_BYTES and grouped into tasks of about as
     * much: the same tasks for any worker count. */
    Span *pieces = malloc((analysis->count + 1) * sizeof(*pieces) + ((size_t)(analysis->code_end / TASK_BYTES) + 1) * sizeof(*pieces));
    size_t *tasks = malloc((analysis->count + 2) * sizeof(*tasks) + ((size_t)(analysis->code_end / TASK_BYTES) + 2) * sizeof(*tasks));
    if (!pieces || !tasks) { free(pieces); free(tasks); return (RzcKey){0, 0}; }
    size_t piece_count = 0, task_count = 0;
    uint64_t task_bytes = TASK_BYTES;
    for (size_t i = 0; i < analysis->count;) {
        uint64_t begin = analysis->functions[i].begin, end = analysis->functions[i].end;
        for (++i; i < analysis->count && analysis->functions[i].begin <= end; ++i)
            if (analysis->functions[i].end > end) end = analysis->functions[i].end;
        for (uint64_t at = begin; at < end; at += TASK_BYTES) {
            if (task_bytes >= TASK_BYTES) { tasks[task_count++] = piece_count; task_bytes = 0; }
            pieces[piece_count] = (Span){at, end - at < TASK_BYTES ? end - at : TASK_BYTES};
            task_bytes += pieces[piece_count++].length;
        }
    }
    tasks[task_count] = piece_count;
    RzcKey *digests = calloc(task_count ? task_count : 1, sizeof(*digests));
    if (!digests) { free(pieces); free(tasks); return (RzcKey){0, 0}; }
    FingerprintJob job = {image, pieces, tasks, volatile_ranges, volatile_count, digests};
    rza_parallel(workers, task_count, fingerprint_task, &job);
    Hasher h;
    hasher_init(&h, 0x534F535Au);
    const uint64_t header[] = {RZC_FORMAT, RZC_ALGORITHM, ZYDIS_VERSION, (uint64_t)(uintptr_t)image,
                               analysis->count, analysis->tables, volatile_count, piece_count, task_count};
    hasher_update(&h, header, sizeof(header));
    hasher_update(&h, pieces, piece_count * sizeof(*pieces));
    hasher_update(&h, tasks, (task_count + 1) * sizeof(*tasks));
    hasher_update(&h, digests, task_count * sizeof(*digests));
    uint32_t ranges[2048];
    for (size_t i = 0; i < analysis->count;) {
        size_t n = 0;
        for (; i < analysis->count && n < 2048; ++i, n += 2) {
            ranges[n] = analysis->functions[i].begin;
            ranges[n + 1] = analysis->functions[i].end;
        }
        hasher_update(&h, ranges, n * sizeof(*ranges));
    }
    hasher_update(&h, volatile_ranges, volatile_count * sizeof(*volatile_ranges));
    free(digests);
    free(tasks);
    free(pieces);
    RzcKey key = hasher_final(&h);
    if (!key.lo && !key.hi) key.lo = 1;
    return key;
}

void rzc_path(const char *directory, RzcKey key, char *out, size_t size) {
    snprintf(out, size, "%s/%016llx%016llx.bin", directory, (unsigned long long)key.lo, (unsigned long long)key.hi);
}

/* The flagged functions' ranges, merged and sorted. */
static Span *flagged_spans(const RzcFunction *flagged, size_t count, size_t *spans) {
    Span *out = malloc((count ? count : 1) * sizeof(*out));
    *spans = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (*spans && flagged[i].begin <= out[*spans - 1].offset + out[*spans - 1].length) {
            const uint64_t end = flagged[i].end;
            if (end > out[*spans - 1].offset + out[*spans - 1].length) out[*spans - 1].length = end - out[*spans - 1].offset;
        } else {
            out[(*spans)++] = (Span){flagged[i].begin, (uint64_t)flagged[i].end - flagged[i].begin};
        }
    }
    return out;
}

int rzc_load(const char *directory, RzcKey key, RzaImage *analysis) {
    if (!directory || !*directory) return 0;
    char path[4200];
    rzc_path(directory, key, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *data = NULL;
    long long size = -1;
    if (!_fseeki64(f, 0, SEEK_END)) size = _ftelli64(f);
    if (size >= (long long)sizeof(RzcHeader) && size <= (64ll << 20) && !_fseeki64(f, 0, SEEK_SET) &&
        (data = malloc((size_t)size)) && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (!data) return 0;
    RzcHeader h;
    memcpy(&h, data, sizeof(h));
    const RzcFunction *flagged = (const RzcFunction *)(data + sizeof(h));
    int ok = !memcmp(h.magic, kMagic, sizeof(kMagic)) && h.format == RZC_FORMAT && h.algorithm == RZC_ALGORITHM &&
             h.decoder == ZYDIS_VERSION && h.key_lo == key.lo && h.key_hi == key.hi && h.tables == analysis->tables &&
             h.functions == analysis->count && h.flagged <= h.functions &&
             (uint64_t)size == sizeof(h) + (uint64_t)h.flagged * sizeof(RzcFunction) + (uint64_t)h.targets * 4 &&
             h.checksum == rzc_checksum(data + sizeof(h), (size_t)size - sizeof(h));
    for (uint32_t i = 0; ok && i < h.flagged; ++i) {
        const RzcFunction *x = &flagged[i];
        ok = x->index < analysis->count && (!i || x->index > flagged[i - 1].index) &&
             analysis->functions[x->index].begin == x->begin && analysis->functions[x->index].end == x->end &&
             (x->flags & (RZA_RED | RZA_INSERTQ)) && !(x->flags & ~(uint32_t)RZA_FLAGS);
    }
    size_t span_count = 0;
    Span *spans = ok ? flagged_spans(flagged, h.flagged, &span_count) : NULL;
    const unsigned char *targets = data + sizeof(h) + (size_t)h.flagged * sizeof(RzcFunction);
    ok = ok && spans;
    for (uint32_t i = 0, s = 0; ok && i < h.targets; ++i) {
        uint32_t t, previous = 0;
        memcpy(&t, targets + 4 * (size_t)i, 4);
        if (i) memcpy(&previous, targets + 4 * (size_t)(i - 1), 4);
        while (s < span_count && spans[s].offset + spans[s].length <= t) ++s;
        ok = (!i || t > previous) && s < span_count && t >= spans[s].offset && t < analysis->code_end;
    }
    if (ok) {
        for (uint32_t i = 0; i < h.flagged; ++i) analysis->functions[flagged[i].index].flags = flagged[i].flags;
        for (uint32_t i = 0; i < h.targets; ++i) {
            uint32_t t;
            memcpy(&t, targets + 4 * (size_t)i, 4);
            analysis->targets[t >> 3] |= (uint8_t)(1u << (t & 7));
        }
    }
    free(spans);
    free(data);
    return ok;
}

static void make_directories(const char *directory) {
    char path[4200];
    snprintf(path, sizeof(path), "%s", directory);
    for (char *p = path + 1; *p; ++p)
        if (*p == '/' || *p == '\\') {
            const char saved = *p;
            *p = 0;
            if (p[-1] != ':') CreateDirectoryA(path, NULL);
            *p = saved;
        }
    CreateDirectoryA(path, NULL);
}

/* Keeps the newest RZC_KEPT files and removes leftovers of an interrupted write. */
static void prune(const char *directory, const char *kept) {
    char pattern[4200];
    snprintf(pattern, sizeof(pattern), "%s/*", directory);
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    struct { char name[MAX_PATH]; FILETIME written; } files[64];
    size_t count = 0;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const size_t length = strlen(found.cFileName);
        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", directory, found.cFileName);
        if (strstr(found.cFileName, ".tmp")) { DeleteFileA(path); continue; }
        if (length != 36 || strcmp(found.cFileName + 32, ".bin") || !strcmp(path, kept) || count == 64) continue;
        memcpy(files[count].name, found.cFileName, length + 1);
        files[count++].written = found.ftLastWriteTime;
    } while (FindNextFileA(search, &found));
    FindClose(search);
    while (count >= RZC_KEPT) {
        size_t oldest = 0;
        for (size_t i = 1; i < count; ++i)
            if (CompareFileTime(&files[i].written, &files[oldest].written) < 0) oldest = i;
        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", directory, files[oldest].name);
        DeleteFileA(path);
        files[oldest] = files[--count];
    }
}

int rzc_save(const char *directory, RzcKey key, const RzaImage *analysis) {
    if (!directory || !*directory) return 0;
    size_t flagged = 0, targets = 0;
    for (size_t i = 0; i < analysis->count; ++i) flagged += (analysis->functions[i].flags & (RZA_RED | RZA_INSERTQ)) != 0;
    RzcFunction *functions = malloc((flagged ? flagged : 1) * sizeof(*functions));
    if (!functions) return 0;
    flagged = 0;
    for (size_t i = 0; i < analysis->count; ++i) {
        const RzaFunction *f = &analysis->functions[i];
        if (f->flags & (RZA_RED | RZA_INSERTQ)) functions[flagged++] = (RzcFunction){(uint32_t)i, f->begin, f->end, f->flags};
    }
    size_t span_count = 0;
    Span *spans = flagged_spans(functions, flagged, &span_count);
    uint64_t target_bytes = 0;
    for (size_t s = 0; spans && s < span_count; ++s) target_bytes += spans[s].length;
    uint32_t *list = spans ? malloc((size_t)(target_bytes ? target_bytes : 1) * 4) : NULL;
    for (size_t s = 0; list && s < span_count; ++s)
        for (uint64_t t = spans[s].offset; t < spans[s].offset + spans[s].length && t < analysis->code_end; ++t)
            if ((analysis->targets[t >> 3] >> (t & 7)) & 1) list[targets++] = (uint32_t)t;
    const size_t payload = flagged * sizeof(*functions) + targets * 4;
    unsigned char *data = list ? malloc(sizeof(RzcHeader) + payload) : NULL;
    int ok = 0;
    if (data) {
        memcpy(data + sizeof(RzcHeader), functions, flagged * sizeof(*functions));
        memcpy(data + sizeof(RzcHeader) + flagged * sizeof(*functions), list, targets * 4);
        RzcHeader h;
        memset(&h, 0, sizeof(h));
        memcpy(h.magic, kMagic, sizeof(kMagic));
        h.format = RZC_FORMAT;
        h.algorithm = RZC_ALGORITHM;
        h.decoder = ZYDIS_VERSION;
        h.key_lo = key.lo;
        h.key_hi = key.hi;
        h.tables = analysis->tables;
        h.functions = (uint32_t)analysis->count;
        h.flagged = (uint32_t)flagged;
        h.targets = (uint32_t)targets;
        h.checksum = rzc_checksum(data + sizeof(h), payload);
        memcpy(data, &h, sizeof(h));
        make_directories(directory);
        char path[4200], temporary[4300];
        rzc_path(directory, key, path, sizeof(path));
        snprintf(temporary, sizeof(temporary), "%s.tmp%lu", path, GetCurrentProcessId());
        FILE *f = fopen(temporary, "wb");
        if (f) {
            ok = fwrite(data, 1, sizeof(h) + payload, f) == sizeof(h) + payload && !fflush(f);
            ok = !fclose(f) && ok;
            ok = ok && MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok) DeleteFileA(temporary);
            else prune(directory, path);
        }
    }
    free(data);
    free(list);
    free(spans);
    free(functions);
    return ok;
}
#endif
