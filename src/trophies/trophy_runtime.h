#ifndef SOS_TROPHY_RUNTIME_H
#define SOS_TROPHY_RUNTIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Local trophies are optional at this milestone; the runtime answers as a console with no trophy pack installed. */
int32_t trophy_runtime_create_context(int32_t *ctx, int32_t user, uint32_t label, uint64_t options);
int32_t trophy_runtime_create_handle(int32_t *handle);
int32_t trophy_runtime_register_context(int32_t ctx, int32_t handle, uint64_t options);
int32_t trophy_runtime_unlock(int32_t ctx, int32_t handle, int32_t id, int32_t *platinum);
int32_t trophy_runtime_game_info(int32_t ctx, int32_t handle, void *details, void *data);
int32_t trophy_runtime_trophy_info(int32_t ctx, int32_t handle, int32_t id, void *details, void *data);
int32_t trophy_runtime_unlock_state(int32_t ctx, int32_t handle, uint32_t *flags, uint32_t *count);
int32_t trophy_runtime_destroy_handle(int32_t handle);
int32_t trophy_runtime_destroy_context(int32_t ctx);
#ifdef __cplusplus
}
#endif
#endif
