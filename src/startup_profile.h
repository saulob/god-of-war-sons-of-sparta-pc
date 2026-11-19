/* The pre-splash window, from the Unity bootstrap to the first AvPlayer source, in which the coarse startup milestones are
 * logged. With SOS_STARTUP_PROFILE=1 it is also profiled: where the guest main thread spends its time (sampled every
 * 2 ms) and the summed wall time of the host services the guest calls, reported once at the end. Otherwise every cost
 * hook is a single relaxed load. */
#ifndef SOS_STARTUP_PROFILE_H
#define SOS_STARTUP_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOS_COST_FILE_OPEN,
    SOS_COST_FILE_READ,
    SOS_COST_FILE_STAT,
    SOS_COST_MEMORY_MAP,
    SOS_COST_MEMORY_PROTECT,
    SOS_COST_THREAD_CREATE,
    SOS_COST_MODULE_LOAD,
    SOS_COST_DLSYM,
    SOS_COST_SLEEP,
    SOS_COST_GPU_FAULT,
    SOS_COST_EMULATED,
    SOS_COST_COUNT
} SosCost;

/* On the guest main thread, just before it enters the guest. */
void sos_startup_profile_begin(const void *image, uint64_t image_size);
/* Stops the sampler and logs the report, once. */
void sos_startup_profile_end(void);

/* Inside the window (milestones), profiled or not. */
int sos_startup_profile_active(void);

/* 0 outside the window; otherwise a start stamp for sos_cost_end. */
int64_t sos_cost_begin(void);
void sos_cost_end(SosCost kind, int64_t start, uint64_t amount);

#ifdef __cplusplus
}
#endif

#endif
