#include "prompt_dialog.h"

#include <stdio.h>
#include <string.h>

#include "controller_prompts.h"
#include "prompt_dialog_sites.h"

enum { CONTROL_DEVICE_GAMEPAD = 2, KEPT_BYTES = 11, FLAG_COMPARE_AT = 11, FLAG_COMPARE_BYTES = 7 };
enum { NATIVE_OBJECT = 0x10, LIST_ITEMS = 0x10, LIST_SIZE = 0x18, ARRAY_LENGTH = 0x18, ARRAY_ITEMS = 0x20 };
enum { CONFIRM_LABEL = 2, BACK_LABEL = 3 };

typedef struct {
    const char *build;
    const SosPromptDialogSite *site;
} BuildSite;

static const BuildSite kBuilds[] = {
    {"CUSA57767-1.00", &kDialog_1_00},
    {"CUSA57767-1.01", &kDialog_1_01},
};

const SosPromptDialogSite *sos_prompt_dialog_site(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(build_key, kBuilds[i].build)) return kBuilds[i].site;
    return NULL;
}

static uint64_t fnv1a(uint64_t hash, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

int sos_prompt_dialog_verify(const uint8_t *image, uint64_t size, const char *build_key, const char **why) {
    const SosPromptDialogSite *site = sos_prompt_dialog_site(build_key);
    const char *reason = NULL;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    if (!site) reason = "no verified dialog hook in this build";
    for (int i = 0; !reason && i < SOS_PROMPT_DIALOG_RANGES; ++i) {
        const SosPromptDialogRange range = site->ranges[i];
        if (range.begin >= range.end || range.end > size) reason = "the image ends before the dialog code";
        else hash = fnv1a(hash, image + range.begin, range.end - range.begin);
    }
    if (!reason && memcmp(image + site->method, site->entry, SOS_PROMPT_DIALOG_ENTRY_BYTES)) reason = "the dialog method does not start as verified";
    else if (!reason && hash != site->hash) reason = "the dialog code is not the verified code";
    if (reason && why) *why = reason;
    return reason == NULL;
}

/* push rdi, rsi, rdx, rcx, r8, r9 ; sub rsp,8 (the method is entered with rsp 8 off 16 and six pushes keep it so) ;
 * mov rax,handler ; call rax ; add rsp,8 ; pop them. */
static const uint8_t kStubHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
static const uint8_t kStubTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
/* The replayed entry: the eleven bytes of pushes, then mov rax,flag ; cmp byte [rax],0 in place of the RIP-relative compare.
 * rax holds nothing at the method's entry and is written before it is read. */
static const uint8_t kLoadFlag[] = {0x48, 0xb8};
static const uint8_t kCompareFlag[] = {0x80, 0x38, 0x00};
enum {
    BODY_SIZE = KEPT_BYTES + sizeof(kLoadFlag) + 8 + sizeof(kCompareFlag),
    STUB_SIZE = sizeof(kStubHead) + 8 + sizeof(kStubTail) + BODY_SIZE + 5
};
_Static_assert(STUB_SIZE <= SOS_PROMPT_DIALOG_STUB_BYTES, "the dialog hook stub fits");

static int reaches(const uint8_t *from_end, const uint8_t *to) {
    const int64_t delta = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from_end;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

int sos_prompt_dialog_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stub, size_t stub_capacity,
                              char *message, size_t message_size) {
    const char *why = "";
    if (!sos_prompt_dialog_verify(image, size, build_key, &why)) {
        snprintf(message, message_size, "not applied: %s in %s", why, build_key ? build_key : "this build");
        return 0;
    }
    const SosPromptDialogSite *site = sos_prompt_dialog_site(build_key);
    uint8_t *method = image + site->method;
    int32_t displacement;
    memcpy(&displacement, method + FLAG_COMPARE_AT + 2, sizeof(displacement));
    const int64_t flag_offset = (int64_t)site->method + SOS_PROMPT_DIALOG_ENTRY_BYTES + displacement;
    if (flag_offset < 0 || (uint64_t)flag_offset >= size) {
        snprintf(message, message_size, "not applied: the method's flag lies outside the image");
        return 0;
    }
    if (!stub || stub_capacity < STUB_SIZE || !reaches(method + 5, stub) ||
        !reaches(stub + STUB_SIZE, method + SOS_PROMPT_DIALOG_ENTRY_BYTES)) {
        snprintf(message, message_size, "not applied: no room for the stub within reach of the image");
        return 0;
    }
    const uint64_t handler = (uint64_t)(uintptr_t)sos_prompt_dialog_handler;
    const uint64_t flag = (uint64_t)(uintptr_t)(image + flag_offset);
    uint8_t *at = stub;
    memcpy(at, kStubHead, sizeof(kStubHead));
    at += sizeof(kStubHead);
    memcpy(at, &handler, sizeof(handler));
    at += sizeof(handler);
    memcpy(at, kStubTail, sizeof(kStubTail));
    at += sizeof(kStubTail);
    memcpy(at, method, KEPT_BYTES);
    at += KEPT_BYTES;
    memcpy(at, kLoadFlag, sizeof(kLoadFlag));
    at += sizeof(kLoadFlag);
    memcpy(at, &flag, sizeof(flag));
    at += sizeof(flag);
    memcpy(at, kCompareFlag, sizeof(kCompareFlag));
    at += sizeof(kCompareFlag);
    const int32_t back = (int32_t)((int64_t)(intptr_t)(method + SOS_PROMPT_DIALOG_ENTRY_BYTES) - (int64_t)(intptr_t)(at + 5));
    *at++ = 0xe9;
    memcpy(at, &back, sizeof(back));
    const int32_t into = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(method + 5));
    method[0] = 0xe9;
    memcpy(method + 1, &into, sizeof(into));
    memset(method + 5, 0x90, SOS_PROMPT_DIALOG_ENTRY_BYTES - 5);
    snprintf(message, message_size, "Settings dialog glyphs hook installed at image+0x%x", site->method);
    return 1;
}

