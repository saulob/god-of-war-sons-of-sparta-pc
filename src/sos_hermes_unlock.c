#include "sos_hermes_unlock.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* cmpb $0,0x40(%rdi); jne show; cmpb $0,0x41(%rdi); je hide */
static const uint8_t kGate[] = {0x80, 0x7f, 0x40, 0x00, 0x75, 0x0a, 0x80, 0x7f, 0x41, 0x00, 0x0f, 0x84, 0x05, 0x01, 0x00, 0x00};
enum { kJumpAt = 10, kJumpLength = 6 };

static const SosHermesUnlockSite kSites[] = {
    {"CUSA57767-1.00", 168044704u, 0x4be86d2, 0x4be87dd},
    {"CUSA57767-1.01", 168046032u, 0x4beef12, 0x4bef01d},
};

const SosHermesUnlockSite *sos_hermes_unlock_site(const char *build) {
    for (size_t i = 0; build && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build, kSites[i].build)) return &kSites[i];
    return NULL;
}

int sos_hermes_unlock_verify(const uint8_t *image, uint64_t size, const SosHermesUnlockSite *site, const char **why) {
    const char *reason = NULL;
    if (!site)
        reason = "not a supported build";
    else if (size != site->image_size)
        reason = "the image is not the verified size";
    else if (site->gate < kJumpAt || site->gate > size || size - site->gate < sizeof(kGate) - kJumpAt)
        reason = "the game mode selection check is outside the image";
    else if (memcmp(image + site->gate - kJumpAt, kGate, sizeof(kGate)))
        reason = "the game mode selection check differs";
    else {
        int32_t rel;
        memcpy(&rel, image + site->gate + 2, sizeof(rel));
        if ((int64_t)site->gate + kJumpLength + rel != (int64_t)site->hide)
            reason = "the hidden Hermes branch differs";
    }
    if (reason && why) *why = reason;
    return reason == NULL;
}

static int true_value(const char *value) {
    return !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "yes") || !strcmp(value, "true");
}

static int false_value(const char *value) {
    return !strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "no") || !strcmp(value, "false");
}

int sos_hermes_unlock_from_file(const char *settings_file) {
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
        if (strcmp(text, SOS_HERMES_UNLOCK_KEY)) continue;
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

int sos_hermes_unlock_apply(uint8_t *image, uint64_t size, const char *build, char *message, size_t message_size) {
    const SosHermesUnlockSite *site = sos_hermes_unlock_site(build);
    const char *why = "";
    if (!site) {
        snprintf(message, message_size, "not applied: not a verified game build");
        return 0;
    }
    if (!sos_hermes_unlock_verify(image, size, site, &why)) {
        snprintf(message, message_size, "not applied: %s in %s; the game's own unlock is kept", why, build);
        return 0;
    }
    memset(image + site->gate, 0x90, kJumpLength);
    snprintf(message, message_size, "offered on the game modes screen from the first new game");
    return 1;
}
