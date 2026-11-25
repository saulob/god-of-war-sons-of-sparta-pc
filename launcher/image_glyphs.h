/* Glyph Images that AutoButtonSprite does not follow (the L2/R2 of the pause, vendor and Pit tabs and similar). While Xbox prompts
 * apply, an enabled Image whose sprite is a known PlayStation glyph shows the game's XboxSeries sprite of the same input as its
 * overrideSprite; its own sprite is never written. Automatic also corrects the text atlas, which the game reads before the icon
 * lookup that the prompt hook covers. */
#ifndef SOS_IMAGE_GLYPHS_H
#define SOS_IMAGE_GLYPHS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GamepadIcons sprite order. */
typedef enum {
    SOS_GLYPH_SOUTH = 0, SOS_GLYPH_NORTH, SOS_GLYPH_EAST, SOS_GLYPH_WEST, SOS_GLYPH_START, SOS_GLYPH_SELECT,
    SOS_GLYPH_LEFT_TRIGGER, SOS_GLYPH_RIGHT_TRIGGER, SOS_GLYPH_LEFT_SHOULDER, SOS_GLYPH_RIGHT_SHOULDER,
    SOS_GLYPH_LEFT_STICK_PRESS = 17, SOS_GLYPH_RIGHT_STICK_PRESS, SOS_GLYPH_TOUCHPAD,
    SOS_GLYPH_NONE = -1
} SosGlyph;

/* The input a sprite of the game's PlayStation prompt sheets shows, from its name; SOS_GLYPH_NONE for every other sprite. D-pad and
 * stick sprites are not mapped: they mean the same on both controllers. */
SosGlyph sos_image_glyph_of_name(const uint16_t *name, int32_t length);

enum { SOS_GLYPH_HOOK_ENABLE, SOS_GLYPH_HOOK_DISABLE, SOS_GLYPH_HOOK_SET_SPRITE, SOS_GLYPH_HOOK_TEXT_ATLAS, SOS_GLYPH_HOOKS };
#define SOS_GLYPH_HOOK_STOLEN 6
#define SOS_GLYPH_STUB_SLOT 64
#define SOS_IMAGE_GLYPHS_STUB_BYTES (SOS_GLYPH_HOOKS * SOS_GLYPH_STUB_SLOT)
#define SOS_GLYPH_SERVICES 5

typedef struct {
    uint32_t method, length;
    uint64_t hash;
    uint8_t entry[SOS_GLYPH_HOOK_STOLEN];
} SosGlyphHook;

typedef struct {
    uint32_t begin, end;
} SosGlyphRange;

typedef struct {
    SosGlyphHook hooks[SOS_GLYPH_HOOKS];
    uint32_t get_instance_id, get_name, is_active, set_override_sprite, get_overseer;
    SosGlyphRange services[SOS_GLYPH_SERVICES];
    uint64_t services_hash;
    uint32_t overseer_slot, runtime_slot;
    uint32_t sprite, override_sprite, icon_data, input_device, xbox_icons;
} SosImageGlyphSite;

const SosImageGlyphSite *sos_image_glyphs_site(const char *build_key);

/* 1 when every method the style hooks and every function the handlers call are the verified code; else 0 with `why`. */
int sos_image_glyphs_verify(const uint8_t *image, uint64_t size, const char *build_key, int style, const char **why);

/* Verifies everything, then writes the hooks of `style` (Xbox: the three Image hooks; Automatic: all four; PlayStation: none).
 * Writes nothing and returns 0, saying why, when any check fails or `stubs` is out of reach of the image. */
int sos_image_glyphs_install(uint8_t *image, uint64_t size, const char *build_key, int style, uint8_t *stubs, size_t stubs_capacity,
                             char *message, size_t message_size);

#define SOS_IMAGE_GLYPHS_SYSV __attribute__((sysv_abi))

typedef struct {
    int32_t(SOS_IMAGE_GLYPHS_SYSV *get_instance_id)(void *object, const void *method);
    void *(SOS_IMAGE_GLYPHS_SYSV *get_name)(void *object, const void *method);
    uint8_t(SOS_IMAGE_GLYPHS_SYSV *is_active)(void *behaviour, const void *method);
    void(SOS_IMAGE_GLYPHS_SYSV *set_override_sprite)(void *image, void *sprite, const void *method);
    void *(SOS_IMAGE_GLYPHS_SYSV *get_overseer)(const void *method);
    const uintptr_t *overseer_slot, *runtime_slot;
} SosImageGlyphGame;

void sos_image_glyphs_arm(int style, const uint8_t *image, const char *build_key);
void sos_image_glyphs_arm_with(int style, const SosImageGlyphSite *site, const SosImageGlyphGame *game);

/* What the stubs call, on the game thread, before the hooked method runs. */
SOS_IMAGE_GLYPHS_SYSV void sos_image_glyphs_on_enable(void *image);
SOS_IMAGE_GLYPHS_SYSV void sos_image_glyphs_on_disable(void *image);
SOS_IMAGE_GLYPHS_SYSV void sos_image_glyphs_on_set_sprite(void *image, void *sprite);

#ifdef __cplusplus
}
#endif

#endif
