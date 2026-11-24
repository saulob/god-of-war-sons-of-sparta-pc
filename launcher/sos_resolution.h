/* The render resolutions and aspect ratios the launcher offers, the render target they give the game, and the verified
 * sites of the game's executable image that carry that size. */
#ifndef SOS_RESOLUTION_H
#define SOS_RESOLUTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;  /* render_resolution in sons_of_sparta.ini */
    const char *name; /* launcher label */
    int width, height;
} SosResolution;

#define SOS_RESOLUTION_COUNT 5
extern const SosResolution sos_resolutions[SOS_RESOLUTION_COUNT];

/* Index of the key, 0 (native) when it is not listed. */
int sos_resolution_index(const char *key);

typedef struct {
    const char *key;
    const char *name;
    int width, height;
} SosAspectRatio;

#define SOS_ASPECT_COUNT 6
extern const SosAspectRatio sos_aspect_ratios[SOS_ASPECT_COUNT];

/* Index of the key, 0 (16:9 Original) when it is not listed. */
int sos_aspect_index(const char *key);

/* What the game renders at. `neo` is the game's PS4 Pro mode, which its screen manager needs for widths above 1920;
 * `display_*` is the display the video out then reports. */
typedef struct {
    int width, height;
    int neo;
    int display_width, display_height;
} SosRenderTarget;

SosRenderTarget sos_native_render_target(void);
SosRenderTarget sos_render_target(const SosResolution *resolution, const SosAspectRatio *aspect);
int sos_render_target_is_native(const SosRenderTarget *target);

/* The target's width over its height, relative to 16:9. The game's cameras are orthographic and take their aspect from
 * the screen, so this is how much wider the world view is at the same vertical view: 1 for 16:9. */
double sos_render_target_widening(const SosRenderTarget *target);
int sos_render_target_wider_than_16_9(const SosRenderTarget *target);

/* The direct memory the runtime must offer besides its retail pool for the larger render targets. */
uint64_t sos_budget_growth(const SosRenderTarget *target);

typedef enum {
    SOS_SITE_WIDTH,          /* two ints: the player settings' video out width and base mode width */
    SOS_SITE_HEIGHT_CALC,    /* the first request's height, 9/16 of the width */
    SOS_SITE_HEIGHT_CHECK,   /* SetResolution's refusal of a height that is not 9/16 of the width */
    SOS_SITE_SET_RESOLUTION  /* the game's own Screen.SetResolution(1920, 1080), run once its settings load */
} SosSiteKind;

/* Every target needs all of them: one left at 1920 x 1080 puts the game back at 1080p 16:9. */
#define SOS_SET_RESOLUTION_SITE_COUNT 7

#define SOS_SITE_MAX_BYTES 24

typedef struct {
    uint32_t offset;                       /* image offset */
    uint8_t length;
    uint8_t kind;                          /* SosSiteKind */
    uint8_t original[SOS_SITE_MAX_BYTES];  /* what the executable holds there before the patch */
} SosResolutionSite;

/* The sites a target changes: all of them for a shape other than 16:9, the width and SetResolution sites otherwise. */
int sos_target_needs_height_sites(const SosRenderTarget *target);

/* Checks every site the target needs still holds its original bytes, then writes the target. Returns 0 and a message in
 * `error` when the image does not match; nothing is written then. */
int sos_apply_render_target(unsigned char *image, uint64_t image_size, const SosResolutionSite *sites, size_t count,
                            const SosRenderTarget *target, char *error, size_t error_size);

/* The render target the settings file asks for, among the resolutions the build allows: native when the file or a value
 * is missing, or `supported` is 0. `resolution` and `aspect` receive the choices' indices. */
SosRenderTarget sos_render_target_from_settings(const char *settings_file, int supported, int *resolution, int *aspect);

/* The boot splash plays a 1920 x 1080 video texture on a RawImage stretched over its canvas, which is scaled by the
 * screen height. On a target wider than 16:9 the guest reads the RawImage as a centred 1920 x 1080 instead, so the video
 * keeps its shape over the splash's black backdrop. */
typedef struct {
    const char *guest_path;
    uint64_t file_length, offset;
    unsigned char original[32], centred[32];
} SosSplashPatch;

extern const SosSplashPatch sos_splash_patch;

/* `bytes` are the 32 bytes at the patch's offset of the guest's file, `file_length` its size. */
int sos_splash_patch_matches(const unsigned char *bytes, uint64_t file_length);

#ifdef __cplusplus
}
#endif

#endif
