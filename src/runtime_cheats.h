/* The cheats in the game process: the hooks on the loaded image, the launcher's saved values with this session's F10
 * changes over them, and the item actions. The F10 menu (video core, window thread) and the hooks (game thread) share it. */
#ifndef SOS_RUNTIME_CHEATS_H
#define SOS_RUNTIME_CHEATS_H

#include <stddef.h>
#include <stdint.h>

#include "sos_cheat_patch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Row states the menu shows (sos::cheats::RowState). */
enum { SOS_CHEAT_UNAVAILABLE, SOS_CHEAT_OFF, SOS_CHEAT_ON, SOS_CHEAT_PENDING, SOS_CHEAT_FAILED };

typedef struct {
    uint8_t has_value, value;
} SosNullableBool;

typedef struct {
    float x, y;
} SosVector2;

/* The game functions the handlers call. Resolved from the verified image; tests supply their own. */
typedef struct {
    void *(SOS_CHEAT_SYSV *get_game_object)(void *component, const void *method);
    uint8_t(SOS_CHEAT_SYSV *is_player)(void *game_object, const void *method);
    void *(SOS_CHEAT_SYSV *get_pawn)(void *controller, const void *method);
    int32_t(SOS_CHEAT_SYSV *player_index)(void *controller, const void *method);
    void *(SOS_CHEAT_SYSV *try_get_wallet)(void *game_object, const void *method);
    void *(SOS_CHEAT_SYSV *item_by_id)(void *id, const void *method);
    int64_t(SOS_CHEAT_SYSV *wallet_amount)(void *wallet, void *item, const void *method);
    void(SOS_CHEAT_SYSV *wallet_add)(void *wallet, void *item, int64_t count, SosNullableBool hide, const void *method);
    SosVector2(SOS_CHEAT_SYSV *slope_aligned)(void *movement, SosVector2 delta, const void *method);
    void *(SOS_CHEAT_SYSV *string_new)(const char *text);
} SosCheatBindings;

/* Before the image is made executable: verifies each cheat's sites and functions on the image of `build_key` and builds
 * their stubs. A cheat whose code is not the analysed one is Unavailable; nothing is written to the game code here. */
void runtime_cheats_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stubs, size_t stubs_size,
                            char *message, size_t message_size);
/* The same with a given table and bindings (tests). */
void runtime_cheats_install_build(uint8_t *image, uint64_t size, const SosCheatBuild *build, uint8_t *stubs,
                                  size_t stubs_size, const SosCheatBindings *bindings, char *message, size_t message_size);

/* The start of a game session: every F10 override is cleared and the saved values of the settings file apply. */
void runtime_cheats_start(const char *settings_file);
/* The same from the settings text (tests). */
void runtime_cheats_start_text(const char *settings_text);

/* The launcher's saved value this session started from; F10 never changes it. */
int runtime_cheat_saved_toggle(int toggle);
int runtime_cheat_saved_multiplier(int multiplier);

int runtime_cheat_toggle_state(int toggle);
int runtime_cheat_multiplier_state(int multiplier);
int runtime_cheat_multiplier_value(int multiplier);
int runtime_cheat_action_state(int action);
/* "+1000", "+10", "Busy" or "Unavailable" for two seconds after the action, else NULL. */
const char *runtime_cheat_action_notice(int action);

/* F10 changes, for this session only: nothing is written to the settings file. */
void runtime_cheat_toggle(int toggle);
void runtime_cheat_cycle(int multiplier);
void runtime_cheat_action(int action);

/* Once per presented frame: gives up an item action no player took in time. */
void runtime_cheats_frame(void);

/* 1 once a cheat was really turned On or an item was added in this process; never cleared. */
int runtime_cheats_used(void);
/* 1 when any cheat's hooks were recognised on this build. */
int runtime_cheats_available(void);

#ifdef __cplusplus
}
#endif

#endif
