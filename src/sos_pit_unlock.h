#ifndef SOS_PIT_UNLOCK_H
#define SOS_PIT_UNLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOS_PIT_UNLOCK_KEY "unlock_the_pit"

/* The title menu raises the event that shows its own ThePitButton when the profile's isPitUnlocked is set. `gate` is the
 * branch over that event and `invoke` is UnityEvent.Invoke. No save data is changed. */
typedef struct {
    const char *build;
    uint64_t image_size;
    uint32_t gate;
    uint32_t invoke;
} SosPitUnlockSite;

/* The code the site must hold: `bytes` (-1 for a byte that is not checked), the branch the patch removes at `jump` and the two
 * UnityEvent.Invoke calls at `pit_call` and `hermes_call`, all as offsets from the start of the check. */
typedef struct {
    const short *bytes;
    int length, jump, pit_call, hermes_call;
} SosPitUnlockPattern;

const SosPitUnlockPattern *sos_pit_unlock_pattern(void);
const SosPitUnlockSite *sos_pit_unlock_site(const char *build);
int sos_pit_unlock_verify(const uint8_t *image, uint64_t size, const SosPitUnlockSite *site, const char **why);
/* 1 when every Pit scene file is in the game folder; otherwise `missing` names the first absent one. */
int sos_pit_unlock_content(const char *game_folder, char *missing, size_t missing_size);
int sos_pit_unlock_apply(uint8_t *image, uint64_t size, const char *build, const char *game_folder, char *message,
                         size_t message_size);
/* 1 when the settings file leaves unlock_the_pit on; a missing file or key means off. */
int sos_pit_unlock_from_file(const char *settings_file);

#ifdef __cplusplus
}
#endif

#endif
