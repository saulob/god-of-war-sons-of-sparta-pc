#include "sos_uw_probe.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x38; mov r13, [rip+..] */
static const uint8_t kPrefix[SOS_UW_PROBE_PREFIX] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                     0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38, 0x4c, 0x8b, 0x2d};

static const struct {
    const char *build;
    uint32_t method;
} kSites[] = {
    {"CUSA57767-1.00", 0x57736b0},
    {"CUSA57767-1.01", 0x577e230},
};

/* System V entry has rsp % 16 == 8; six pushes keep it 8 mod 16, sub rsp,8 aligns to 16 before the call. */
static const uint8_t kSaveHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
static const uint8_t kRestoreTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};

enum { STUB_SIZE = sizeof(kSaveHead) + 8 + sizeof(kRestoreTail) + SOS_UW_PROBE_STOLEN + 5 };
_Static_assert(STUB_SIZE <= SOS_UW_PROBE_STUB_BYTES, "the stub fits its slot");

static const struct {
    const char *build;
    uint32_t runtime_class;
} kRuntimeSites[] = {
    {"CUSA57767-1.00", 0x9bbbd10u},
    {"CUSA57767-1.01", 0x9bbbf68u},
};

static SosUwProbePublish g_publish;
static SosUwProbeSerial g_serial;
static const uint8_t *g_image;
static uint64_t g_image_size;
static uint32_t g_runtime_class;
static uint32_t g_last_serial;

