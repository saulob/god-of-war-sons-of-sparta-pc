#include "controller_prompts.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "controller_prompts_sites.h"

typedef struct {
    const char *build;
    const SosPromptSite *sites;
    size_t count;
    const SosPromptHook *hook;
} BuildSites;

#define SITES(array) array, sizeof(array) / sizeof(*(array))

static const BuildSites kBuilds[] = {
    {"CUSA57767-1.00", SITES(kSites_1_00), &kHook_1_00},
    {"CUSA57767-1.01", SITES(kSites_1_01), &kHook_1_01},
};

enum { CONTROL_DEVICE_GAMEPAD = 2, GAMEPAD_XBOX_SERIES = 2 };

enum { SITE_NOT_VERIFIED = -1, SITE_ORIGINAL = 0, SITE_XBOX = 1 };

static const BuildSites *find_build(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(build_key, kBuilds[i].build)) return &kBuilds[i];
    return NULL;
}

/* What a site holds: its original bytes, its Xbox bytes, or neither (also when the bytes around it differ). */
static int site_state(const uint8_t *image, uint64_t size, const SosPromptSite *site) {
    const uint64_t at = site->offset;
    if (at < sizeof(site->before) || at + site->length + sizeof(site->after) > size) return SITE_NOT_VERIFIED;
    if (memcmp(image + at - sizeof(site->before), site->before, sizeof(site->before)) ||
        memcmp(image + at + site->length, site->after, sizeof(site->after)))
        return SITE_NOT_VERIFIED;
    if (!memcmp(image + at, site->original, site->length)) return SITE_ORIGINAL;
    if (!memcmp(image + at, site->xbox, site->length)) return SITE_XBOX;
    return SITE_NOT_VERIFIED;
}

static int all_sites_verified(const uint8_t *image, uint64_t size, const BuildSites *build, int *failed) {
    for (size_t i = 0; i < build->count; ++i) {
        if (site_state(image, size, &build->sites[i]) == SITE_NOT_VERIFIED) {
            *failed = (int)i + 1;
            return 0;
        }
    }
    return 1;
}

int sos_prompt_style_parse(const char *name, int *style) {
    int value;
    if (name && !strcmp(name, SOS_PROMPTS_AUTOMATIC_KEY)) value = SOS_PROMPTS_AUTOMATIC;
    else if (name && !strcmp(name, SOS_PROMPTS_PLAYSTATION_KEY)) value = SOS_PROMPTS_PLAYSTATION;
    else if (name && !strcmp(name, SOS_PROMPTS_XBOX_KEY)) value = SOS_PROMPTS_XBOX;
    else return 0;
    if (style) *style = value;
    return 1;
}

const char *sos_prompt_style_label(int style) {
    if (style == SOS_PROMPTS_XBOX) return "Xbox";
    return style == SOS_PROMPTS_AUTOMATIC ? "Automatic" : "PlayStation";
}

int sos_controller_prompts_supported(const char *build_key) { return find_build(build_key) != NULL; }

int sos_controller_prompts_from_file(const char *settings_file) {
    int style = SOS_PROMPTS_AUTOMATIC;
    FILE *file = settings_file && *settings_file ? fopen(settings_file, "rb") : NULL;
    if (!file) return style;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *text = line;
        while (isspace((unsigned char)*text)) ++text;
        char *equals = strchr(text, '=');
        if (*text == '#' || !equals) continue;
        char *end = equals;
        while (end > text && isspace((unsigned char)end[-1])) --end;
        *end = 0;
        if (strcmp(text, SOS_PROMPTS_KEY)) continue;
        char *value = equals + 1;
        while (isspace((unsigned char)*value)) ++value;
        char *stop = strstr(value, " #");
        if (!stop) stop = value + strlen(value);
        while (stop > value && isspace((unsigned char)stop[-1])) --stop;
        *stop = 0;
        int parsed;
        style = sos_prompt_style_parse(value, &parsed) ? parsed : SOS_PROMPTS_AUTOMATIC;
    }
    fclose(file);
    return style;
}

