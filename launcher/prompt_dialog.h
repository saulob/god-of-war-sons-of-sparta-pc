/* Settings confirmation dialog and campfire menu glyphs. The footer shows its button symbols through AutoButtonSprite and follows the controller
 * prompts, but the Settings dialogs (Quit Game, Reset to Default, Unsaved Changes, Restart Run, Return to Pit Entrance) show
 * two plain UnityEngine.UI.Image glyphs serialized with the PlayStation sprites. PromptTextSwitch.UpdatePromptDescriptions runs
 * whenever such a dialog opens; the hook lets it run, then sets Image.overrideSprite of the Confirm and Back glyphs to the
 * game's own XboxSeries South and East sprites while Xbox prompts apply, and clears it otherwise.
 *
 * The campfire menu (SaveMiniMenu) and the pit player menu (PitPlayerSelectMiniMenu) show the same kind of plain Image prompts:
 * their confirm glyphs are serialized with the PlayStation Cross sprite and only the circle-confirm regions change them. Their
 * InitializeMiniMenu is hooked the same way; its Images get the XboxSeries South sprite, the confirm action, and lose it again
 * when Xbox prompts stop applying. */
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

/* A menu whose InitializeMiniMenu is hooked: the method, its length up to the next method, the FNV-1a of those bytes, its first
 * bytes (whole instructions, none relative) and the offsets of the Image fields that show its confirm glyphs. */
#define SOS_PROMPT_MENUS 2
#define SOS_PROMPT_MENU_STOLEN 6
#define SOS_PROMPT_MENU_IMAGES 4
#define SOS_PROMPT_MENU_STUB_BYTES 96
typedef struct {
    uint32_t method, length;
    uint64_t hash;
    uint8_t entry[SOS_PROMPT_MENU_STOLEN];
    uint32_t image_count;
    uint32_t images[SOS_PROMPT_MENU_IMAGES];
} SosPromptMenu;

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
    uint64_t services; /* FNV-1a of the code of get_overseer and set_override_sprite, which the menu handlers call */
    SosPromptMenu menus[SOS_PROMPT_MENUS];
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

/* 1 when the hooked method of menu `index` and every function its handler calls are the verified code; else 0 with `why`. */
int sos_prompt_menu_verify(const uint8_t *image, uint64_t size, const char *build_key, int index, const char **why);

/* Verifies, then writes the hook of menu `index`: a jump over the first 6 bytes of its InitializeMiniMenu to a stub in `stub`, which
 * calls the menu's handler (System V) with the menu in rdi, replays the 6 bytes and returns to the method. Writes nothing and
 * returns 0, saying why, when the build or its code is not the verified one or `stub` is out of reach of the image. */
int sos_prompt_menu_install(uint8_t *image, uint64_t size, const char *build_key, int index, uint8_t *stub, size_t stub_capacity,
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

/* What the stubs of the campfire menu (0) and the pit player menu (1) call: the confirm glyphs of the menu in `menu`. */
SOS_PROMPT_DIALOG_SYSV void sos_prompt_menu_handler_save(void *menu);
SOS_PROMPT_DIALOG_SYSV void sos_prompt_menu_handler_pit(void *menu);

#ifdef __cplusplus
}
#endif

#endif