static int readable(const void *ptr, size_t size) {
    if (!ptr || !size) return 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(ptr, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    const uintptr_t start = (uintptr_t)ptr, end = start + size;
    const uintptr_t region_end = (uintptr_t)info.BaseAddress + info.RegionSize;
    return end >= start && end <= region_end;
#else
    return 1;
#endif
}

static int safe_read(const void *src, void *dst, size_t size) {
    if (!readable(src, size)) return 0;
    memcpy(dst, src, size);
    return 1;
}

static int read_string(const void *string, char *out, size_t capacity) {
    int32_t length;
    if (!out || capacity < 2 || !safe_read((const uint8_t *)string + 0x10, &length, sizeof(length)) ||
        length < 0 || length > 200)
        return 0;
    const size_t count = (size_t)length < capacity - 1 ? (size_t)length : capacity - 1;
    for (size_t i = 0; i < count; ++i) {
        uint16_t character;
        if (!safe_read((const uint8_t *)string + 0x14 + i * 2, &character, sizeof(character))) return 0;
        out[i] = character < 0x80 ? (char)character : '?';
    }
    out[count] = '\0';
    return 1;
}

static int read_class_name(const void *object, char *out, size_t capacity) {
    void *klass = NULL, *name = NULL;
    if (!out || capacity < 2 || !safe_read(object, &klass, sizeof(klass)) || !klass ||
        !safe_read((const uint8_t *)klass + 0x10, &name, sizeof(name)) || !name)
        return 0;
    for (size_t i = 0; i + 1 < capacity; ++i) {
        char character;
        if (!safe_read((const uint8_t *)name + i, &character, sizeof(character))) break;
        out[i] = character;
        if (!character) return 1;
    }
    out[capacity - 1] = '\0';
    return 1;
}

static void log_parallax(float camera_x, float camera_y, float ortho) {
    printf("DEBUG_SAULO Ultrawide parallax: camera=(%.2f,%.2f) ortho=%.2f\n", camera_x, camera_y, ortho);
    if (!g_image || !g_runtime_class || (uint64_t)g_runtime_class + sizeof(void *) > g_image_size) {
        printf("DEBUG_SAULO Ultrawide parallax: unavailable runtime image\n");
        return;
    }
    void *klass = NULL, *statics = NULL, *runtime = NULL;
    if (!safe_read(g_image + g_runtime_class, &klass, sizeof(klass)) || !klass ||
        !safe_read((const uint8_t *)klass + 0xb8, &statics, sizeof(statics)) || !statics ||
        !safe_read(statics, &runtime, sizeof(runtime)) || !runtime) {
        printf("DEBUG_SAULO Ultrawide parallax: unavailable GameRuntime\n");
        return;
    }
    void *instantiator = NULL, *subsystems = NULL;
    if (!safe_read((const uint8_t *)runtime + 0x20, &instantiator, sizeof(instantiator)) || !instantiator ||
        !safe_read((const uint8_t *)instantiator + 0x10, &subsystems, sizeof(subsystems)) || !subsystems) {
        printf("DEBUG_SAULO Ultrawide parallax: unavailable SubsystemInstantiator\n");
        return;
    }
    void *items = NULL;
    int32_t subsystem_count = 0;
    if (!safe_read((const uint8_t *)subsystems + 0x10, &items, sizeof(items)) ||
        !safe_read((const uint8_t *)subsystems + 0x18, &subsystem_count, sizeof(subsystem_count)) ||
        !items || subsystem_count < 0 || subsystem_count > 64) {
        printf("DEBUG_SAULO Ultrawide parallax: unavailable subsystem list\n");
        return;
    }
    void *parallax = NULL;
    for (int32_t i = 0; i < subsystem_count && !parallax; ++i) {
        void *candidate = NULL;
        char name[64] = "";
        if (safe_read((const uint8_t *)items + 0x20 + i * 8, &candidate, sizeof(candidate)) && candidate &&
            read_class_name(candidate, name, sizeof(name)) && !strcmp(name, "ParallaxSubsystem"))
            parallax = candidate;
    }
    if (!parallax) {
        printf("DEBUG_SAULO Ultrawide parallax: ParallaxSubsystem missing (%d subsystems)\n", subsystem_count);
        return;
    }
    void *layers = NULL, *entries = NULL;
    int32_t layer_count = 0;
    if (!safe_read((const uint8_t *)parallax + 0x28, &layers, sizeof(layers)) || !layers ||
        !safe_read((const uint8_t *)layers + 0x18, &entries, sizeof(entries)) ||
        !safe_read((const uint8_t *)layers + 0x24, &layer_count, sizeof(layer_count)) ||
        !entries || layer_count < 0 || layer_count > 512) {
        printf("DEBUG_SAULO Ultrawide parallax: unavailable layer set\n");
        return;
    }
    int printed = 0, candidates = 0;
    const float span_x = ortho * 3.6f + 4.0f, span_y = ortho + 4.0f;
    for (int32_t i = 0; i < layer_count && printed < 28; ++i) {
        int32_t hash = 0;
        void *layer = NULL;
        if (!safe_read((const uint8_t *)entries + 0x20 + i * 16, &hash, sizeof(hash)) || hash < 0 ||
            !safe_read((const uint8_t *)entries + 0x28 + i * 16, &layer, sizeof(layer)) || !layer)
            continue;
        void *game_object = NULL, *guid = NULL;
        float initial[2] = {0}, offset[2] = {0}, effect_position[2] = {0}, effect_size[2] = {0};
        uint8_t showing = 0, only_in_frustum = 0, list_initialized = 0;
        int32_t behaviour = 0, renderer_count = -1, disabled_count = -1;
        char bounds_name[64] = "?";
        if (safe_read((const uint8_t *)layer + 0x10, &game_object, sizeof(game_object)) && !game_object)
            game_object = NULL;
        if (!safe_read((const uint8_t *)layer + 0xd0, &guid, sizeof(guid)) ||
            !read_string(guid, bounds_name, sizeof(bounds_name)))
            strcpy(bounds_name, "?");
        safe_read((const uint8_t *)layer + 0xe8, initial, sizeof(initial));
        safe_read((const uint8_t *)layer + 0xf0, offset, sizeof(offset));
        safe_read((const uint8_t *)layer + 0x104, effect_position, sizeof(effect_position));
        safe_read((const uint8_t *)layer + 0x10c, effect_size, sizeof(effect_size));
        safe_read((const uint8_t *)layer + 0x114, &showing, sizeof(showing));
        safe_read((const uint8_t *)layer + 0x4c, &only_in_frustum, sizeof(only_in_frustum));
        safe_read((const uint8_t *)layer + 0x64, &behaviour, sizeof(behaviour));
        safe_read((const uint8_t *)layer + 0x131, &list_initialized, sizeof(list_initialized));
        void *renderers = NULL, *disabled = NULL;
        if (safe_read((const uint8_t *)layer + 0x118, &renderers, sizeof(renderers)) && renderers)
            safe_read((const uint8_t *)renderers + 0x18, &renderer_count, sizeof(renderer_count));
        if (safe_read((const uint8_t *)layer + 0x138, &disabled, sizeof(disabled)) && disabled)
            safe_read((const uint8_t *)disabled + 0x18, &disabled_count, sizeof(disabled_count));
        const float effect_min_x = effect_position[0], effect_max_x = effect_position[0] + effect_size[0];
        const float effect_min_y = effect_position[1], effect_max_y = effect_position[1] + effect_size[1];
        if (effect_max_x < camera_x - span_x || effect_min_x > camera_x + span_x || effect_max_y < camera_y - span_y ||
            effect_min_y > camera_y + span_y)
            continue;
        ++candidates;
        char renderer_text[24] = "null", disabled_text[24] = "null";
        if (renderer_count >= 0) snprintf(renderer_text, sizeof(renderer_text), "%d", (int)renderer_count);
        if (disabled_count >= 0) snprintf(disabled_text, sizeof(disabled_text), "%d", (int)disabled_count);
        void *bounds_instance = NULL;
        float cached[6] = {0};
        uint8_t bounds_ready = 0;
        if (safe_read((const uint8_t *)layer + 0x120, &bounds_instance, sizeof(bounds_instance)) && bounds_instance &&
            safe_read((const uint8_t *)bounds_instance + 0x80, &bounds_ready, sizeof(bounds_ready)) && bounds_ready)
            safe_read((const uint8_t *)bounds_instance + 0x84, cached, sizeof(cached));
        printf("DEBUG_SAULO Ultrawide parallax: layer=%llx go=%llx bounds=\"%s\" init=(%.2f,%.2f) offset=(%.2f,%.2f) "
               "effect=(%.2f,%.2f) size=(%.2f,%.2f) show=%u only=%u behaviour=%d renderers=%s listInit=%u disabled=%s "
               "cached=(%.2f,%.2f)+-(%.2f,%.2f)\n",
               (unsigned long long)(uintptr_t)layer, (unsigned long long)(uintptr_t)game_object, bounds_name, initial[0],
               initial[1], offset[0], offset[1], effect_position[0], effect_position[1], effect_size[0], effect_size[1],
               (unsigned)showing, (unsigned)only_in_frustum, behaviour, renderer_text, (unsigned)list_initialized,
               disabled_text, cached[0], cached[1], cached[3], cached[4]);
        ++printed;
    }
    printf("DEBUG_SAULO Ultrawide parallax: layers=%d printed=%d candidates=%d\n", layer_count, printed, candidates);
    fflush(stdout);
}

uint32_t sos_uw_probe_site(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kSites) / sizeof(*kSites); ++i)
        if (!strcmp(build_key, kSites[i].build)) return kSites[i].method;
    return 0;
}

