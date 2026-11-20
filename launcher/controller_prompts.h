/* Controller prompts: which button symbols the game shows. The game keeps its current controller type in
 * InputOverseer.m_currentControllerUsed (GamepadType: 1 XboxOne, 2 XboxSeries, 3 PS4, 4 PS5) and picks its icon map,
 * text-sprite atlas, layout offsets and UI modules from it.
 *
 * PlayStation leaves the game's code as it is. Xbox changes every place of the build that writes the type (the constructor
 * and three DualShock detections store XboxSeries) and every place that reads it, so no later detection can bring the
 * PlayStation symbols back; the sites of each build are in controller_prompts_sites.h. Automatic leaves those sites alone and
 * hooks InputOverseer.ResolveIconsForCurrentInput: while the physical controller is an Xbox one and the game takes its input
 * from a gamepad, the method sees XboxSeries, and the game's own value is put back as soon as that stops being true. */
#ifndef SOS_CONTROLLER_PROMPTS_H
#define SOS_CONTROLLER_PROMPTS_H

#include <stddef.h>
#include <stdint.h>

#include "sos_pad_kind.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOS_PROMPTS_KEY "controller_prompts"
#define SOS_PROMPTS_AUTOMATIC_KEY "automatic"
#define SOS_PROMPTS_PLAYSTATION_KEY "playstation"
#define SOS_PROMPTS_XBOX_KEY "xbox"
#define SOS_PROMPTS_DEFAULT_KEY SOS_PROMPTS_AUTOMATIC_KEY

typedef enum { SOS_PROMPTS_PLAYSTATION = 0, SOS_PROMPTS_XBOX = 1, SOS_PROMPTS_AUTOMATIC = 2 } SosPromptStyle;

/* 1 and the style for a saved name; 0 for anything else. `style` may be NULL. */
int sos_prompt_style_parse(const char *name, int *style);
const char *sos_prompt_style_label(int style);

/* 1 when the build ("CUSA57767-1.00") has verified sites. */
int sos_controller_prompts_supported(const char *build_key);

/* The style the settings file holds; Automatic when the file, the line or the value is missing or invalid. */
int sos_controller_prompts_from_file(const char *settings_file);

/* 1 when Xbox symbols apply: the Xbox style, or the Automatic style with an Xbox controller. */
int sos_prompt_uses_xbox(int style, int pad_kind);

/* The style the verified sites hold now; -1 when the build has none, the bytes are not the verified code or the sites are
 * only partly patched. */
int sos_controller_prompts_state(const uint8_t *image, uint64_t size, const char *build_key);

/* Makes the sites hold `style` (Xbox or PlayStation; Automatic leaves them as the game has them). Returns the style they hold
 * afterwards, -1 when nothing was written because the build has no verified sites or the bytes are not the verified code;
 * `message` says what happened, for the log. */
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

/* The method the Automatic style hooks: its offset, its length up to the next method, the FNV-1a of those bytes, its first
 * bytes and the InputOverseer field offsets. */
typedef struct {
    uint32_t method;
    uint32_t length;
    uint64_t hash;
    uint8_t prologue[8];
    uint32_t input_device;
    uint32_t controller_used;
} SosPromptHook;

#define SOS_PROMPT_HOOK_STOLEN 6
#define SOS_PROMPT_HOOK_STUB_BYTES 96

/* The hook of a build; NULL when the build has none. */
const SosPromptHook *sos_controller_prompts_hook(const char *build_key);

/* The game's own value of the controller type while the Xbox override holds, and whether it holds. */
typedef struct {
    int replaced;
    int32_t game_value;
} SosPromptOverride;

/* One call of the hooked method, with the InputOverseer's input device and controller type: sets the controller type to
 * XboxSeries while `xbox` holds and the input device is a gamepad, and puts the game's own value back when that stops
 * holding. Nothing else is read or written. */
void sos_prompt_override_step(SosPromptOverride *state, int xbox, int32_t input_device, int32_t *controller_used);

/* 1 when the method is the verified code (every byte, not only its start); else 0 with `why` naming the check. */
int sos_prompt_hook_verify(const uint8_t *image, uint64_t size, const char *build_key, const char **why);

/* Verifies, then writes the hook: a jump over the method's first 6 bytes to a stub in `stub`, which calls `handler` (System V)
 * with the InputOverseer in rdi, replays the 6 bytes and returns to the method. Writes nothing and returns 0, saying why, when
 * the build or its code is not the verified one or `stub` is out of reach of the image. */
int sos_prompt_hook_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stub, size_t stub_capacity,
                            char *message, size_t message_size);

/* Sets the style the hook follows and the offsets of the build it was installed for. */
void sos_prompt_hook_arm(int style, const char *build_key);

/* What the stub calls: the hook's work on the InputOverseer in `overseer`. */
__attribute__((sysv_abi)) void sos_prompt_hook_handler(uint8_t *overseer);

#ifdef __cplusplus
}
#endif

#endif
