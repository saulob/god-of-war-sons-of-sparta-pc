/* The launcher, called by the loader before the guest and the game window exist. */
#ifndef SOS_LAUNCHER_H
#define SOS_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SosGraphicsCheck)(char* cause, size_t cause_size, char* advice, size_t advice_size);

typedef struct {
    const char* app0;
    const char* image_dir;
    const char* user_dir;
    const char* settings_file; /* NULL: <user_dir>/sons_of_sparta.ini */
    const char* logs_dir;
    int skip_once;
    int force_show;
    SosGraphicsCheck check_graphics; /* NULL: dcp_check_vulkan */
    const char* log_file;
} SosLauncherConfig;

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

int sos_resolve_paths(const SosPathRequest* request, SosPaths* paths);
int sos_logs_directory(const char* logs_dir, const char* log_file, char* out, size_t size);

int sos_build_boot_image(const char* app0, const char* out, char* error, size_t error_size);

typedef struct {
    const char* label;
    const char* title_id;
    const char* version;
    const char* region;
} SosGameBuild;

int sos_game_build(const char* image_dir, SosGameBuild* build);
void sos_game_build_problem(const char* image_dir, char* out, size_t size);

int sos_gpu_names(char (*names)[256], int max);
int sos_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size);

typedef enum {
    SOS_LAUNCHER_PLAY = 0,
    SOS_LAUNCHER_QUIT = 1,
    SOS_LAUNCHER_BLOCKED = 2
} SosLauncherResult;

SosLauncherResult sos_launcher_run(const SosLauncherConfig* config);

#ifdef __cplusplus
}
#endif

#endif
