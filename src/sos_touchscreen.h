/* A touchscreen as the DualShock 4 touch surface (SteamOS and other Gamescope sessions, see sos_touch_gamescope.h).
 *
 * The window thread forwards the screen's SDL finger events; the pad sampler takes one sample per game read of player 1.
 * Up to two fingers are followed, as the DualShock 4 reports two contacts, each with its own id. A finger maps onto the
 * touch surface the way the presented game picture covers the window (letterbox bars are outside the surface), the same
 * 1920x943 surface and 0..1919 / 0..942 points a controller touchpad gives (runtime_pad.c). A short single-finger tap
 * also clicks the touchpad, as a mouse click would have. A finger is never a mouse: the window drops SDL's touch-made
 * mouse events while this route is on.
 *
 * Pure C, no SDL and no locking: the caller serialises the window thread and the pad sampler. */
#ifndef SOS_TOUCHSCREEN_H
#define SOS_TOUCHSCREEN_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOS_TOUCH_CONTACTS 2
#define SOS_TOUCH_MAX_X 1919 /* scePadGetControllerInformation: 1920 x 943 */
#define SOS_TOUCH_MAX_Y 942
#define SOS_TOUCHPAD_BUTTON 0x100000u /* the PS4 touchpad click */
#define SOS_TOUCH_TAP_MAX_NS 300000000ull
#define SOS_TOUCH_TAP_RADIUS 60 /* touch units a tap may wander */
/* The game reads the pad once per frame, so a tap's click is held for a few reads. */
#define SOS_TOUCH_CLICK_READS 3

/* The presented game picture as fractions of the window. */
typedef struct {
    float x, y, width, height;
} SosTouchArea;

typedef enum { SOS_CONTACT_NONE, SOS_CONTACT_DOWN, SOS_CONTACT_LIFTED } SosContactState;

typedef struct {
    SosContactState state;
    uint64_t finger;
    uint64_t down_ns;
    uint16_t x, y, start_x, start_y;
    int moved;
    int shown;       /* reads that carried this contact */
    int click_reads; /* reads left holding a tap's click once lifted */
    uint8_t id;
} SosTouchContact;

typedef struct {
    SosTouchContact contact[SOS_TOUCH_CONTACTS];
    int blocked;
    int multi; /* a second finger joined the current gesture: no tap click */
    uint8_t next_id;
} SosTouchscreen;

typedef struct {
    int count;
    uint16_t x[SOS_TOUCH_CONTACTS], y[SOS_TOUCH_CONTACTS];
    uint8_t id[SOS_TOUCH_CONTACTS];
    uint32_t buttons;
} SosTouchSample;

/* A picture of `width` x `height` centred on a surface of `surface_width` x `surface_height` (the presenter letterboxes
 * and centres the frame); the whole window when any size is unknown. */
static inline SosTouchArea sos_touch_area_centred(int width, int height, int surface_width, int surface_height) {
    const SosTouchArea whole = {0.0f, 0.0f, 1.0f, 1.0f};
    if (width <= 0 || height <= 0 || surface_width <= 0 || surface_height <= 0 || width > surface_width ||
        height > surface_height)
        return whole;
    const float sw = (float)surface_width, sh = (float)surface_height;
    SosTouchArea area = {(float)((surface_width - width) / 2) / sw, (float)((surface_height - height) / 2) / sh,
                         (float)width / sw, (float)height / sh};
    return area;
}

static inline float sos_touch_clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* Maps a window fraction to touch surface points, clamped to the picture. Returns whether the point is on the picture. */
static inline int sos_touch_point(SosTouchArea area, float window_x, float window_y, uint16_t *x, uint16_t *y) {
    if (!(area.width > 0.0f) || !(area.height > 0.0f)) {
        area.x = area.y = 0.0f;
        area.width = area.height = 1.0f;
    }
    const float u = (window_x - area.x) / area.width, v = (window_y - area.y) / area.height;
    const int inside = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
    *x = (uint16_t)lroundf(sos_touch_clamp01(u) * (float)SOS_TOUCH_MAX_X);
    *y = (uint16_t)lroundf(sos_touch_clamp01(v) * (float)SOS_TOUCH_MAX_Y);
    return inside;
}

static inline SosTouchContact *sos_touch_find(SosTouchscreen *s, uint64_t finger) {
    for (int i = 0; i < SOS_TOUCH_CONTACTS; ++i)
        if (s->contact[i].state == SOS_CONTACT_DOWN && s->contact[i].finger == finger) return &s->contact[i];
    return NULL;
}

static inline int sos_touch_active(const SosTouchscreen *s) {
    int active = 0;
    for (int i = 0; i < SOS_TOUCH_CONTACTS; ++i) active += s->contact[i].state != SOS_CONTACT_NONE;
    return active;
}

static inline void sos_touch_follow(SosTouchContact *c, SosTouchArea area, float x, float y) {
    sos_touch_point(area, x, y, &c->x, &c->y);
    if (abs((int)c->x - (int)c->start_x) > SOS_TOUCH_TAP_RADIUS || abs((int)c->y - (int)c->start_y) > SOS_TOUCH_TAP_RADIUS)
        c->moved = 1;
}

/* Window thread. Positions are window fractions; times are SDL event timestamps in nanoseconds. A finger counts only
 * when it lands on the picture and a contact is free; a third finger is ignored like on the DualShock 4. */
