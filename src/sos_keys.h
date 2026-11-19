/* Keyboard bindings: the actions, their defaults and the saved key names, shared by the launcher, the runtime pad
 * and the video core. Keys are SDL scancodes, saved as the shadPS4 names (space, lshift, f10, pgup...) or SDL's own
 * lowercase names for the rest. */
#ifndef SOS_KEYS_H
#define SOS_KEYS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOS_KEY_LEFT_STICK_UP, SOS_KEY_LEFT_STICK_DOWN, SOS_KEY_LEFT_STICK_LEFT, SOS_KEY_LEFT_STICK_RIGHT,
    SOS_KEY_RIGHT_STICK_UP, SOS_KEY_RIGHT_STICK_DOWN, SOS_KEY_RIGHT_STICK_LEFT, SOS_KEY_RIGHT_STICK_RIGHT,
    SOS_KEY_CROSS, SOS_KEY_CIRCLE, SOS_KEY_SQUARE, SOS_KEY_TRIANGLE,
    SOS_KEY_L1, SOS_KEY_R1, SOS_KEY_L2, SOS_KEY_R2, SOS_KEY_L3, SOS_KEY_R3,
    SOS_KEY_DPAD_UP, SOS_KEY_DPAD_DOWN, SOS_KEY_DPAD_LEFT, SOS_KEY_DPAD_RIGHT,
    SOS_KEY_OPTIONS, SOS_KEY_TOUCHPAD_LEFT, SOS_KEY_TOUCHPAD_RIGHT,
    SOS_KEY_HOTKEY_WINDOW_MODE,
    SOS_KEY_COUNT
} SosKeyAction;

#define SOS_KEY_FIRST_HOTKEY SOS_KEY_HOTKEY_WINDOW_MODE

typedef struct {
    const char *ini_key; /* the line in sons_of_sparta.ini */
    const char *label;   /* the launcher's row */
    int default_scancode;
} SosKeyInfo;

extern const SosKeyInfo sos_key_actions[SOS_KEY_COUNT];

typedef struct { int scancode[SOS_KEY_COUNT]; } SosKeyMap;

void sos_keys_defaults(SosKeyMap *map);

/* The scancode a saved name stands for (case-insensitive), or 0 when it names no bindable key. */
int sos_key_from_name(const char *name);
/* False for no key and for Escape, which cancels a capture. */
int sos_key_bindable(int scancode);
/* The name saved for a scancode, written to `buffer`; empty when the key is not bindable. */
const char *sos_key_name(int scancode, char *buffer, size_t size);

/* Applies one saved line to the map; false when the key is not an action or the value names no bindable key. */
int sos_keys_apply(SosKeyMap *map, const char *ini_key, const char *value);
/* Defaults, then every valid binding in the sons_of_sparta.ini at `path`; a missing file or line keeps the default. */
void sos_keys_load(const char *path, SosKeyMap *map);

#ifdef __cplusplus
}
#endif

#endif
