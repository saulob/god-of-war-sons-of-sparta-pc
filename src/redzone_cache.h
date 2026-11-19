/* The red-zone analysis (redzone_analysis.h) of a guest image, kept between runs. A file only answers for the exact
 * bytes it was made from: its key covers every byte the analysis reads. */
#ifndef SOS_REDZONE_CACHE_H
#define SOS_REDZONE_CACHE_H
#include "redzone_analysis.h"

typedef struct { uint64_t lo, hi; } RzcKey;

/* The function ranges and their bytes with `volatile_ranges` zeroed, those ranges, the image address and the analysis
 * and decoder versions; {0, 0} without memory. */
RzcKey rzc_fingerprint(const unsigned char *image, const RzaImage *analysis, const RzaRange *volatile_ranges,
                       size_t volatile_count, unsigned workers);
/* Sets the flags and the branch targets inside flagged functions from the file for `key` in `directory`. 0 when there
 * is none or it fails any check; `analysis` is then unchanged. */
int rzc_load(const char *directory, RzcKey key, RzaImage *analysis);
/* Writes the file through a temporary and a rename, keeping the newest few; 0 on failure. */
int rzc_save(const char *directory, RzcKey key, const RzaImage *analysis);

/* File layout, for the tests. */
enum { RZC_FORMAT = 1, RZC_ALGORITHM = 1, RZC_KEPT = 4 };
typedef struct {
    char magic[8];
    uint32_t format, algorithm;
    uint64_t decoder, key_lo, key_hi;
    uint32_t tables, functions, flagged, targets;
    uint64_t checksum; /* of everything after the header */
} RzcHeader;
typedef struct { uint32_t index, begin, end, flags; } RzcFunction;
void rzc_path(const char *directory, RzcKey key, char *out, size_t size);
uint64_t rzc_checksum(const void *data, size_t size);

#endif
