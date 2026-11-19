/* The read-only part of the red-zone pass (runtime_redzone.c): the guest functions and what their bytes alone say about
 * them, with no host feature and no host address but the image's own. */
#ifndef SOS_REDZONE_ANALYSIS_H
#define SOS_REDZONE_ANALYSIS_H
#include <stddef.h>
#include <stdint.h>

enum { RZA_RED = 1, RZA_HOOKED = 2, RZA_BAD = 4, RZA_INDIRECT = 8, RZA_INSERTQ = 16, RZA_FLAGS = 31 };
typedef struct { uint32_t begin, end, flags; } RzaFunction;
typedef struct { uint32_t offset, length; } RzaRange;

typedef struct {
    RzaFunction *functions; /* sorted by begin */
    size_t count;
    unsigned tables;
    uint64_t code_end;
    uint8_t *starts, *targets; /* one bit per image byte below code_end */
} RzaImage;

/* Every function of every .eh_frame_hdr table in the image, flags clear. 0 when there is none or no memory. */
int rza_enumerate(const unsigned char *image, uint64_t size, const uint64_t *segments, uint64_t segment_count,
                  RzaImage *out);
/* Flags and branch targets of every function; the result does not depend on the number of workers. `volatile_ranges`
 * (sorted, disjoint) hold values that may differ between runs: returns 0 when one of them lies anywhere in a function
 * but an immediate operand or the displacement of an fs/gs absolute access, where no value changes the result; -1
 * without a decoder or memory. */
int rza_scan(const unsigned char *image, uint64_t size, RzaImage *analysis, unsigned workers,
             const RzaRange *volatile_ranges, size_t volatile_count);
void rza_free(RzaImage *analysis);

/* Threads for `bytes` of guest code: one per 2 MiB, at most 24, leaving one logical processor free on hosts with more
 * than two. */
unsigned rza_workers(uint64_t bytes);
/* run(context, task) for each task in [0, tasks), on the caller and workers - 1 threads. */
void rza_parallel(unsigned workers, size_t tasks, void (*run)(void *context, size_t task), void *context);

#endif
