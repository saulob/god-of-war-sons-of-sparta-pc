/* Window backend for Steam-launched Wayland desktop sessions: Steam cannot associate a
 * native Wayland window with a non-Steam shortcut, so its Steam Input layout stays on Desktop. Asking SDL for X11
 * (XWayland) first fixes that. Gamescope (Gaming Mode) is left untouched. */
#ifndef SOS_VIDEO_BACKEND_H
#define SOS_VIDEO_BACKEND_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SOS_VIDEO_BACKEND_POLICY_ONLY
#include <SDL3/SDL.h>
#endif

typedef struct {
    int linux_host;
    const char *steam_game_id, *steam_app_id;
    const char *session_type;
    const char *wayland_display, *display;
    const char *gamescope_display;
    const char *video_driver;
    int x11_driver_listed;
} SosVideoEnvironment;

typedef enum {
    SOS_VIDEO_UNCHANGED,
    SOS_VIDEO_USE_X11,
    SOS_VIDEO_X11_UNAVAILABLE,
    SOS_VIDEO_KEEP_GAMESCOPE,
    SOS_VIDEO_KEEP_USER_DRIVER
} SosVideoChoice;

#define SOS_VIDEO_X11_FIRST "x11,wayland"
#define SOS_VIDEO_LINE_SIZE 160
#define SOS_VIDEO_MAX_LINES 2

static inline int sos_video_set(const char *value) { return value && *value; }

static inline int sos_video_steam_launch(const SosVideoEnvironment *e) {
    return sos_video_set(e->steam_game_id) || sos_video_set(e->steam_app_id);
}

static inline int sos_video_wayland_session(const SosVideoEnvironment *e) {
    if (sos_video_set(e->session_type)) return !strcmp(e->session_type, "wayland");
    return sos_video_set(e->wayland_display);
}

static inline SosVideoChoice sos_video_choose(const SosVideoEnvironment *e) {
    if (!e->linux_host || !sos_video_steam_launch(e)) return SOS_VIDEO_UNCHANGED;
    if (sos_video_set(e->gamescope_display)) return SOS_VIDEO_KEEP_GAMESCOPE;
    if (!sos_video_wayland_session(e)) return SOS_VIDEO_UNCHANGED;
    if (sos_video_set(e->video_driver)) return SOS_VIDEO_KEEP_USER_DRIVER;
    if (!sos_video_set(e->display) || !e->x11_driver_listed) return SOS_VIDEO_X11_UNAVAILABLE;
    return SOS_VIDEO_USE_X11;
}

/* Telemetry lines for one session: only event names, the session type, the requested backend and yes/no values. */
static inline size_t sos_video_telemetry(SosVideoChoice choice, char lines[][SOS_VIDEO_LINE_SIZE]) {
    const char *gamescope = choice == SOS_VIDEO_KEEP_GAMESCOPE ? "yes" : "no";
    const char *event = NULL, *backend = "default", *reason = "";
    size_t n = 0;
    if (choice == SOS_VIDEO_UNCHANGED) return 0;
    if (choice != SOS_VIDEO_KEEP_GAMESCOPE)
        snprintf(lines[n++], SOS_VIDEO_LINE_SIZE,
                 "Steam Input telemetry: event=steam_input_wayland_detected session=wayland backend=default "
                 "gamescope=no count=1");
    switch (choice) {
    case SOS_VIDEO_USE_X11: event = "steam_input_xwayland_selected"; backend = "x11"; break;
    case SOS_VIDEO_X11_UNAVAILABLE: event = "steam_input_xwayland_unavailable"; break;
    case SOS_VIDEO_KEEP_GAMESCOPE: event = "steam_input_native_wayland_retained"; reason = " reason=gamescope"; break;
    default: event = "steam_input_native_wayland_retained"; reason = " reason=user_driver"; break;
    }
    snprintf(lines[n++], SOS_VIDEO_LINE_SIZE, "Steam Input telemetry: event=%s session=wayland backend=%s gamescope=%s%s count=1",
             event, backend, gamescope, reason);
    return n;
}

#ifndef SOS_VIDEO_BACKEND_POLICY_ONLY
static inline SosVideoEnvironment sos_video_environment(void) {
    SosVideoEnvironment e;
    memset(&e, 0, sizeof(e));
#ifdef __linux__
    e.linux_host = 1;
#endif
    e.steam_game_id = getenv("SteamGameId");
    e.steam_app_id = getenv("SteamAppId");
    e.session_type = getenv("XDG_SESSION_TYPE");
    e.wayland_display = getenv("WAYLAND_DISPLAY");
    e.display = getenv("DISPLAY");
    e.gamescope_display = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    e.video_driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    for (int i = 0, n = SDL_GetNumVideoDrivers(); i < n; ++i) {
        const char *name = SDL_GetVideoDriver(i);
        if (name && !strcmp(name, "x11")) e.x11_driver_listed = 1;
    }
    return e;
}

/* Before the first SDL_INIT_VIDEO, and again after any SDL_Quit (which clears hints). `report` logs once per session. */
static inline SosVideoChoice sos_video_backend_prepare(int report) {
    const SosVideoEnvironment e = sos_video_environment();
    const SosVideoChoice choice = sos_video_choose(&e);
    if (choice == SOS_VIDEO_USE_X11) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, SOS_VIDEO_X11_FIRST);
    if (report) {
        char lines[SOS_VIDEO_MAX_LINES][SOS_VIDEO_LINE_SIZE];
        const size_t n = sos_video_telemetry(choice, lines);
        for (size_t i = 0; i < n; ++i) puts(lines[i]);
    }
    return choice;
}
#endif

#endif
