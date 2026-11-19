#include "controller_prompts.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "controller_prompts_sites.h"

typedef struct {
    const char *build;
    const SosPromptSite *sites;
    size_t count;
} BuildSites;

#define SITES(array) array, sizeof(array) / sizeof(*(array))

static const BuildSites kBuilds[] = {
    {"CUSA57767-1.00", SITES(kSites_1_00)},
    {"CUSA57767-1.01", SITES(kSites_1_01)},
};

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
    for (size_t i = 0; i < build->count; ++i) {
        const SosPromptSite *site = &build->sites[i];
        memcpy(image + site->offset, style == SOS_PROMPTS_XBOX ? site->xbox : site->original, site->length);
    }
    snprintf(message, message_size, "%s (%s)", label,
             style == SOS_PROMPTS_XBOX ? "game prompts set to Xbox" : "the game's own");
    return style;
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
