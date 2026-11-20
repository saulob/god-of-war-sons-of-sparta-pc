/* Settings confirmation dialog glyphs. The footer shows its button symbols through AutoButtonSprite and follows the controller
 * prompts, but the Settings dialogs (Quit Game, Reset to Default, Unsaved Changes, Restart Run, Return to Pit Entrance) show
 * two plain UnityEngine.UI.Image glyphs serialized with the PlayStation sprites. PromptTextSwitch.UpdatePromptDescriptions runs
 * whenever such a dialog opens; the hook lets it run, then sets Image.overrideSprite of the Confirm and Back glyphs to the
 * game's own XboxSeries South and East sprites while Xbox prompts apply, and clears it otherwise. */
#ifndef SOS_PROMPT_DIALOG_H
#define SOS_PROMPT_DIALOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t begin, end;
} SosPromptDialogRange;

#define SOS_PROMPT_DIALOG_RANGES 8
#define SOS_PROMPT_DIALOG_ENTRY_BYTES 18
#define SOS_PROMPT_DIALOG_STUB_BYTES 80

/* One build: the method, its entry bytes, the method infos and game functions the hook calls (image offsets), the code
 * ranges they hash to and the object layout. */
typedef struct {
    uint32_t method;
    uint8_t entry[SOS_PROMPT_DIALOG_ENTRY_BYTES];
    uint32_t image_slot, overseer_slot;
    uint32_t get_component, get_overseer, set_override_sprite;
    uint32_t transform_of, parent_of, child_count, child_at;
    SosPromptDialogRange ranges[SOS_PROMPT_DIALOG_RANGES];
    uint64_t hash;
    uint32_t text_list, icon_data, input_device, xbox_confirm, xbox_back, override_sprite;
} SosPromptDialogSite;

/* The site of a build ("CUSA57767-1.00"); NULL for any other build. */
const SosPromptDialogSite *sos_prompt_dialog_site(const char *build_key);

/* 1 when the method and every function the hook calls are the verified code (every byte of their ranges); else 0 with `why`. */
int sos_prompt_dialog_verify(const uint8_t *image, uint64_t size, const char *build_key, const char **why);

/* Verifies, then writes the hook: a jump over the method's first 18 bytes to a stub in `stub`, which calls the handler (System V)
 * with the PromptTextSwitch in rdi, then replays the 18 bytes (the flag test rebuilt with an absolute address) and returns to
 * the method. Writes nothing and returns 0, saying why, when the build or its code is not the verified one or `stub` is out
 * of reach of the image. */
int sos_prompt_dialog_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stub, size_t stub_capacity,
                              char *message, size_t message_size);

#define SOS_PROMPT_DIALOG_SYSV __attribute__((sysv_abi))

/* The game functions and method info slots the handler uses. */
typedef struct {
    void *(SOS_PROMPT_DIALOG_SYSV *get_component)(void *self, const void *method);
    void *(SOS_PROMPT_DIALOG_SYSV *get_overseer)(const void *method);
    void (SOS_PROMPT_DIALOG_SYSV *set_override_sprite)(void *image, void *sprite, const void *method);
    void *(SOS_PROMPT_DIALOG_SYSV *transform_of)(void *component, const void *method);
    void *(SOS_PROMPT_DIALOG_SYSV *parent_of)(void *transform, const void *method);
    int (SOS_PROMPT_DIALOG_SYSV *child_count)(void *transform, const void *method);
    void *(SOS_PROMPT_DIALOG_SYSV *child_at)(void *transform, int index, const void *method);
    const uintptr_t *image_slot, *overseer_slot;
} SosPromptDialogGame;

/* Sets the prompt style the handler follows and the game it calls: the functions of the build in the image at `image`. */
void sos_prompt_dialog_arm(int style, const uint8_t *image, const char *build_key);

/* The same with the functions and layout given; `site` and `game` are copied. */
void sos_prompt_dialog_arm_with(int style, const SosPromptDialogSite *site, const SosPromptDialogGame *game);

/* What the stub calls: the glyphs of the PromptTextSwitch in `prompt`. */
SOS_PROMPT_DIALOG_SYSV void sos_prompt_dialog_handler(void *prompt);

#ifdef __cplusplus
}
#endif

#endif
