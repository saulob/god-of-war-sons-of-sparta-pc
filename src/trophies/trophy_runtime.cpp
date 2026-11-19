#include "trophies/trophy_runtime.h"

static int32_t next_id;

int32_t trophy_runtime_create_context(int32_t *ctx, int32_t user, uint32_t label, uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return -1;
    *ctx = ++next_id;
    return 0;
}
int32_t trophy_runtime_create_handle(int32_t *handle) {
    if (!handle) return -1;
    *handle = ++next_id;
    return 0;
}
int32_t trophy_runtime_register_context(int32_t ctx, int32_t handle, uint64_t options) {
    (void)ctx; (void)handle; (void)options;
    return 0;
}
int32_t trophy_runtime_unlock(int32_t ctx, int32_t handle, int32_t id, int32_t *platinum) {
    (void)ctx; (void)handle; (void)id;
    if (platinum) *platinum = 0;
    return 0;
}
int32_t trophy_runtime_game_info(int32_t ctx, int32_t handle, void *details, void *data) {
    (void)ctx; (void)handle; (void)details; (void)data;
    return 0;
}
int32_t trophy_runtime_trophy_info(int32_t ctx, int32_t handle, int32_t id, void *details, void *data) {
    (void)ctx; (void)handle; (void)id; (void)details; (void)data;
    return 0;
}
int32_t trophy_runtime_unlock_state(int32_t ctx, int32_t handle, uint32_t *flags, uint32_t *count) {
    (void)ctx; (void)handle;
    if (flags) *flags = 0;
    if (count) *count = 0;
    return 0;
}
int32_t trophy_runtime_destroy_handle(int32_t handle) { (void)handle; return 0; }
int32_t trophy_runtime_destroy_context(int32_t ctx) { (void)ctx; return 0; }
