#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline int sos_gamepad_db_path(const char *settings_file, char *out, size_t size) {
    static const char name[] = "gamecontrollerdb.txt";
    if (!out || !size) return 0;
    out[0] = 0;
    if (!settings_file || !*settings_file) return 0;
    const char *end = strrchr(settings_file, '/');
    const char *back = strrchr(settings_file, '\\');
    if (back && (!end || back > end)) end = back;
    const size_t folder = end ? (size_t)(end - settings_file) + 1 : 0;
    if (folder + sizeof(name) > size) return 0;
    memcpy(out, settings_file, folder);
    memcpy(out + folder, name, sizeof(name));
    return 1;
}

#define SOS_UNMAPPED_SEEN 16

typedef struct {
    uint32_t ids[SOS_UNMAPPED_SEEN];
    size_t count;
} SosUnmappedSeen;

static inline int sos_unmapped_first_sight(SosUnmappedSeen *seen, uint32_t id) {
    for (size_t i = 0; i < seen->count; ++i) {
        if (seen->ids[i] == id) return 0;
    }
    if (seen->count == SOS_UNMAPPED_SEEN) {
        memmove(seen->ids, seen->ids + 1, (SOS_UNMAPPED_SEEN - 1) * sizeof(*seen->ids));
        --seen->count;
    }
    seen->ids[seen->count++] = id;
    return 1;
}

static inline void sos_unmapped_forget_absent(SosUnmappedSeen *seen, const uint32_t *present, size_t count) {
    size_t kept = 0;
    for (size_t i = 0; i < seen->count; ++i) {
        int found = 0;
        for (size_t j = 0; j < count && !found; ++j) found = present[j] == seen->ids[i];
        if (found) seen->ids[kept++] = seen->ids[i];
    }
    seen->count = kept;
}

static inline void sos_unmapped_joystick_line(const char *name, const char *guid, unsigned vendor, unsigned product,
                                              char *line, size_t size) {
    char clean[96] = "";
    if (name) {
        size_t k = 0;
        for (const char *c = name; *c && k + 1 < sizeof(clean); ++c) {
            clean[k++] = (unsigned char)*c < 0x20 || *c == 0x7f ? ' ' : *c;
        }
        clean[k] = 0;
    }
    char vidpid[24];
    if (vendor || product) {
        snprintf(vidpid, sizeof(vidpid), "VID:PID=%04x:%04x", vendor & 0xffffu, product & 0xffffu);
    } else {
        snprintf(vidpid, sizeof(vidpid), "VID:PID=unavailable");
    }
    if (clean[0]) {
        snprintf(line, size,
                 "Runtime: Unmapped joystick: %s, GUID=%s, %s; add a mapping to user/gamecontrollerdb.txt",
                 clean, guid ? guid : "", vidpid);
    } else {
        snprintf(line, size,
                 "Runtime: Unmapped joystick: GUID=%s, %s; add a mapping to user/gamecontrollerdb.txt",
                 guid ? guid : "", vidpid);
    }
}

/* Formats load status using only the logical user/gamecontrollerdb.txt name, redacting local paths from SDL errors. */
static inline void sos_gamepad_db_load_line(int added, const char *path, const char *sdl_error,
                                            char *line, size_t size) {
    if (added >= 0) {
        snprintf(line, size, "Runtime: loaded %d user controller %s from user/gamecontrollerdb.txt",
                 added, added == 1 ? "mapping" : "mappings");
        return;
    }
    const size_t path_len = path ? strlen(path) : 0;
    size_t folder_len = 0;
    if (path && path_len) {
        const char *end = strrchr(path, '/');
        const char *back = strrchr(path, '\\');
        if (back && (!end || back > end)) end = back;
        if (end) folder_len = (size_t)(end - path) + 1;
    }
    char reason[128] = "";
    if (sdl_error && *sdl_error) {
        size_t k = 0;
        for (const char *c = sdl_error; *c && k + 1 < sizeof(reason);) {
            if (path_len && !strncmp(c, path, path_len)) {
                static const char logical[] = "user/gamecontrollerdb.txt";
                const size_t n = sizeof(logical) - 1;
                if (k + n < sizeof(reason)) {
                    memcpy(reason + k, logical, n);
                    k += n;
                }
                c += path_len;
            } else if (folder_len && !strncmp(c, path, folder_len)) {
                static const char prefix[] = "user/";
                const size_t n = sizeof(prefix) - 1;
                if (k + n < sizeof(reason)) {
                    memcpy(reason + k, prefix, n);
                    k += n;
                }
                c += folder_len;
            } else {
                reason[k++] = (unsigned char)*c < 0x20 || *c == 0x7f ? ' ' : *c;
                ++c;
            }
        }
        reason[k] = 0;
    }
    snprintf(line, size, "Runtime: could not load user controller mappings from user/gamecontrollerdb.txt: %s",
             reason[0] ? reason : "read error");
}
