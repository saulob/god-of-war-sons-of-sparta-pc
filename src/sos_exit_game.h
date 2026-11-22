#ifndef SOS_EXIT_GAME_H
#define SOS_EXIT_GAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOS_EXIT_GAME_RANGES 9
#define SOS_EXIT_GAME_TOGGLE_STOLEN 7
#define SOS_EXIT_GAME_CONFIRM_STOLEN 10
#define SOS_EXIT_GAME_QUIT_ENTRY 11
#define SOS_EXIT_GAME_TEXT_STOLEN 7
#define SOS_EXIT_GAME_STUB_SLOT 96
#define SOS_EXIT_GAME_STUB_BYTES (4 * SOS_EXIT_GAME_STUB_SLOT)
#define SOS_EXIT_GAME_LANGUAGE_COUNT 23

#define SOS_EXIT_GAME_SYSV __attribute__((sysv_abi))

typedef struct {
    uint32_t begin, end;
} SosExitGameRange;

typedef struct {
    uint32_t toggle_method, confirm_method, quit_method, text_method;
    uint8_t toggle_entry[SOS_EXIT_GAME_TOGGLE_STOLEN];
    uint8_t confirm_entry[SOS_EXIT_GAME_CONFIRM_STOLEN];
    uint8_t quit_entry[SOS_EXIT_GAME_QUIT_ENTRY];
    uint8_t text_entry[SOS_EXIT_GAME_TEXT_STOLEN];
    uint32_t get_game_object, get_transform, get_parent, get_name, alloc_string;
    uint32_t localization_class_slot;
    SosExitGameRange ranges[SOS_EXIT_GAME_RANGES];
    uint64_t hash;
    uint32_t native_object_offset, static_fields_offset, text_language_offset;
    uint32_t string_length_offset, string_chars_offset;
} SosExitGameSite;

typedef struct {
    void *(SOS_EXIT_GAME_SYSV *get_game_object)(void *component, const void *method);
    void *(SOS_EXIT_GAME_SYSV *get_transform)(void *component, const void *method);
    void *(SOS_EXIT_GAME_SYSV *get_parent)(void *transform, const void *method);
    void *(SOS_EXIT_GAME_SYSV *get_name)(void *object, const void *method);
    void *(SOS_EXIT_GAME_SYSV *alloc_string)(int32_t length, const void *method);
    const uintptr_t *localization_class_slot;
} SosExitGameBindings;

typedef int (*SosExitGameHostHandler)(void);

typedef struct {
    int option;
    const char *name;
    const char *original_9203;
    const char *text;
} SosExitGameLanguage;

const SosExitGameSite *sos_exit_game_site(const char *build_key);
int sos_exit_game_verify(const uint8_t *image, uint64_t size, const char *build_key, const char **why);
int sos_exit_game_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stubs, size_t stubs_capacity,
                          char *message, size_t message_size);

void sos_exit_game_arm(const uint8_t *image, const char *build_key, SosExitGameHostHandler host_exit);
void sos_exit_game_arm_with(const SosExitGameSite *site, const SosExitGameBindings *bindings,
                            SosExitGameHostHandler host_exit);
void sos_exit_game_reset(void);
int sos_exit_game_requested(void);

const SosExitGameLanguage *sos_exit_game_languages(size_t *count);
const char *sos_exit_game_translation(int language_option, const char **language_name);
const char *sos_exit_game_translation_for_original(const char *original_utf8, const char **language_name);

SOS_EXIT_GAME_SYSV int sos_exit_game_on_toggle(void *toggler);
SOS_EXIT_GAME_SYSV int sos_exit_game_on_confirm(void *anim_event);
SOS_EXIT_GAME_SYSV void sos_exit_game_on_quit_app(void);
SOS_EXIT_GAME_SYSV void *sos_exit_game_on_text(void *localize_event, void *value);

#ifdef __cplusplus
}
#endif

#endif
