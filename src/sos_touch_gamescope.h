/* Touchscreen routing under Gamescope (SteamOS Gaming Mode, Steam Deck and other handhelds), after Dragon's Crown Pro
 * (c5a05e2148ed, eb6ac2b67330).
 *
 * Gamescope turns a touch into a mouse pointer and a left click unless its touch click mode is Passthrough, and it keeps
 * one mode for the whole compositor: the newest STEAM_TOUCH_CLICK_MODE write on any of its Xwayland roots wins. Gaming
 * Mode runs the game's root and the one Steam uses, and Steam writes its own mode there again. While the game window has
 * focus the game therefore writes Passthrough on its own display and on every other local Gamescope root, and again
 * about once a second; each root's previous value is given back on focus loss and on every exit path. The launcher never
 * asks: its taps stay mouse clicks. SOS_TOUCH_GAMESCOPE=0 keeps Gamescope's own touch handling.
 *
 * This header holds the decisions (when to ask, what each root gives back) and the bounded route telemetry. No SDL, no
 * X11: gpu/shim/sos_gamescope_touch.h does the X11 calls. */
#ifndef SOS_TOUCH_GAMESCOPE_H
#define SOS_TOUCH_GAMESCOPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOS_GAMESCOPE_TOUCH_ATOM "STEAM_TOUCH_CLICK_MODE"
#define SOS_GAMESCOPE_TOUCH_MARKER_ATOM "GAMESCOPE_INPUT_COUNTER"
#define SOS_GAMESCOPE_TOUCH_LEFT 1u /* Gamescope's default: a touch is a pointer and a left click */
#define SOS_GAMESCOPE_TOUCH_PASSTHROUGH 4u
/* Steam writes the mode on its own Gamescope root, so the game rewrites Passthrough while it has focus. */
#define SOS_GAMESCOPE_TOUCH_RECHECK_MS 1000u
#define SOS_GAMESCOPE_TOUCH_MAX_ROOTS 4
#define SOS_TOUCH_ROUTE_LINE_SIZE 160

typedef struct {
    int linux_host;
    int game_window; /* the game's window asks; the launcher never does */
    int gamescope;   /* the session runs under Gamescope */
    const char *display;
    const char *opt_out; /* SOS_TOUCH_GAMESCOPE: "0" keeps Gamescope's own touch handling */
} SosTouchGamescopeEnv;

/* Whether the touchscreen is routed to the game as fingers: Linux, the game window, Gamescope with an X display, and no
 * opt-out. Everything else (Windows, a desktop session, the launcher) keeps the touch-as-mouse behaviour. */
static inline int sos_touch_gamescope_wanted(const SosTouchGamescopeEnv *e) {
    if (!e->linux_host || !e->game_window || !e->gamescope || !e->display || !*e->display) return 0;
    return !(e->opt_out && !strcmp(e->opt_out, "0"));
}

/* One X root the game asks for Passthrough. The game owns it from its first write until its release and remembers the
 * value that root held. */
typedef struct {
    int owned;
    uint32_t saved;
} SosTouchModeRoot;

/* Adopts the root's current value as the one to give back, taking a newer choice from Steam. Passthrough is written on
 * every call: a competing write on another root wins Gamescope's single mode even while this root reads Passthrough. */
static inline void sos_touch_mode_claim(SosTouchModeRoot *root, int have, uint32_t current) {
    if (!root->owned || (have && current != SOS_GAMESCOPE_TOUCH_PASSTHROUGH)) {
        root->saved = have ? current : SOS_GAMESCOPE_TOUCH_LEFT;
        root->owned = 1;
    }
}

/* Gives back the saved value only while Passthrough is still the one this game wrote; returns whether to write. A root
 * that already held Passthrough gives nothing back, and a value Steam wrote in the meantime is left alone. */
static inline int sos_touch_mode_release(SosTouchModeRoot *root, int have, uint32_t current, uint32_t *value) {
    const int owned = root->owned;
    root->owned = 0;
    if (!owned || !have || current != SOS_GAMESCOPE_TOUCH_PASSTHROUGH) return 0;
    if (root->saved == SOS_GAMESCOPE_TOUCH_PASSTHROUGH) return 0;
    *value = root->saved;
    return 1;
}

