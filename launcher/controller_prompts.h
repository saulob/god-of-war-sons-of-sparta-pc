/* Controller prompts: which button symbols the game shows. The game keeps its current controller type in
 * InputOverseer.m_currentControllerUsed (GamepadType) and picks its icons, text sprites and UI events from it. The type
 * is stored in four places: the constructor's default and three DualShock detections. PlayStation keeps the original
 * values (4 and 3); Xbox stores XboxSeries (2) in all four. */
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

/* The style the verified sites hold now; -1 when the build has none or the bytes are not the verified code. */
int sos_controller_prompts_state(const uint8_t *image, uint64_t size, const char *build_key);

/* Makes the four sites hold `style`. Returns the style they hold afterwards, -1 when nothing was written because the
 * build has no verified sites or the bytes are not the verified code; `message` says what happened, for the log. */
int sos_controller_prompts_apply(uint8_t *image, uint64_t size, const char *build_key, int style, char *message,
                                 size_t message_size);

#ifdef __cplusplus
}
#endif

#endif
