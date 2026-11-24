/* Skip Intro: UiScripts.SplashScreen's start coroutine (<Start>d__24.MoveNext) is sent from its first state straight to
 * its own ending, so the Sony presentation, the startup video and its music never start. The ending still marks the game
 * as booted, saves, loads the FMOD banks and the next scene; it first waits for the save system and the settings
 * defaults, which the presentation used to outlast. The waits the presentation covered are shortened to 1 ms: the save
 * system's startup delay of 10 s and the 100 ms polls of the save loading code. The title intro is skipped once through the
 * game's own transition to PRESS START, when the intro has run a few ticks and the main menu is open and idle. */
#ifndef SOS_SKIP_INTRO_PATCH_H
#define SOS_SKIP_INTRO_PATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Offsets are image offsets, relative to the coroutine for the logical points of the patch. */
typedef struct {
    const char *build;      /* "CUSA57767-1.00" */
    uint64_t image_size;
    uint32_t coroutine;     /* <Start>d__24.MoveNext */
    uint32_t init_save_system; /* <InitSaveSystem>d__44.MoveNext */
    uint32_t task_delay;    /* System.Threading.Tasks.Task.Delay, the overload the save system awaits */
    uint32_t task_delay_other; /* the other Task.Delay overload that takes milliseconds */
    uint32_t load_save_task; /* PS4SaveTasks.<LoadSaveTask>d__44.MoveNext */
    uint32_t check_file_exist; /* PS4SaveTasks.<CheckFileExist>d__6.MoveNext */
    uint32_t title_check;   /* MainMenuState_Intro.TickState: cmp byte [r14+0x6c],0, the queued switch to the next state */
    uint32_t base_tick;     /* MainMenuState.TickState, called just before */
} SosSkipIntroSite;

/* 1 when the settings file turns skip_intro on. */
int sos_skip_intro_from_file(const char *settings_file);

/* The verified site of a build key ("CUSA57767-1.00"); NULL for any other build. */
const SosSkipIntroSite *sos_skip_intro_site(const char *build);

/* 1 when the coroutine and every point the patch uses are the verified code; else 0 with *why naming the check. */
int sos_skip_intro_verify(const uint8_t *image, uint64_t size, const SosSkipIntroSite *site, const char **why);

#define SOS_SKIP_INTRO_STUB_BYTES 64
#define SOS_SKIP_INTRO_MIN_TICKS 3

/* Points the title intro's queued-switch test at a stub in `stubs` that calls sos_skip_intro_claim; the stub sets the
 * game's own switch flag at `check` (cmp byte [r14+0x6c],0) when the claim is granted. 0 and nothing written when the
 * check is another or the stub is out of reach. */
int sos_skip_intro_hook_title(uint8_t *check, uint8_t *stubs);

/* 1 the first time after sos_skip_intro_arm that the intro state (the stub passes its TickState's `this`) has ticked
 * SOS_SKIP_INTRO_MIN_TICKS times and its main menu is open and not processing the load button; else 0. Called by the stub
 * (System V). */
void sos_skip_intro_arm(void);
__attribute__((sysv_abi)) int sos_skip_intro_claim(const uint8_t *state);

#if defined(_WIN32)
#define SOS_SKIP_INTRO_SYSV __attribute__((sysv_abi))
#else
#define SOS_SKIP_INTRO_SYSV
#endif
typedef void (SOS_SKIP_INTRO_SYSV *SosSkipIntroCancel)(void *component);

/* The title presentation is chained through ExecuteEventOnAnimationFinished watchers, which only test that the
 * Animator's current clip reached its end. Skip Intro moves the state machine to PRESS ANY BUTTON before that chain is
 * done, so dismissing the prompt must cancel the watchers still waiting on it; otherwise the next clip on the same
 * Animator completes them and replays the prompt (and its copyright) over the main menu. */
void sos_skip_intro_arm_title(uint32_t native_object_offset, SosSkipIntroCancel cancel);
int sos_skip_intro_watching_title(void);
void sos_skip_intro_watch_title_animation(void *component);
void sos_skip_intro_dismiss_title(void);

/* Verifies, then writes the patch and arms the title claim. Writes nothing and returns 0, saying why, when the build or its
 * code is not the verified one. `stubs` has SOS_SKIP_INTRO_STUB_BYTES within a 32-bit jump of the image. */
int sos_skip_intro_apply(uint8_t *image, uint64_t size, const char *build, uint8_t *stubs, size_t stub_capacity, char *message,
                         size_t message_size);

#ifdef __cplusplus
}
#endif

#endif