/* Only these names reach a log line. */
static inline const char *sos_touch_video_name(const char *driver) {
    if (!driver) return "none";
    if (!strcmp(driver, "wayland") || !strcmp(driver, "x11") || !strcmp(driver, "windows")) return driver;
    return "other";
}

typedef enum { SOS_TOUCH_CLAIM_NO_X11, SOS_TOUCH_CLAIM_NO_DISPLAY } SosTouchClaimFailure;

typedef struct {
    const char *video;
    int gamescope;
    int routed;     /* fingers reach the game's touchpad */
    int direct_devices;
    int roots;      /* X roots the game claimed Passthrough on */
    int compositor; /* one of them is Gamescope's own root rather than the game's display */
    int failure;    /* SosTouchClaimFailure when no root could be claimed */
} SosTouchRouteInfo;

typedef enum {
    SOS_TOUCH_ROUTE_READY,
    SOS_TOUCH_ROUTE_CLAIMED,
    SOS_TOUCH_ROUTE_CLAIM_FAILED,
    SOS_TOUCH_ROUTE_REASSERT,
    SOS_TOUCH_ROUTE_RELEASED,
    SOS_TOUCH_ROUTE_EVENT_COUNT
} SosTouchRouteEvent;

/* READY, CLAIMED and CLAIM_FAILED are logged once; REASSERT and RELEASED at the first, then every tenth power. */
typedef struct {
    int logged[SOS_TOUCH_ROUTE_EVENT_COUNT];
    unsigned reasserts;
    unsigned releases;
} SosTouchRouteLog;

static inline int sos_touch_route_due(unsigned count) {
    while (count >= 10 && count % 10 == 0) count /= 10;
    return count == 1;
}

static inline int sos_touch_route_note(SosTouchRouteLog *log, SosTouchRouteEvent event, const SosTouchRouteInfo *route,
                                       char *line, size_t size) {
    if ((int)event < 0 || (int)event >= SOS_TOUCH_ROUTE_EVENT_COUNT) return 0;
    const int devices = route->direct_devices < 0 ? 0 : route->direct_devices > 99 ? 99 : route->direct_devices;
    const int roots = route->roots < 0 ? 0 : route->roots > 9 ? 9 : route->roots;
    switch (event) {
    case SOS_TOUCH_ROUTE_READY:
        if (log->logged[(int)event]) return 0;
        snprintf(line, size,
                 "Touchscreen telemetry: event=touch_route_ready video=%s gamescope=%s routed=%s direct_devices=%d count=1",
                 sos_touch_video_name(route->video), route->gamescope ? "yes" : "no", route->routed ? "yes" : "no",
                 devices);
        break;
    case SOS_TOUCH_ROUTE_CLAIMED:
        if (log->logged[(int)event]) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_claim roots=%d compositor=%s count=1", roots,
                 route->compositor ? "yes" : "no");
        break;
    case SOS_TOUCH_ROUTE_CLAIM_FAILED:
        if (log->logged[(int)event]) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_claim_failed reason=%s count=1",
                 route->failure == SOS_TOUCH_CLAIM_NO_DISPLAY ? "display" : "x11");
        break;
    case SOS_TOUCH_ROUTE_REASSERT:
        if (!log->logged[SOS_TOUCH_ROUTE_CLAIMED] || !sos_touch_route_due(++log->reasserts)) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_reassert roots=%d count=%u", roots,
                 log->reasserts);
        break;
    case SOS_TOUCH_ROUTE_RELEASED:
        if (!log->logged[SOS_TOUCH_ROUTE_CLAIMED] || !sos_touch_route_due(++log->releases)) return 0;
        snprintf(line, size, "Touchscreen telemetry: event=gamescope_touch_release roots=%d count=%u", roots,
                 log->releases);
        break;
    default:
        return 0;
    }
    log->logged[(int)event] = 1;
    return 1;
}

#endif
