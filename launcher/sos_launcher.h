/* The launcher, called by the loader before the guest and the game window exist. */
#ifndef SOS_LAUNCHER_H
#define SOS_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#include "sos_resolution.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the processor and graphics system can run the game. Otherwise 0, with what is wrong and what to do about it in
 * the buffers. */
typedef int (*SosGraphicsCheck)(char* cause, size_t cause_size, char* advice, size_t advice_size);

typedef struct {
    const char* app0;          /* prepared game folder (/app0) */
    const char* image_dir;     /* folder with boot.bin, content.bin and image.json */
    const char* user_dir;      /* user data; sons_of_sparta.ini lives here unless settings_file is set */
    const char* settings_file; /* NULL: <user_dir>/sons_of_sparta.ini */
    const char* logs_dir;      /* NULL when unknown */
    int skip_once;             /* --sos-skip-launcher: do not show it unless the game files are not ready */
    int force_show;            /* --launcher: show it even when "Show this launcher" is off, like holding Shift */
    SosGraphicsCheck check_graphics; /* NULL: sos_check_vulkan */
    int game_process;          /* Windows: with Allow overlay programs on, Play starts the game as a process of its own */
    const char* log_file;      /* this run's log, continued by the game process; NULL when there is none */
} SosLauncherConfig;

/* NULL asks for the default, which is relative to the executable (launcher/paths.h). */
typedef struct {
    const char* boot;
    const char* content_profile;
    const char* app0;
    const char* user_dir;
    const char* logs_dir;
} SosPathRequest;

typedef struct {
    char boot[4096];
    char content_profile[4096];
    char app0[4096];
    char image_dir[4096];
    char user_dir[4096];
    char logs_dir[4096];
    char error[512];
} SosPaths;

/* 0 when the executable's folder is unknown, a path does not fit or SOS_GAME_BUILD cannot be used. */
int sos_resolve_paths(const SosPathRequest* request, SosPaths* paths);

/* The folder holding this run's log: the log_file's parent when one is given, else logs_dir. 0 when
 * neither yields a path. */
int sos_logs_directory(const char* logs_dir, const char* log_file, char* out, size_t size);

/* 1 when the settings file keeps the logs of normal sessions (detailed_logging; 1 by default). */
int sos_detailed_logging(const char* settings_file);

/* Prints the startup state support needs as "Settings:" lines: port version, platform, game build and the display and
 * gameplay settings in effect. */
void sos_log_settings_summary(const char* settings_file, const char* image_dir);

/* Starts the game process sos_launcher_run created suspended. Call it once nothing writes to the log any more; 1 when
 * it runs. A process that could not be started is ended. */
int sos_resume_game_process(void);

int sos_build_boot_image(const char* app0, const char* out, char* error, size_t error_size);

typedef struct {
    char assets[4096];   /* the prepared resources.assets the game reads in place of its own; empty: the game's own files */
    char streamed[4096]; /* the prepared resources.assets.resS, always together with assets */
    char bundle[4096];   /* the prepared Addressables icon bundle, always together with the two above */
    char report[4096];   /* what the preparation found and did, one line per step, for the log */
} SosControllerIcons;

/* The Xbox prompts need left-stick glyphs and interaction icons the game does not ship; they are added to prepared copies of
 * three of its data files (launcher/controller_icons.h). Does nothing for the PlayStation style or a build without verified prompts. Only when copies
 * have to be made is a small window shown while it happens. Never fails: with no prepared copies the game's own files are used. */
void sos_prepare_controller_icons(const char* app0, const char* image_dir, int prompt_style, int build_supported,
                                  SosControllerIcons* icons);

typedef struct {
    const char* label;
    const char* title_id;
    const char* version;
    const char* region;
    const SosResolutionSite* resolution_sites; /* empty: the build runs only at its native size */
    size_t resolution_site_count;
} SosGameBuild;

int sos_game_build(const char* image_dir, SosGameBuild* build);
void sos_game_build_problem(const char* image_dir, char* out, size_t size);

/* Fills `names` with the distinct names of the Vulkan devices the renderer can use and returns how many; enumerates
 * only, no device is created. */
int sos_gpu_names(char (*names)[256], int max);

/* A SosGraphicsCheck: 1 when Vulkan can run the renderer. Otherwise logs why and returns 0. It asks again on every call
 * until it succeeds, so a driver installed meanwhile is found. */
int sos_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size);

typedef enum {
    SOS_LAUNCHER_PLAY = 0,
    SOS_LAUNCHER_QUIT = 1,
    SOS_LAUNCHER_BLOCKED = 2,
    SOS_LAUNCHER_GAME_STARTED = 3 /* game_process: the game process exists, suspended; this process ends */
} SosLauncherResult;

/* The result's name for the log ("Launcher result: play"), by value, never by array position. */
static inline const char *sos_launcher_result_name(SosLauncherResult result) {
    switch (result) {
    case SOS_LAUNCHER_PLAY: return "play";
    case SOS_LAUNCHER_QUIT: return "quit";
    case SOS_LAUNCHER_BLOCKED: return "blocked";
    case SOS_LAUNCHER_GAME_STARTED: return "game process started";
    }
    return "unknown";
}

/* Shows the launcher when its rules say so (game files not ready, "Show this launcher" on, or Shift held) and returns
 * once the player pressed Play or quit. Returns SOS_LAUNCHER_PLAY at once when it is not shown.
 * While check_graphics fails, Play is refused: the launcher says why and stays open. A launcher that is not shown
 * explains in a window of its own and returns SOS_LAUNCHER_BLOCKED.
 * With game_process and Allow overlay programs on, Play creates the game process instead and returns
 * SOS_LAUNCHER_GAME_STARTED; when it cannot be created, the launcher stays open with the reason (or explains in a window
 * of its own). */
SosLauncherResult sos_launcher_run(const SosLauncherConfig* config);

#ifdef __cplusplus
}
#endif

#endif