int sos_controller_prompts_state(const uint8_t *image, uint64_t size, const char *build_key) {
    const BuildSites *build = find_build(build_key);
    if (!build) return -1;
    int xbox = 0;
    for (size_t i = 0; i < build->count; ++i) {
        const int state = site_state(image, size, &build->sites[i]);
        if (state == SITE_NOT_VERIFIED) return -1;
        xbox += state == SITE_XBOX;
    }
    if (xbox == 0) return SOS_PROMPTS_PLAYSTATION;
    return (size_t)xbox == build->count ? SOS_PROMPTS_XBOX : -1;
}

int sos_controller_prompts_apply(uint8_t *image, uint64_t size, const char *build_key, int style, char *message,
                                 size_t message_size) {
    const BuildSites *build = find_build(build_key);
    const char *label = sos_prompt_style_label(style);
    if (!build) {
        snprintf(message, message_size, "%s requested but unavailable: no verified code in %s", label,
                 build_key ? build_key : "this build");
        return -1;
    }
    int failed = 0;
    if (!all_sites_verified(image, size, build, &failed)) {
        snprintf(message, message_size, "%s requested but unavailable: site %d of %s is not the verified code", label, failed,
                 build->build);
        return -1;
    }
    const int xbox = style == SOS_PROMPTS_XBOX;
    for (size_t i = 0; i < build->count; ++i) {
        const SosPromptSite *site = &build->sites[i];
        memcpy(image + site->offset, xbox ? site->xbox : site->original, site->length);
    }
    snprintf(message, message_size, "%s (%s)", label, xbox ? "game prompts set to Xbox" : "the game's own code");
    return xbox ? SOS_PROMPTS_XBOX : SOS_PROMPTS_PLAYSTATION;
}

const SosPromptSite *sos_controller_prompts_sites(const char *build_key, size_t *count) {
    const BuildSites *build = find_build(build_key);
    if (count) *count = build ? build->count : 0;
    return build ? build->sites : NULL;
}

size_t sos_controller_prompts_value_ranges(const uint8_t *image, uint64_t size, const char *build_key, uint64_t *offsets,
                                           uint8_t *lengths, size_t capacity) {
    const BuildSites *build = find_build(build_key);
    int failed = 0;
    size_t count = 0;
    if (!build || !all_sites_verified(image, size, build, &failed)) return 0;
    for (size_t i = 0; i < build->count; ++i) {
        if (!build->sites[i].immediate) continue;
        if (count == capacity) return 0;
        offsets[count] = build->sites[i].offset;
        lengths[count++] = build->sites[i].length;
    }
    return count;
}

int sos_prompt_uses_xbox(int style, int pad_kind) {
    return style == SOS_PROMPTS_XBOX || (style == SOS_PROMPTS_AUTOMATIC && pad_kind == SOS_PAD_XBOX);
}

static atomic_int g_pad_kind = SOS_PAD_NONE;

void sos_pad_kind_publish(int pad_kind) { atomic_store_explicit(&g_pad_kind, pad_kind, memory_order_release); }

int sos_pad_kind_current(void) { return atomic_load_explicit(&g_pad_kind, memory_order_acquire); }

const SosPromptHook *sos_controller_prompts_hook(const char *build_key) {
    const BuildSites *build = find_build(build_key);
    return build ? build->hook : NULL;
}

void sos_prompt_override_step(SosPromptOverride *state, int xbox, int32_t input_device, int32_t *controller_used) {
    if (xbox && input_device == CONTROL_DEVICE_GAMEPAD) {
        if (*controller_used == GAMEPAD_XBOX_SERIES) return;
        if (!state->replaced) state->game_value = *controller_used;
        state->replaced = 1;
        *controller_used = GAMEPAD_XBOX_SERIES;
    } else if (state->replaced) {
        if (*controller_used == GAMEPAD_XBOX_SERIES) *controller_used = state->game_value;
        state->replaced = 0;
    }
}

static uint64_t fnv1a(const uint8_t *data, size_t size) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

int sos_prompt_hook_verify(const uint8_t *image, uint64_t size, const char *build_key, const char **why) {
    const SosPromptHook *hook = sos_controller_prompts_hook(build_key);
    const char *reason = NULL;
    if (!hook) reason = "no verified prompt hook in this build";
    else if ((uint64_t)hook->method + hook->length > size) reason = "the image ends before the hooked method";
    else if (hook->length < SOS_PROMPT_HOOK_STOLEN || memcmp(image + hook->method, hook->prologue, SOS_PROMPT_HOOK_STOLEN))
        reason = "the hooked method does not start as verified";
    else if (fnv1a(image + hook->method, hook->length) != hook->hash) reason = "the hooked method is not the verified code";
    if (reason && why) *why = reason;
    return reason == NULL;
}

