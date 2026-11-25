#include "sos_pit_unlock.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define ANY (-1)

/* r14b = isPitUnlocked, r15b = isHermesModeUnlocked: each skips its own event, and both together end the cheat input. */
static const short kGate[] = {
    0x44, 0x0f, 0xb6, 0x77, 0x1e, 0x44, 0x0f, 0xb6, 0x7f, 0x41, 0x45, 0x84, 0xf6, 0x74, 0x13,
    0x48, 0x8b, 0xbb, 0xe0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xff, 0x74, 0x07, 0x31, 0xf6, 0xe8, ANY, ANY, ANY, ANY,
    0x45, 0x84, 0xff, 0x74, 0x13,
    0x48, 0x8b, 0xbb, 0xe8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xff, 0x74, 0x07, 0x31, 0xf6, 0xe8, ANY, ANY, ANY, ANY,
    0x45, 0x84, 0xf7, 0x0f, 0x84, 0x85, 0x00, 0x00, 0x00,
};
enum { kGateLength = sizeof(kGate) / sizeof(*kGate), kJumpAt = 13, kJumpLength = 2, kPitCallAt = 29, kHermesCallAt = 53 };

static const SosPitUnlockSite kSites[] = {
    {"CUSA57767-1.00", 168044704u, 0x4c22879, 0x90e2640},
    {"CUSA57767-1.01", 168046032u, 0x4c290b9, 0x90f9b20},
};

/* Unity build indices of the Pit scenes, the same in both builds. */
static const struct {
    int first, last;
} kScenes[] = {{175, 200}, {202, 219}};

static const char *const kBanks[] = {
    "StreamingAssets/Scenes/PitofAgonies.bank",
    "StreamingAssets/Scenes/PitofAgonies.assets.bank",
    "StreamingAssets/Scenes/PitofAgonies.streams.bank",
};

const SosPitUnlockSite *sos_pit_unlock_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

static int calls(const uint8_t *image, uint64_t at, uint32_t target) {
    int32_t rel;
    memcpy(&rel, image + at + 1, sizeof(rel));
    return (int64_t)at + 5 + rel == (int64_t)target;
}

int sos_pit_unlock_verify(const uint8_t *image, uint64_t size, const SosPitUnlockSite *site, const char **why) {
    const char *reason = NULL;
    if (!site)
        reason = "not a supported build";
    else if (size != site->image_size)
        reason = "the image is not the verified size";
    else if (site->gate < kJumpAt || site->gate > size || size - site->gate < kGateLength - kJumpAt)
        reason = "the title menu unlock check is outside the image";
    else {
        const uint64_t start = site->gate - kJumpAt;
        for (int i = 0; i < kGateLength; ++i)
            if (kGate[i] != ANY && image[start + i] != kGate[i]) reason = "the title menu unlock check differs";
        if (!reason && (!calls(image, start + kPitCallAt, site->invoke) || !calls(image, start + kHermesCallAt, site->invoke)))
            reason = "the title menu unlock event differs";
    }
    if (reason && why) *why = reason;
    return reason == NULL;
}

static int file_present(const char *folder, const char *name) {
    char path[1200];
    const int length = snprintf(path, sizeof(path), "%s/Media/%s", folder, name);
    if (length < 0 || (size_t)length >= sizeof(path)) return 0;
    FILE *file = fopen(path, "rb");
    if (file) fclose(file);
    return file != NULL;
}

static int report_missing(char *missing, size_t missing_size, const char *name) {
    if (missing && missing_size) snprintf(missing, missing_size, "%s", name);
    return 0;
}

int sos_pit_unlock_content(const char *game_folder, char *missing, size_t missing_size) {
    if (!game_folder || !*game_folder) return report_missing(missing, missing_size, "the game folder");
    for (size_t r = 0; r < sizeof(kScenes) / sizeof(*kScenes); ++r)
        for (int scene = kScenes[r].first; scene <= kScenes[r].last; ++scene) {
            char name[64];
            snprintf(name, sizeof(name), "level%d", scene);
            if (!file_present(game_folder, name)) return report_missing(missing, missing_size, name);
            snprintf(name, sizeof(name), "sharedassets%d.assets", scene);
            if (!file_present(game_folder, name)) return report_missing(missing, missing_size, name);
        }
    for (size_t i = 0; i < sizeof(kBanks) / sizeof(*kBanks); ++i)
        if (!file_present(game_folder, kBanks[i])) return report_missing(missing, missing_size, kBanks[i]);
    return 1;
}

static int true_value(const char *value) {
    return !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "yes") || !strcmp(value, "true");
}

static int false_value(const char *value) {
    return !strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "no") || !strcmp(value, "false");
}

int sos_pit_unlock_from_file(const char *settings_file) {
    int on = 0;
    FILE *file = settings_file && *settings_file ? fopen(settings_file, "rb") : NULL;
    if (!file) return 0;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *text = line;
        while (isspace((unsigned char)*text)) ++text;
        char *equals = strchr(text, '=');
        if (*text == '#' || !equals) continue;
        char *end = equals;
        while (end > text && isspace((unsigned char)end[-1])) --end;
        *end = 0;
        if (strcmp(text, SOS_PIT_UNLOCK_KEY)) continue;
        char *value = equals + 1;
        while (isspace((unsigned char)*value)) ++value;
        char *stop = strstr(value, " #");
        if (!stop) stop = value + strlen(value);
        while (stop > value && isspace((unsigned char)stop[-1])) --stop;
        *stop = 0;
        if (true_value(value)) on = 1;
        else if (false_value(value)) on = 0;
    }
    fclose(file);
    return on;
}

int sos_pit_unlock_apply(uint8_t *image, uint64_t size, const char *build, const char *game_folder, char *message,
                         size_t message_size) {
    const SosPitUnlockSite *site = sos_pit_unlock_site(build);
    const char *why = "";
    char missing[64];
    if (!site) {
        snprintf(message, message_size, "not applied: not a verified game build");
        return 0;
    }
    if (!sos_pit_unlock_verify(image, size, site, &why)) {
        snprintf(message, message_size, "not applied: %s in %s; the game's own unlock is kept", why, build);
        return 0;
    }
    if (!sos_pit_unlock_content(game_folder, missing, sizeof(missing))) {
        snprintf(message, message_size, "not applied: %s is missing from the installed game", missing);
        return 0;
    }
    memset(image + site->gate, 0x90, kJumpLength);
    snprintf(message, message_size, "offered in the title menu");
    return 1;
}