static uint32_t runtime_site(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kRuntimeSites) / sizeof(*kRuntimeSites); ++i)
        if (!strcmp(build_key, kRuntimeSites[i].build)) return kRuntimeSites[i].runtime_class;
    return 0;
}

static int reaches(const uint8_t *from_end, const uint8_t *to) {
    const int64_t delta = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from_end;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

static void write_jmp(uint8_t *at, const uint8_t *target) {
    const int32_t rel = (int32_t)((int64_t)(intptr_t)target - (int64_t)(intptr_t)(at + 5));
    at[0] = 0xe9;
    memcpy(at + 1, &rel, sizeof(rel));
}

int sos_uw_probe_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stubs, size_t stubs_capacity,
                         char *message, size_t message_size) {
    const uint32_t site = sos_uw_probe_site(build_key);
    const uint32_t runtime = runtime_site(build_key);
    const char *reason = !site || !runtime                                    ? "not a supported game build"
                         : !image || (uint64_t)site + SOS_UW_PROBE_PREFIX > size ? "image ends before the camera code"
                         : memcmp(image + site, kPrefix, sizeof(kPrefix))   ? "CinemachineBrain.PushStateToUnityCamera bytes mismatch"
                         : !stubs || stubs_capacity < SOS_UW_PROBE_STUB_BYTES ? "stub buffer too small"
                                                                              : NULL;
    uint8_t *const method = image ? image + site : NULL;
    if (!reason && (!reaches(method + 5, stubs) || !reaches(stubs + STUB_SIZE, method + SOS_UW_PROBE_STOLEN)))
        reason = "stub out of rel32 range";
    if (reason) {
        if (message && message_size) snprintf(message, message_size, "unavailable: %s", reason);
        return 0;
    }
    const uint64_t handler = (uint64_t)(uintptr_t)sos_uw_probe_on_camera;
    uint8_t *at = stubs;
    memcpy(at, kSaveHead, sizeof(kSaveHead));
    at += sizeof(kSaveHead);
    memcpy(at, &handler, sizeof(handler));
    at += sizeof(handler);
    memcpy(at, kRestoreTail, sizeof(kRestoreTail));
    at += sizeof(kRestoreTail);
    memcpy(at, method, SOS_UW_PROBE_STOLEN);
    at += SOS_UW_PROBE_STOLEN;
    write_jmp(at, method + SOS_UW_PROBE_STOLEN);
    write_jmp(method, stubs);
    memset(method + 5, 0x90, SOS_UW_PROBE_STOLEN - 5);
    if (message && message_size)
        snprintf(message, message_size, "on CinemachineBrain.PushStateToUnityCamera at image+0x%x", (unsigned)site);
    g_image = image;
    g_image_size = size;
    g_runtime_class = runtime;
    return 1;
}

void sos_uw_probe_arm(SosUwProbePublish publish, SosUwProbeSerial serial) {
    g_publish = publish;
    g_serial = serial;
    g_last_serial = 0;
}

SOS_UW_PROBE_SYSV void sos_uw_probe_on_camera(void *brain, const void *state) {
    (void)brain;
    if (!state) return;
    float ortho, raw[2], correction[2];
    memcpy(&ortho, (const uint8_t *)state + SOS_UW_PROBE_ORTHO, sizeof(ortho));
    memcpy(raw, (const uint8_t *)state + SOS_UW_PROBE_RAW_POSITION, sizeof(raw));
    memcpy(correction, (const uint8_t *)state + SOS_UW_PROBE_CORRECTION, sizeof(correction));
    const float x = raw[0] + correction[0], y = raw[1] + correction[1];
    if (!isfinite(x) || !isfinite(y) || !isfinite(ortho)) return;
    if (g_publish) g_publish(x, y, ortho);
    if (g_serial) {
        const uint32_t serial = g_serial();
        if (serial != g_last_serial) {
            g_last_serial = serial;
            log_parallax(x, y, ortho);
        }
    }
}
