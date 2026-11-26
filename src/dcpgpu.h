/* Interface between the loader/runtime and the video core (shadPS4-derived, gpu/shim/dcpgpu.cpp). */
#ifndef DCPGPU_H
#define DCPGPU_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* user folder: the pipeline cache lives in its cache folder */
    const char *settings_file;  /* dcp.ini */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    uint32_t features;          /* DCP_GPU_FEATURE_*: what the game build supports (the Options menu shows only those) */
    int32_t display_width, display_height; /* the display sceVideoOutGetResolutionStatus reports; 0: 1920x1080 */
    const char *version;        /* the game build's APP_VER ("1.01"): each build keeps its own pipeline cache; "" or NULL if unknown */
} DcpGpuConfig;
#define DCP_GPU_FEATURE_RENDER_RESOLUTION (1u << 0)
#define DCP_GPU_FEATURE_TRUE_ULTRAWIDE (1u << 1)
#define DCP_GPU_FEATURE_JAPANESE_NPC_VOICES (1u << 2)
#define DCP_GPU_FEATURE_UNLOCK_GALLERY (1u << 3)
#define DCP_GPU_FEATURE_BOSS_BATTLE (1u << 4)
#define DCP_GPU_FEATURE_SKIP_INTRO (1u << 5)
/* 1 when a video core is linked in, 0 for the stub. */
int dcpgpu_available(void);
/* Registers kernel event queues (needed with or without graphics). */
void dcpgpu_register_kernel(void);
/* Creates window, renderer and GPU command processor; 0 on success. */
int dcpgpu_init(const DcpGpuConfig *config);
/* Function for an imported NID ("NID#lib"), or 0 when the video core does not provide it. */
uintptr_t dcpgpu_resolve(const char *scoped_nid);
/* A compact one-line summary of the first-frame stages, for the STOP/crash report. */
void dcpgpu_first_frame_summary(char *out, size_t size);
/* Called first by the loader's fault handler: 1 when a GPU page-tracking fault was handled. */
int dcpgpu_handle_fault(void *context, void *address);
void dcpgpu_dump_guest_writes(void *context);
/* Number of symbols registered by the video core (diagnostics). */
unsigned dcpgpu_symbol_count(void);
/* Queues the shutdown a closed window performs (clean-exit mark, log finalize, process exit). Callable from any thread;
 * 1 for the first request, 0 with no window or once already requested. */
int dcpgpu_request_close(void);
/* Keyboard text entry and list selection drawn by the video core; begin returns 0 without a window.
 * text poll: 0 typing, 1 confirmed, 2 cancelled. choice poll: -1 open, -2 cancelled, else the index. */
int dcpgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int dcpgpu_text_input_poll(char *out_utf8, uint64_t size);
int dcpgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int dcpgpu_choice_poll(void);
/* The choice list reads PS4 pad button bits from this callback every frame while it is open. */
void dcpgpu_choice_set_input(uint32_t (*buttons)(void));
/* Whether the game's vibration reaches the controllers (the Options menu changes it live). */
int dcpgpu_controller_rumble(void);
void dcpgpu_set_controller_rumble(int enabled);
/* 1 while the settings menu is open: the game's pad input should be held neutral. */
int dcpgpu_overlay_captures_input(void);
/* DCP_INPUT_LATENCY_TRACE=1: 1 when set; the SDL_GetTicksNS time of the newest host button or key press. */
int dcpgpu_latency_trace_enabled(void);
uint64_t dcpgpu_latency_press_ns(void);
/* SteamOS Gamescope touchscreen (src/sos_touch_gamescope.h): up to two screen contacts as DualShock 4 touchpad points
 * (x 0..1919, y 0..942, arrays of 2) and the PS4 buttons a tap adds. Returns the contact count; 0 off that route. One
 * call per game read of player 1. */
int dcpgpu_touchscreen_sample(uint16_t *x, uint16_t *y, uint8_t *id, uint32_t *buttons);
/* Gives each Gamescope root its touch click mode back. Every exit path calls it before the process ends; any thread. */
void dcpgpu_touch_release(void);

/* 1 while the final presenter should black out the ultrawide side extensions for the frame being built: the real
 * loading/saving fade is drawn, or the fixed-screen transition fallback is active. Set once per guest frame. */
void dcpgpu_ultrawide_side_mask(int wanted);
/* One title menu update (the menu cursor, -1 when the scene is not the title's); presenter telemetry. */
void dcpgpu_debug_title_tick(int cursor);
#ifdef __cplusplus
}
#endif
#endif