static inline void sos_touchscreen_down(SosTouchscreen *s, uint64_t finger, SosTouchArea area, float x, float y,
                                        uint64_t time_ns) {
    uint16_t tx, ty;
    if (s->blocked || sos_touch_find(s, finger) || !sos_touch_point(area, x, y, &tx, &ty)) return;
    SosTouchContact *free_contact = NULL;
    for (int i = 0; i < SOS_TOUCH_CONTACTS && !free_contact; ++i)
        if (s->contact[i].state == SOS_CONTACT_NONE) free_contact = &s->contact[i];
    if (!free_contact) return;
    int held = 0;
    for (int i = 0; i < SOS_TOUCH_CONTACTS; ++i) held += s->contact[i].state == SOS_CONTACT_DOWN;
    s->multi = held > 0;
    memset(free_contact, 0, sizeof(*free_contact));
    free_contact->state = SOS_CONTACT_DOWN;
    free_contact->finger = finger;
    free_contact->down_ns = time_ns;
    free_contact->x = free_contact->start_x = tx;
    free_contact->y = free_contact->start_y = ty;
    free_contact->id = s->next_id;
    s->next_id = (uint8_t)((s->next_id + 1) & 0x7f);
}

static inline void sos_touchscreen_motion(SosTouchscreen *s, uint64_t finger, SosTouchArea area, float x, float y) {
    SosTouchContact *c = sos_touch_find(s, finger);
    if (c) sos_touch_follow(c, area, x, y);
}

static inline void sos_touchscreen_up(SosTouchscreen *s, uint64_t finger, SosTouchArea area, float x, float y,
                                      uint64_t time_ns) {
    SosTouchContact *c = sos_touch_find(s, finger);
    if (!c) return;
    sos_touch_follow(c, area, x, y);
    const int tap = !s->multi && !c->moved && time_ns >= c->down_ns && time_ns - c->down_ns <= SOS_TOUCH_TAP_MAX_NS;
    c->state = SOS_CONTACT_LIFTED;
    c->click_reads = tap ? SOS_TOUCH_CLICK_READS : 0;
}

/* The finger left without a gesture (SDL_EVENT_FINGER_CANCELED): it disappears, nothing is clicked. */
static inline void sos_touchscreen_cancel(SosTouchscreen *s, uint64_t finger) {
    SosTouchContact *c = sos_touch_find(s, finger);
    if (c) memset(c, 0, sizeof(*c));
}

/* Focus loss, an overlay, text entry or a window mode switch: every contact and click is dropped. */
static inline void sos_touchscreen_release_all(SosTouchscreen *s) {
    for (int i = 0; i < SOS_TOUCH_CONTACTS; ++i) memset(&s->contact[i], 0, sizeof(s->contact[i]));
    s->multi = 0;
}

static inline void sos_touchscreen_block(SosTouchscreen *s, int blocked) {
    s->blocked = blocked != 0;
    if (s->blocked) sos_touchscreen_release_all(s);
}

/* Pad sampler, one call per game read. A lifted contact is still shown once if no read saw it, so a tap shorter than a
 * frame still lands where it was; a tap then holds the touchpad click for SOS_TOUCH_CLICK_READS reads. */
static inline void sos_touchscreen_sample(SosTouchscreen *s, SosTouchSample *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < SOS_TOUCH_CONTACTS; ++i) {
        SosTouchContact *c = &s->contact[i];
        if (c->state == SOS_CONTACT_NONE) continue;
        if (c->state == SOS_CONTACT_LIFTED && c->shown > 0) {
            if (c->click_reads <= 0) {
                memset(c, 0, sizeof(*c));
                continue;
            }
            --c->click_reads;
            out->buttons |= SOS_TOUCHPAD_BUTTON;
        }
        ++c->shown;
        out->x[out->count] = c->x;
        out->y[out->count] = c->y;
        out->id[out->count] = c->id;
        ++out->count;
    }
}

/* Touch events the log counts: the first line, then every tenth power. */
typedef enum {
    SOS_TOUCH_EVENT_DETECTED,         /* touchscreen_detected: once per session */
    SOS_TOUCH_EVENT_TOUCHPAD_USED,    /* touchscreen_touchpad_used: each finger handed to the touchpad */
    SOS_TOUCH_EVENT_MOUSE_SUPPRESSED, /* touchscreen_synthetic_mouse_suppressed: each touch-made mouse press dropped */
    SOS_TOUCH_EVENT_COUNT
} SosTouchEvent;

static inline int sos_touch_count_due(uint64_t count) {
    while (count >= 10 && count % 10 == 0) count /= 10;
    return count == 1;
}

/* Counts `event` in `counts` and returns whether a log line is due, written to `line`. */
static inline int sos_touch_event_note(uint64_t counts[SOS_TOUCH_EVENT_COUNT], SosTouchEvent event, char *line,
                                       size_t size) {
    static const char *const names[SOS_TOUCH_EVENT_COUNT] = {
        "touchscreen_detected", "touchscreen_touchpad_used", "touchscreen_synthetic_mouse_suppressed"};
    if ((int)event < 0 || event >= SOS_TOUCH_EVENT_COUNT) return 0;
    const uint64_t count = ++counts[event];
    if ((event == SOS_TOUCH_EVENT_DETECTED && count > 1) || !sos_touch_count_due(count)) return 0;
    snprintf(line, size, "Touchscreen telemetry: event=%s count=%llu", names[event], (unsigned long long)count);
    return 1;
}

#endif
