#ifndef SOS_IMAGE_FORMAT_H
#define SOS_IMAGE_FORMAT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SOS_BOOT_MAGIC "SOSBOOT1"
#define SOS_CONTENT_MAGIC "SOSCONT1"
#define SOS_MAGIC_SIZE 8
#define SOS_CONTENT_VALUES 5
#define SOS_CONTENT_PROFILE_SIZE (SOS_MAGIC_SIZE + 4 * SOS_CONTENT_VALUES)
#define SOS_CONTENT_SKU_TRIAL 1u
#define SOS_CONTENT_SKU_FULL 3u

typedef enum { SOS_CONTENT_OK = 0, SOS_CONTENT_BAD_SIZE, SOS_CONTENT_BAD_SIGNATURE, SOS_CONTENT_BAD_SKU } SosContentStatus;

/* Magic, then SKU and the four user-defined param.sfo values as little-endian uint32. */
static inline void sos_content_profile_encode(unsigned char out[SOS_CONTENT_PROFILE_SIZE],
                                              const uint32_t values[SOS_CONTENT_VALUES]) {
    memcpy(out, SOS_CONTENT_MAGIC, SOS_MAGIC_SIZE);
    for (unsigned i = 0; i < SOS_CONTENT_VALUES; ++i)
        for (unsigned b = 0; b < 4; ++b) out[SOS_MAGIC_SIZE + i * 4 + b] = (unsigned char)(values[i] >> (8 * b));
}

static inline SosContentStatus sos_content_profile_parse(const unsigned char *data, size_t size,
                                                         uint32_t values[SOS_CONTENT_VALUES]) {
    if (size != SOS_CONTENT_PROFILE_SIZE) return SOS_CONTENT_BAD_SIZE;
    if (memcmp(data, SOS_CONTENT_MAGIC, SOS_MAGIC_SIZE)) return SOS_CONTENT_BAD_SIGNATURE;
    for (unsigned i = 0; i < SOS_CONTENT_VALUES; ++i) {
        const unsigned char *p = data + SOS_MAGIC_SIZE + i * 4;
        values[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    return values[0] == SOS_CONTENT_SKU_FULL || values[0] == SOS_CONTENT_SKU_TRIAL ? SOS_CONTENT_OK : SOS_CONTENT_BAD_SKU;
}
#endif