static struct {
    int style, overridden;
    SosPromptDialogSite site;
    SosPromptDialogGame game;
    int armed;
} g_dialog;

void sos_prompt_dialog_arm_with(int style, const SosPromptDialogSite *site, const SosPromptDialogGame *game) {
    memset(&g_dialog, 0, sizeof(g_dialog));
    if (!site || !game) return;
    g_dialog.style = style;
    g_dialog.site = *site;
    g_dialog.game = *game;
    g_dialog.armed = 1;
}

void sos_prompt_dialog_arm(int style, const uint8_t *image, const char *build_key) {
    const SosPromptDialogSite *site = sos_prompt_dialog_site(build_key);
    if (!site || !image) {
        sos_prompt_dialog_arm_with(style, NULL, NULL);
        return;
    }
    SosPromptDialogGame game;
    game.get_component = (void *(SOS_PROMPT_DIALOG_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_component);
    game.get_overseer = (void *(SOS_PROMPT_DIALOG_SYSV *)(const void *))(uintptr_t)(image + site->get_overseer);
    game.set_override_sprite = (void (SOS_PROMPT_DIALOG_SYSV *)(void *, void *, const void *))(uintptr_t)(image + site->set_override_sprite);
    game.transform_of = (void *(SOS_PROMPT_DIALOG_SYSV *)(void *, const void *))(uintptr_t)(image + site->transform_of);
    game.parent_of = (void *(SOS_PROMPT_DIALOG_SYSV *)(void *, const void *))(uintptr_t)(image + site->parent_of);
    game.child_count = (int (SOS_PROMPT_DIALOG_SYSV *)(void *, const void *))(uintptr_t)(image + site->child_count);
    game.child_at = (void *(SOS_PROMPT_DIALOG_SYSV *)(void *, int, const void *))(uintptr_t)(image + site->child_at);
    game.image_slot = (const uintptr_t *)(image + site->image_slot);
    game.overseer_slot = (const uintptr_t *)(image + site->overseer_slot);
    sos_prompt_dialog_arm_with(style, site, &game);
}

static const uint8_t *bytes_of(const void *object) { return (const uint8_t *)object; }

static void *pointer_at(const void *object, size_t offset) {
    void *value;
    memcpy(&value, bytes_of(object) + offset, sizeof(value));
    return value;
}

static int32_t int_at(const void *object, size_t offset) {
    int32_t value;
    memcpy(&value, bytes_of(object) + offset, sizeof(value));
    return value;
}

static int alive(const void *object) { return object != NULL && pointer_at(object, NATIVE_OBJECT) != NULL; }

/* A metadata slot holds the game's resolved MethodInfo, or a tagged index until its first use. */
static const void *resolved_slot(const uintptr_t *slot) {
    if (!slot) return NULL;
    const uintptr_t value = *slot;
    return (value & 1) == 0 ? (const void *)value : NULL;
}

/* The glyph Image next to a dialog label: the one sibling that has an Image. */
static void *glyph_image_of(void *label, const void *image_method) {
    const SosPromptDialogGame *game = &g_dialog.game;
    void *const transform = alive(label) ? game->transform_of(label, NULL) : NULL;
    void *const parent = alive(transform) ? game->parent_of(transform, NULL) : NULL;
    if (!alive(parent)) return NULL;
    void *found = NULL;
    const int children = game->child_count(parent, NULL);
    for (int i = 0; i < children; ++i) {
        void *const child = game->child_at(parent, i, NULL);
        void *const image = alive(child) ? game->get_component(child, image_method) : NULL;
        if (!image) continue;
        if (found) return NULL;
        found = image;
    }
    return alive(found) ? found : NULL;
}

static void *label_at(const void *prompt, size_t index) {
    void *const list = pointer_at(prompt, g_dialog.site.text_list);
    if (!list || index >= (size_t)int_at(list, LIST_SIZE)) return NULL;
    void *const items = pointer_at(list, LIST_ITEMS);
    if (!items || index >= (size_t)int_at(items, ARRAY_LENGTH)) return NULL;
    return pointer_at(items, ARRAY_ITEMS + index * sizeof(void *));
}

/* The glyphs are set as Image.overrideSprite, so clearing it brings back the dialog's own PlayStation sprite. */
static void set_glyph(void *image, void *sprite) {
    if (sprite) {
        g_dialog.game.set_override_sprite(image, sprite, NULL);
        g_dialog.overridden = 1;
    } else if (pointer_at(image, g_dialog.site.override_sprite)) {
        g_dialog.game.set_override_sprite(image, NULL, NULL);
    }
}

static int xbox_glyphs_active(const void *overseer) {
    return sos_prompt_uses_xbox(g_dialog.style, sos_pad_kind_current()) && int_at(overseer, g_dialog.site.input_device) == CONTROL_DEVICE_GAMEPAD;
}

/* Called from the hooked method's stub on the game thread, with the PromptTextSwitch in rdi, before the dialog's text is set. */
SOS_PROMPT_DIALOG_SYSV void sos_prompt_dialog_handler(void *prompt) {
    if (!g_dialog.armed) return;
    if (!g_dialog.overridden && !sos_prompt_uses_xbox(g_dialog.style, sos_pad_kind_current())) return;
    const void *const overseer_method = resolved_slot(g_dialog.game.overseer_slot);
    const void *const image_method = resolved_slot(g_dialog.game.image_slot);
    if (!alive(prompt) || !overseer_method || !image_method) return;
    void *const overseer = g_dialog.game.get_overseer(overseer_method);
    void *const icons = overseer ? pointer_at(overseer, g_dialog.site.icon_data) : NULL;
    if (!alive(icons)) return;
    const int xbox = xbox_glyphs_active(overseer);
    void *const confirm = xbox ? pointer_at(icons, g_dialog.site.xbox_confirm) : NULL;
    void *const back = xbox ? pointer_at(icons, g_dialog.site.xbox_back) : NULL;
    if (xbox && (!alive(confirm) || !alive(back))) return;
    void *const confirm_image = glyph_image_of(label_at(prompt, CONFIRM_LABEL), image_method);
    void *const back_image = glyph_image_of(label_at(prompt, BACK_LABEL), image_method);
    if (xbox && (!confirm_image || !back_image || confirm_image == back_image)) return;
    if (confirm_image) set_glyph(confirm_image, confirm);
    if (back_image) set_glyph(back_image, back);
}
