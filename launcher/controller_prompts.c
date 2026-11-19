#include "controller_prompts.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { GAMEPAD_XBOX_SERIES = 2, MAX_SIGNATURE = 48, SITE_COUNT = 4 };

typedef struct {
    uint8_t bytes[MAX_SIGNATURE];
    uint8_t length;
    uint8_t before; /* signature bytes before the instruction */
    uint8_t original;
} Signature;

typedef struct {
    const char *build;
    uint32_t at[SITE_COUNT]; /* image offset of each store instruction */
} BuildSites;

/* Constructor default, Initialize, DetermineController and OnInputActionChange. The immediate is the instruction's 4th byte. */
static const Signature kSignatures[SITE_COUNT] = {
    {{0x48, 0x8b, 0x47, 0x10, 0xc3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc7, 0x47, 0x34, 0x04, 0, 0, 0, 0xc6, 0x47, 0x10,
      0x01, 0xc3},
     28, 16, 4},
    {{0x30, 0x01, 0x00, 0x00, 0x72, 0x22, 0x48, 0x8b, 0x92, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x39, 0x4c, 0xf2, 0xf8, 0x75,
      0x14, 0x48, 0x8d, 0x7b, 0x38, 0xc7, 0x43, 0x34, 0x03, 0, 0, 0, 0x48, 0x89, 0x43, 0x38},
     35, 24, 3},
    {{0x00, 0x72, 0x0e, 0x48, 0x8b, 0x92, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x39, 0x4c, 0xf2, 0xf8, 0x74, 0x07, 0x48, 0x83,
      0xc4, 0x08, 0x5b, 0x5d, 0xc3, 0xc7, 0x43, 0x34, 0x03, 0, 0, 0, 0x48, 0x8d, 0x7b, 0x38, 0x48, 0x89, 0x43, 0x38},
     39, 24, 3},
    {{0xff, 0xff, 0x48, 0x8b, 0x92, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x39, 0x4c, 0xf2, 0xf8, 0x0f, 0x85, 0x52, 0xfd, 0xff,
      0xff, 0x49, 0x8d, 0x7e, 0x38, 0x41, 0xc7, 0x01, 0x03, 0, 0, 0, 0x48, 0x89, 0x07},
     34, 24, 3},
};

static const BuildSites kBuilds[] = {
    {"CUSA57767-1.00", {0x47c0710u, 0x47bce56u, 0x47bcf4cu, 0x47c0543u}},
    {"CUSA57767-1.01", {0x47c59a0u, 0x47c20e6u, 0x47c21dcu, 0x47c57d3u}},
};

static const BuildSites *find_build(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(build_key, kBuilds[i].build)) return &kBuilds[i];
    return NULL;
}

static uint8_t value_of(int style, uint8_t original) {
    return style == SOS_PROMPTS_XBOX ? GAMEPAD_XBOX_SERIES : original;
}

/* Where the immediate of a site is, or NULL when the bytes around it are not the verified code. The immediate itself may
 * hold the original or the Xbox value. */
static uint8_t *immediate(uint8_t *image, uint64_t size, uint32_t at, const Signature *signature) {
    const uint64_t start = (uint64_t)at - signature->before;
    if (at < signature->before || start + signature->length > size) return NULL;
    const uint8_t imm = signature->before + 3;
    for (uint8_t i = 0; i < signature->length; ++i) {
        if (i == imm) {
            const uint8_t held = image[start + i];
            if (held != signature->original && held != GAMEPAD_XBOX_SERIES) return NULL;
        } else if (image[start + i] != signature->bytes[i]) {
            return NULL;
        }
    }
    return image + start + imm;
}

int sos_prompt_style_parse(const char *name, int *style) {
    int value;
    if (name && !strcmp(name, SOS_PROMPTS_PLAYSTATION_KEY)) value = SOS_PROMPTS_PLAYSTATION;
    else if (name && !strcmp(name, SOS_PROMPTS_XBOX_KEY)) value = SOS_PROMPTS_XBOX;
    else return 0;
    if (style) *style = value;
    return 1;
}

const char *sos_prompt_style_label(int style) {
    return style == SOS_PROMPTS_XBOX ? "Xbox" : "PlayStation (Original)";
}

int sos_controller_prompts_supported(const char *build_key) { return find_build(build_key) != NULL; }

int sos_controller_prompts_from_file(const char *settings_file) {
    int style = SOS_PROMPTS_PLAYSTATION;
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
        style = sos_prompt_style_parse(value, &parsed) ? parsed : SOS_PROMPTS_PLAYSTATION;
    }
    fclose(file);
    return style;
}

int sos_controller_prompts_state(const uint8_t *image, uint64_t size, const char *build_key) {
    const BuildSites *build = find_build(build_key);
    if (!build) return -1;
    int xbox = 0;
    for (int i = 0; i < SITE_COUNT; ++i) {
        const uint8_t *held = immediate((uint8_t *)image, size, build->at[i], &kSignatures[i]);
        if (!held) return -1;
        xbox += *held == GAMEPAD_XBOX_SERIES;
    }
    if (xbox == 0) return SOS_PROMPTS_PLAYSTATION;
    return xbox == SITE_COUNT ? SOS_PROMPTS_XBOX : -1;
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
    uint8_t *held[SITE_COUNT];
    for (int i = 0; i < SITE_COUNT; ++i) {
        held[i] = immediate(image, size, build->at[i], &kSignatures[i]);
        if (!held[i]) {
            snprintf(message, message_size, "%s requested but unavailable: site %d of %s is not the verified code", label,
                     i + 1, build->build);
            return -1;
        }
    }
    for (int i = 0; i < SITE_COUNT; ++i) *held[i] = value_of(style, kSignatures[i].original);
    snprintf(message, message_size, "%s (%s)", label, style == SOS_PROMPTS_XBOX ? "game prompts set to Xbox" : "the game's own");
    return style;
}
