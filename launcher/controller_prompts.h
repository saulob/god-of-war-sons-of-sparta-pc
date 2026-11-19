/* Controller prompts: which button symbols the game shows. The game keeps its current controller type in
 * InputOverseer.m_currentControllerUsed (GamepadType: 1 XboxOne, 2 XboxSeries, 3 PS4, 4 PS5) and picks its icon map,
 * text-sprite atlas, layout offsets and UI modules from it. PlayStation leaves the game's code as it is. Xbox changes every
 * place of the build that writes the type (the constructor and three DualShock detections store XboxSeries) and every place
 * that reads it (the atlas and icon map choice, the text builder, the UI module listener, the keybind layout), so no later
 * detection can bring the PlayStation symbols back. The sites of each build are in controller_prompts_sites.h. */
#ifndef SOS_CONTROLLER_PROMPTS_H
#define SOS_CONTROLLER_PROMPTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOS_PROMPTS_KEY "controller_prompts"
#define SOS_PROMPTS_PLAYSTATION_KEY "playstation"
#define SOS_PROMPTS_XBOX_KEY "xbox"
#define SOS_PROMPTS_DEFAULT_KEY SOS_PROMPTS_PLAYSTATION_KEY

typedef enum { SOS_PROMPTS_PLAYSTATION = 0, SOS_PROMPTS_XBOX = 1 } SosPromptStyle;

/* 1 and the style for a saved name; 0 for anything else. `style` may be NULL. */
int sos_prompt_style_parse(const char *name, int *style);
const char *sos_prompt_style_label(int style);

/* 1 when the build ("CUSA57767-1.00") has verified sites. */
int sos_controller_prompts_supported(const char *build_key);

/* The style the settings file holds; PlayStation when the file, the line or the value is missing or invalid. */
int sos_controller_prompts_from_file(const char *settings_file);

/* The style the verified sites hold now; -1 when the build has none, the bytes are not the verified code or the sites are
 * only partly patched. */
int sos_controller_prompts_state(const uint8_t *image, uint64_t size, const char *build_key);

/* Makes the sites hold `style`. Returns the style they hold afterwards, -1 when nothing was written because the
 * build has no verified sites or the bytes are not the verified code; `message` says what happened, for the log. */
int sos_controller_prompts_apply(uint8_t *image, uint64_t size, const char *build_key, int style, char *message,
                                 size_t message_size);

/* One place of the game code that a style changes: the bytes around it identify the verified code. */
typedef struct {
    uint32_t offset;
    uint8_t length;
    uint8_t immediate; /* the bytes are part of an instruction's immediate value */
    uint8_t before[8];
    uint8_t original[4];
    uint8_t xbox[4];
    uint8_t after[8];
} SosPromptSite;

/* The sites of a build, in table order; NULL when the build has none. */
const SosPromptSite *sos_controller_prompts_sites(const char *build_key, size_t *count);

/* The image ranges a style changes that are plain immediates (the red-zone analysis can ignore their value), whichever style
 * they hold; the number written to `offsets` and `lengths` (at most `capacity`), 0 when the bytes are not the verified code. */
size_t sos_controller_prompts_value_ranges(const uint8_t *image, uint64_t size, const char *build_key, uint64_t *offsets,
                                           uint8_t *lengths, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