static struct {
    int style;
    uint32_t input_device, controller_used;
    SosPromptOverride state;
} g_hook = {SOS_PROMPTS_PLAYSTATION, 0x30, 0x34, {0, 0}};

void sos_prompt_hook_arm(int style, const char *build_key) {
    const SosPromptHook *hook = sos_controller_prompts_hook(build_key);
    g_hook.style = style;
    g_hook.state = (SosPromptOverride){0, 0};
    if (!hook) return;
    g_hook.input_device = hook->input_device;
    g_hook.controller_used = hook->controller_used;
}

/* Called from the hooked method's stub on the game thread, with the InputOverseer in rdi. */
__attribute__((sysv_abi)) void sos_prompt_hook_handler(uint8_t *overseer) {
    if (!overseer) return;
    int32_t device, used;
    memcpy(&device, overseer + g_hook.input_device, sizeof(device));
    memcpy(&used, overseer + g_hook.controller_used, sizeof(used));
    const int32_t before = used;
    sos_prompt_override_step(&g_hook.state, sos_prompt_uses_xbox(g_hook.style, sos_pad_kind_current()), device, &used);
    if (used != before) memcpy(overseer + g_hook.controller_used, &used, sizeof(used));
}

/* push rdi, rsi, rdx, rcx, r8, r9 ; sub rsp,8 (the method is entered with rsp 8 off 16 and six pushes keep it so) ;
 * mov rax,handler ; call rax ; add rsp,8 ; pop them. The method's own first 6 bytes and a jump back follow. */
static const uint8_t kStubHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
static const uint8_t kStubTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
enum { STUB_SIZE = sizeof(kStubHead) + 8 + sizeof(kStubTail) + SOS_PROMPT_HOOK_STOLEN + 5 };
_Static_assert(STUB_SIZE <= SOS_PROMPT_HOOK_STUB_BYTES, "the prompt hook stub fits");

static int reaches(const uint8_t *from_end, const uint8_t *to) {
    const int64_t delta = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from_end;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

int sos_prompt_hook_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stub, size_t stub_capacity,
                            char *message, size_t message_size) {
    const char *why = "";
    if (!sos_prompt_hook_verify(image, size, build_key, &why)) {
        snprintf(message, message_size, "not applied: %s in %s", why, build_key ? build_key : "this build");
        return 0;
    }
    const SosPromptHook *hook = sos_controller_prompts_hook(build_key);
    uint8_t *method = image + hook->method;
    if (!stub || stub_capacity < STUB_SIZE || !reaches(method + 5, stub) ||
        !reaches(stub + STUB_SIZE, method + SOS_PROMPT_HOOK_STOLEN)) {
        snprintf(message, message_size, "not applied: no room for the stub within reach of the image");
        return 0;
    }
    const uint64_t handler = (uint64_t)(uintptr_t)sos_prompt_hook_handler;
    uint8_t *at = stub;
    memcpy(at, kStubHead, sizeof(kStubHead));
    at += sizeof(kStubHead);
    memcpy(at, &handler, sizeof(handler));
    at += sizeof(handler);
    memcpy(at, kStubTail, sizeof(kStubTail));
    at += sizeof(kStubTail);
    memcpy(at, method, SOS_PROMPT_HOOK_STOLEN);
    at += SOS_PROMPT_HOOK_STOLEN;
    const int32_t back = (int32_t)((int64_t)(intptr_t)(method + SOS_PROMPT_HOOK_STOLEN) - (int64_t)(intptr_t)(at + 5));
    *at++ = 0xe9;
    memcpy(at, &back, sizeof(back));
    const int32_t into = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(method + 5));
    method[0] = 0xe9;
    memcpy(method + 1, &into, sizeof(into));
    method[5] = 0x90;
    snprintf(message, message_size, "Automatic prompt hook installed at image+0x%x", hook->method);
    return 1;
}
