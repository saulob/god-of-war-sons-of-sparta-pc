#ifndef SOS_HERMES_UNLOCK_H
#define SOS_HERMES_UNLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOS_HERMES_UNLOCK_KEY "unlock_hermes_mode"

/* Hermes Mode is the game's speedrun mode. MainMenuState_GameModeSelection.CheckSpeedRunModePossible shows its
 * existing button only when the profile has beaten the game or the cheat code set isHermesModeUnlocked. The site is
 * that method's branch to the hidden-button path. */
typedef struct {
    const char *build;
    uint64_t image_size;
    uint32_t gate;
    uint32_t hide;
} SosHermesUnlockSite;

const SosHermesUnlockSite *sos_hermes_unlock_site(const char *build);
int sos_hermes_unlock_verify(const uint8_t *image, uint64_t size, const SosHermesUnlockSite *site, const char **why);
int sos_hermes_unlock_apply(uint8_t *image, uint64_t size, const char *build, char *message, size_t message_size);
/* 1 when the settings file leaves unlock_hermes_mode on; a missing file or key means off. */
int sos_hermes_unlock_from_file(const char *settings_file);

#ifdef __cplusplus
}
#endif

#endif
