/* The system a session runs on, logged once at start so a bug report can tell a Steam Deck in Gaming Mode from a
 * desktop session: port version, OS and kernel, SteamOS, Gamescope, Steam mode, display session, memory and the overlay
 * and Vulkan layer switches. Names and numbers only: no paths, user names or environment values that could hold them. */
#ifndef SOS_SYSTEM_REPORT_H
#define SOS_SYSTEM_REPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *(*SosEnvironment)(const char *name);

typedef struct SosSystemFacts {
    int gamescope;      /* a Gamescope session or a game nested in Gamescope */
    int gaming_mode;    /* Steam Gaming Mode: Gamescope is the session compositor */
    int steam;          /* started by Steam */
    int steam_deck;     /* SteamDeck=1 */
    int steamos;        /* /etc/os-release ID=steamos */
    int wayland;        /* WAYLAND_DISPLAY set */
    int x11;            /* DISPLAY set */
    int steam_overlay;  /* gameoverlayrenderer in LD_PRELOAD */
    int appimage;       /* started from the AppImage */
} SosSystemFacts;

/* The facts from an environment and the text of /etc/os-release (either may be NULL). */
SosSystemFacts sos_system_facts(SosEnvironment environment, const char *os_release);

/* "Gaming Mode", "Desktop Mode (Steam)", "nested Gamescope", "desktop" ... */
const char *sos_system_mode_name(const SosSystemFacts *facts);

/* The report lines ("System: ...\n" each) for the given sources; any may be NULL. Returns the length written. */
size_t sos_system_report_format(char *out, size_t size, const char *port_version, SosEnvironment environment,
                                const char *os_release, const char *meminfo, const char *kernel_release,
                                const char *machine);

/* Gathers the sources of this process and prints the report. */
void sos_system_report_log(const char *port_version);

#ifdef __cplusplus
}
#endif

#endif
