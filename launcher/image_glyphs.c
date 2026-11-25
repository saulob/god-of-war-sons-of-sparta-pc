#include "image_glyphs.h"

#include <stdio.h>
#include <string.h>

#include "controller_prompts.h"
#include "image_glyphs_sites.h"

enum { CONTROL_DEVICE_GAMEPAD = 2 };
enum { NATIVE_OBJECT = 0x10, STRING_LENGTH = 0x10, STRING_CHARS = 0x14, CLASS_STATICS = 0xb8, RUNTIME_SUBSYSTEMS = 0x20 };
enum { TRACKED_IMAGES = 512, CACHED_SPRITES = 1024, RETRY_CALLS = 256 };

#define FNV_OFFSET UINT64_C(0xcbf29ce484222325)

typedef struct {
    const char *build;
    const SosImageGlyphSite *site;
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA57767-1.00", &kImageGlyphs_1_00},
    {"CUSA57767-1.01", &kImageGlyphs_1_01},
};

const SosImageGlyphSite *sos_image_glyphs_site(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(build_key, kBuilds[i].build)) return kBuilds[i].site;
    return NULL;
}

/* The glyph of each sprite number of a sheet. */
#define NONE SOS_GLYPH_NONE
static const int8_t kOutlineSheet[] = {
    SOS_GLYPH_WEST, SOS_GLYPH_EAST, SOS_GLYPH_SOUTH, SOS_GLYPH_NORTH, SOS_GLYPH_START, SOS_GLYPH_SELECT, NONE, NONE, NONE, NONE, NONE,
    SOS_GLYPH_LEFT_STICK_PRESS, SOS_GLYPH_LEFT_TRIGGER, SOS_GLYPH_RIGHT_TRIGGER, SOS_GLYPH_LEFT_SHOULDER, SOS_GLYPH_RIGHT_SHOULDER,
    NONE, NONE, SOS_GLYPH_RIGHT_STICK_PRESS, SOS_GLYPH_TOUCHPAD,
};
static const int8_t kIconMapSheet[] = {
    SOS_GLYPH_EAST, SOS_GLYPH_SOUTH, SOS_GLYPH_NORTH, SOS_GLYPH_WEST, SOS_GLYPH_SELECT, SOS_GLYPH_START, NONE, NONE, NONE, NONE, NONE,
    SOS_GLYPH_LEFT_TRIGGER, SOS_GLYPH_RIGHT_TRIGGER, SOS_GLYPH_LEFT_SHOULDER, SOS_GLYPH_RIGHT_SHOULDER, NONE, NONE,
    SOS_GLYPH_RIGHT_STICK_PRESS, SOS_GLYPH_LEFT_STICK_PRESS, SOS_GLYPH_TOUCHPAD,
};
static const int8_t kFilledSheet[] = {SOS_GLYPH_WEST, SOS_GLYPH_EAST, SOS_GLYPH_SOUTH, SOS_GLYPH_NORTH};
#undef NONE

#define SHEET(prefix, glyphs) {prefix, sizeof(prefix) - 1, glyphs, sizeof(glyphs)}
static const struct {
    const char *prefix;
    int32_t length;
    const int8_t *glyphs;
    int32_t count;
} kSheets[] = {
    SHEET("KS_ButtonPrompt_", kOutlineSheet),
    SHEET("PS_ButtonPrompts_", kIconMapSheet),
    SHEET("PS4_ButtonPrompts_", kIconMapSheet),
    SHEET("ButtonPromptSheetTemp(256,64)_", kFilledSheet),
};

SosGlyph sos_image_glyph_of_name(const uint16_t *name, int32_t length) {
    for (size_t i = 0; name && i < sizeof(kSheets) / sizeof(*kSheets); ++i) {
        const int32_t digits = length - kSheets[i].length;
        if (digits < 1 || digits > 2) continue;
        int32_t at = 0;
        while (at < kSheets[i].length && name[at] == (uint8_t)kSheets[i].prefix[at]) ++at;
        if (at < kSheets[i].length) continue;
        int32_t number = 0;
        for (; at < length && name[at] >= '0' && name[at] <= '9'; ++at) number = number * 10 + (name[at] - '0');
        if (at < length || (digits == 2 && number < 10) || number >= kSheets[i].count) return SOS_GLYPH_NONE;
        return (SosGlyph)kSheets[i].glyphs[number];
    }
    return SOS_GLYPH_NONE;
}

static uint64_t fnv1a(uint64_t hash, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

static int hooks_of(int style) {
    if (style == SOS_PROMPTS_AUTOMATIC) return SOS_GLYPH_HOOKS;
    return style == SOS_PROMPTS_XBOX ? SOS_GLYPH_HOOK_TEXT_ATLAS : 0;
}

int sos_image_glyphs_verify_site(const uint8_t *image, uint64_t size, const SosImageGlyphSite *site, int style, const char **why) {
    const char *reason = NULL;
    uint64_t services = FNV_OFFSET;
    if (!site) reason = "no verified glyph hooks in this build";
    else if (!hooks_of(style)) reason = "the PlayStation style has no glyph hooks";
    for (int i = 0; !reason && i < hooks_of(style); ++i) {
        const SosGlyphHook *hook = &site->hooks[i];
        if (hook->length < SOS_GLYPH_HOOK_STOLEN || (uint64_t)hook->method + hook->length > size) reason = "the image ends before a hooked method";
        else if (memcmp(image + hook->method, hook->entry, SOS_GLYPH_HOOK_STOLEN)) reason = "a hooked method does not start as verified";
        else if (fnv1a(FNV_OFFSET, image + hook->method, hook->length) != hook->hash) reason = "a hooked method is not the verified code";
    }
    for (int i = 0; !reason && i < SOS_GLYPH_SERVICES; ++i) {
        const SosGlyphRange range = site->services[i];
        if (range.begin >= range.end || range.end > size) reason = "the image ends before the code the handlers call";
        else services = fnv1a(services, image + range.begin, range.end - range.begin);
    }
    if (!reason && services != site->services_hash) reason = "the code the handlers call is not the verified code";
    if (reason && why) *why = reason;
    return reason == NULL;
}

int sos_image_glyphs_verify(const uint8_t *image, uint64_t size, const char *build_key, int style, const char **why) {
    return sos_image_glyphs_verify_site(image, size, sos_image_glyphs_site(build_key), style, why);
}

/* push rdi, rsi, rdx, rcx, r8, r9 ; sub rsp,8 ; mov rax,handler ; call rax ; add rsp,8 ; pop them. */
static const uint8_t kStubHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
static const uint8_t kStubTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
enum { STUB_SIZE = sizeof(kStubHead) + 8 + sizeof(kStubTail) + SOS_GLYPH_HOOK_STOLEN + 5 };
_Static_assert(STUB_SIZE <= SOS_GLYPH_STUB_SLOT, "a glyph hook stub fits its slot");

static int reaches(const uint8_t *from_end, const uint8_t *to) {
    const int64_t delta = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from_end;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

static void put_rel32(uint8_t *at, const uint8_t *from_end, const uint8_t *to) {
    const int32_t rel = (int32_t)((int64_t)(intptr_t)to - (int64_t)(intptr_t)from_end);
    memcpy(at, &rel, sizeof(rel));
}

static void write_hook(uint8_t *method, uint8_t *stub, uintptr_t handler) {
    const uint64_t target = handler;
    uint8_t *at = stub;
    memcpy(at, kStubHead, sizeof(kStubHead));
    at += sizeof(kStubHead);
    memcpy(at, &target, sizeof(target));
    at += sizeof(target);
    memcpy(at, kStubTail, sizeof(kStubTail));
    at += sizeof(kStubTail);
    memcpy(at, method, SOS_GLYPH_HOOK_STOLEN);
    at += SOS_GLYPH_HOOK_STOLEN;
    *at++ = 0xe9;
    put_rel32(at, at + 4, method + SOS_GLYPH_HOOK_STOLEN);
    method[0] = 0xe9;
    put_rel32(method + 1, method + 5, stub);
    method[5] = 0x90;
}

int sos_image_glyphs_install_site(uint8_t *image, uint64_t size, const SosImageGlyphSite *site, int style, uint8_t *stubs, size_t stubs_capacity,
                                  char *message, size_t message_size) {
    const char *why = "";
    if (!sos_image_glyphs_verify_site(image, size, site, style, &why)) {
        snprintf(message, message_size, "not applied: %s", why);
        return 0;
    }
    const int hooks = hooks_of(style);
    int in_reach = stubs && stubs_capacity >= (size_t)hooks * SOS_GLYPH_STUB_SLOT;
    for (int i = 0; in_reach && i < hooks; ++i) {
        const uint8_t *method = image + site->hooks[i].method, *stub = stubs + i * SOS_GLYPH_STUB_SLOT;
        in_reach = reaches(method + 5, stub) && reaches(stub + STUB_SIZE, method + SOS_GLYPH_HOOK_STOLEN);
    }
    if (!in_reach) {
        snprintf(message, message_size, "not applied: no room for the stubs within reach of the image");
        return 0;
    }
    const uintptr_t handlers[SOS_GLYPH_HOOKS] = {(uintptr_t)sos_image_glyphs_on_enable, (uintptr_t)sos_image_glyphs_on_disable,
                                                 (uintptr_t)sos_image_glyphs_on_set_sprite, (uintptr_t)sos_prompt_hook_handler};
    for (int i = 0; i < hooks; ++i) write_hook(image + site->hooks[i].method, stubs + i * SOS_GLYPH_STUB_SLOT, handlers[i]);
    snprintf(message, message_size, "Menu glyph hooks installed at image+0x%x%s", site->hooks[0].method,
             hooks > SOS_GLYPH_HOOK_TEXT_ATLAS ? " and the text atlas" : "");
    return 1;
}

int sos_image_glyphs_install(uint8_t *image, uint64_t size, const char *build_key, int style, uint8_t *stubs, size_t stubs_capacity,
                             char *message, size_t message_size) {
    const int installed = sos_image_glyphs_install_site(image, size, sos_image_glyphs_site(build_key), style, stubs, stubs_capacity, message, message_size);
    const size_t used = strlen(message);
    if (!installed && used < message_size) snprintf(message + used, message_size - used, " in %s", build_key ? build_key : "this build");
    return installed;
}

typedef struct {
    void *image;
    void *shown; /* the override set on the Image, or NULL */
    int glyph;
} TrackedImage;

static struct {
    int armed, style, busy, shown, pending, calls;
    SosImageGlyphSite site;
    SosImageGlyphGame game;
    size_t count;
    TrackedImage images[TRACKED_IMAGES];
    struct {
        int32_t id;
        int8_t glyph;
    } sprites[CACHED_SPRITES];
} g_glyphs;

void sos_image_glyphs_arm_with(int style, const SosImageGlyphSite *site, const SosImageGlyphGame *game) {
    memset(&g_glyphs, 0, sizeof(g_glyphs));
    if (!site || !game || !hooks_of(style)) return;
    g_glyphs.style = style;
    g_glyphs.site = *site;
    g_glyphs.game = *game;
    g_glyphs.armed = 1;
}

void sos_image_glyphs_arm(int style, const uint8_t *image, const char *build_key) {
    const SosImageGlyphSite *site = sos_image_glyphs_site(build_key);
    if (!site || !image) {
        sos_image_glyphs_arm_with(style, NULL, NULL);
        return;
    }
    SosImageGlyphGame game;
    game.get_instance_id = (int32_t(SOS_IMAGE_GLYPHS_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_instance_id);
    game.get_name = (void *(SOS_IMAGE_GLYPHS_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_name);
    game.is_active = (uint8_t(SOS_IMAGE_GLYPHS_SYSV *)(void *, const void *))(uintptr_t)(image + site->is_active);
    game.set_override_sprite = (void(SOS_IMAGE_GLYPHS_SYSV *)(void *, void *, const void *))(uintptr_t)(image + site->set_override_sprite);
    game.get_overseer = (void *(SOS_IMAGE_GLYPHS_SYSV *)(const void *))(uintptr_t)(image + site->get_overseer);
    game.overseer_slot = (const uintptr_t *)(image + site->overseer_slot);
    game.runtime_slot = (const uintptr_t *)(image + site->runtime_slot);
    sos_image_glyphs_arm_with(style, site, &game);
}

static void *pointer_at(const void *object, size_t offset) {
    void *value;
    memcpy(&value, (const uint8_t *)object + offset, sizeof(value));
    return value;
}

static int32_t int_at(const void *object, size_t offset) {
    int32_t value;
    memcpy(&value, (const uint8_t *)object + offset, sizeof(value));
    return value;
}

static int alive(const void *object) { return object != NULL && pointer_at(object, NATIVE_OBJECT) != NULL; }

/* A metadata slot holds a tagged index until the game first uses it. */
static const void *resolved_slot(const uintptr_t *slot) {
    const uintptr_t value = *slot;
    return (value & 1) == 0 ? (const void *)value : NULL;
}

/* The lookup throws before GameRuntime exists, and Images are enabled earlier than that. */
static int runtime_ready(void) {
    const void *runtime = resolved_slot(g_glyphs.game.runtime_slot);
    const void *statics = runtime ? pointer_at(runtime, CLASS_STATICS) : NULL;
    const void *instance = statics ? pointer_at(statics, 0) : NULL;
    return instance && pointer_at(instance, RUNTIME_SUBSYSTEMS) != NULL;
}

/* The game's Xbox icons while its input comes from a gamepad; NULL otherwise. */
static const void *xbox_icons(void) {
    const void *method = resolved_slot(g_glyphs.game.overseer_slot);
    if (!method || !runtime_ready()) return NULL;
    const void *overseer = g_glyphs.game.get_overseer(method);
    if (!overseer || int_at(overseer, g_glyphs.site.input_device) != CONTROL_DEVICE_GAMEPAD) return NULL;
    const void *icons = pointer_at(overseer, g_glyphs.site.icon_data);
    return alive(icons) ? (const uint8_t *)icons + g_glyphs.site.xbox_icons : NULL;
}

/* Instance IDs are never reused, unlike object addresses. */
static int glyph_of_sprite(void *sprite) {
    if (!alive(sprite)) return SOS_GLYPH_NONE;
    const int32_t id = g_glyphs.game.get_instance_id(sprite, NULL);
    const uint32_t slot = ((uint32_t)id * UINT32_C(2654435761)) >> 22;
    if (id && g_glyphs.sprites[slot].id == id) return g_glyphs.sprites[slot].glyph;
    const void *name = g_glyphs.game.get_name(sprite, NULL);
    const int glyph = name ? sos_image_glyph_of_name((const uint16_t *)((const uint8_t *)name + STRING_CHARS), int_at(name, STRING_LENGTH))
                           : SOS_GLYPH_NONE;
    g_glyphs.sprites[slot].id = id;
    g_glyphs.sprites[slot].glyph = (int8_t)glyph;
    return glyph;
}

static void show(TrackedImage *tracked, const void *icons) {
    void *sprite = icons ? pointer_at(icons, (size_t)tracked->glyph * sizeof(void *)) : NULL;
    if (!alive(sprite)) sprite = NULL;
    if (tracked->shown && pointer_at(tracked->image, g_glyphs.site.override_sprite) != tracked->shown) tracked->shown = NULL;
    if (sprite == tracked->shown) return;
    if (!tracked->shown && pointer_at(tracked->image, g_glyphs.site.override_sprite)) return;
    tracked->shown = sprite;
    g_glyphs.game.set_override_sprite(tracked->image, sprite, NULL);
}

static const void *wanted_icons(void) {
    if (!sos_prompt_uses_xbox(g_glyphs.style, sos_pad_kind_current())) return NULL;
    const void *icons = xbox_icons();
    if (!icons) g_glyphs.pending = 1;
    return icons;
}

/* Follows a controller change, and the moment the icons become available, on the Images that are already showing. */
static void refresh(void) {
    const int wanted = sos_prompt_uses_xbox(g_glyphs.style, sos_pad_kind_current());
    if (wanted == g_glyphs.shown && !(g_glyphs.pending && ++g_glyphs.calls >= RETRY_CALLS)) return;
    g_glyphs.calls = 0;
    g_glyphs.pending = 0;
    g_glyphs.shown = wanted;
    if (!g_glyphs.count) return;
    const void *icons = wanted_icons();
    for (size_t i = 0; i < g_glyphs.count; ++i) show(&g_glyphs.images[i], icons);
}

static TrackedImage *find(const void *image) {
    for (size_t i = 0; i < g_glyphs.count; ++i)
        if (g_glyphs.images[i].image == image) return &g_glyphs.images[i];
    return NULL;
}

static void track(void *image, int glyph) {
    if (g_glyphs.count == TRACKED_IMAGES) return;
    TrackedImage *tracked = &g_glyphs.images[g_glyphs.count++];
    *tracked = (TrackedImage){image, NULL, glyph};
    show(tracked, wanted_icons());
}

static void forget(TrackedImage *tracked) { *tracked = g_glyphs.images[--g_glyphs.count]; }

static int enter(const void *image) {
    if (!g_glyphs.armed || g_glyphs.busy || !image) return 0;
    g_glyphs.busy = 1;
    return 1;
}

SOS_IMAGE_GLYPHS_SYSV void sos_image_glyphs_on_enable(void *image) {
    if (!enter(image)) return;
    refresh();
    if (alive(image) && !find(image)) {
        const int glyph = glyph_of_sprite(pointer_at(image, g_glyphs.site.sprite));
        if (glyph != SOS_GLYPH_NONE) track(image, glyph);
    }
    g_glyphs.busy = 0;
}

/* Never skipped: a tracked Image may be destroyed after this. */
SOS_IMAGE_GLYPHS_SYSV void sos_image_glyphs_on_disable(void *image) {
    if (!g_glyphs.armed) return;
    TrackedImage *tracked = find(image);
    if (!tracked) return;
    void *const none = NULL;
    if (tracked->shown && pointer_at(image, g_glyphs.site.override_sprite) == tracked->shown)
        memcpy((uint8_t *)image + g_glyphs.site.override_sprite, &none, sizeof(none));
    forget(tracked);
}

SOS_IMAGE_GLYPHS_SYSV void sos_image_glyphs_on_set_sprite(void *image, void *sprite) {
    if (!enter(image)) return;
    refresh();
    const int glyph = glyph_of_sprite(sprite);
    TrackedImage *tracked = find(image);
    if (tracked && glyph == SOS_GLYPH_NONE) {
        show(tracked, NULL);
        forget(tracked);
    } else if (tracked && glyph != tracked->glyph) {
        tracked->glyph = glyph;
        show(tracked, wanted_icons());
    } else if (!tracked && glyph != SOS_GLYPH_NONE && alive(image) && g_glyphs.game.is_active(image, NULL)) {
        track(image, glyph);
    }
    g_glyphs.busy = 0;
}
