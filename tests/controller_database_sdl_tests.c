/* Headless real-SDL3 integration tests of user/gamecontrollerdb.txt and unmapped joystick diagnostics on the real
 * src/runtime_pad.c using SDL virtual joysticks in fresh child processes. No window, GPU, or audio device is opened. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include PAD_SOURCE
#undef printf

static char log_lines[512][384];
static int log_count;
static int checks, failures;

int runtime_log_printf(const char *format, ...) {
    char buf[384];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (log_count < 512) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
        snprintf(log_lines[log_count++], sizeof(log_lines[0]), "%s", buf);
    }
    return n;
}

int dcpgpu_overlay_captures_input(void) { return 0; }
void dcpgpu_choice_set_input(uint32_t (*read)(void)) { (void)read; }
int dcpgpu_latency_trace_enabled(void) { return 0; }
uint64_t dcpgpu_latency_press_ns(void) { return 0; }
void sos_keys_load(const char *file, SosKeyMap *map) { (void)file; memset(map, 0, sizeof(*map)); }
void sos_pad_kind_publish(int pad_kind) { (void)pad_kind; }
void runtime_history_note(const char *api, const char *library, uintptr_t caller) { (void)api; (void)library; (void)caller; }
static int logged_users[5];
void runtime_user_login(int player) { if (player >= 1 && player <= 4) logged_users[player] = 1; }
int runtime_user_player(int32_t user) { return (user >= 1 && user <= 4 && (user == 1 || logged_users[user])) ? user : 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { \
        ++failures; \
        printf("  [fail] %s:%d: %s\n", __FILE__, __LINE__, msg); \
    } \
} while (0)

static int log_lines_with(const char *a, const char *b) {
    int n = 0;
    for (int i = 0; i < log_count; ++i) {
        if ((!a || !*a || strstr(log_lines[i], a)) && (!b || !*b || strstr(log_lines[i], b))) ++n;
    }
    return n;
}

typedef struct {
    SDL_JoystickID id;
    SDL_Joystick *joystick;
} Virtual;

static Virtual attach_virtual(SDL_JoystickType type, uint16_t vendor, uint16_t product, const char *name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = type;
    desc.naxes = 6;
    desc.nbuttons = 15;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    Virtual v = {SDL_AttachVirtualJoystick(&desc), NULL};
    CHECK(v.id != 0, "SDL_AttachVirtualJoystick succeeds");
    v.joystick = SDL_OpenJoystick(v.id);
    CHECK(v.joystick != NULL, "SDL_OpenJoystick succeeds for test fixture");
    return v;
}

static void detach_virtual(Virtual *v) {
    if (v->joystick) {
        SDL_CloseJoystick(v->joystick);
        v->joystick = NULL;
    }
    if (v->id) {
        CHECK(SDL_DetachVirtualJoystick(v->id), "SDL_DetachVirtualJoystick succeeds");
        v->id = 0;
    }
}

static void guid_text(SDL_JoystickID id, char guid[33]) {
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, 33);
}

static void db_line(char *out, size_t size, SDL_JoystickID id, const char *name) {
    char guid[33];
    guid_text(id, guid);
    snprintf(out, size,
             "%s,%s,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:%s,\n",
             guid, name, SDL_GetPlatform());
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "write test file");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void disable_physical_backends(void) {
    static const char *const backends[] = {
        "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_DIRECTINPUT",
        "SDL_XINPUT_ENABLED", "SDL_JOYSTICK_WGI", "SDL_JOYSTICK_GAMEINPUT", "SDL_JOYSTICK_MFI",
        "SDL_JOYSTICK_LINUX_CLASSIC", "SDL_JOYSTICK_LINUX_DEADZONES"
    };
    for (size_t i = 0; i < sizeof(backends) / sizeof(*backends); ++i) SDL_SetHint(backends[i], "0");
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
}

static uint32_t read_buttons(int32_t handle) {
    SDL_UpdateJoysticks();
    PadData d;
    memset(&d, 0, sizeof(d));
    CHECK(pad_read_state(handle, &d) == 0, "scePadReadState succeeds");
    return d.buttons;
}

static void scan_for(int ms, int32_t handle) {
    for (int waited = 0; waited < ms; waited += 100) {
        SDL_Delay(100);
        read_buttons(handle);
    }
}

static void init_pad_runtime(const char *dir, int32_t *p1_out) {
    char settings[600];
    snprintf(settings, sizeof(settings), "%s/sons_of_sparta.ini", dir);
    runtime_pad_set_settings(settings);
    CHECK(pad_init() == 0, "scePadInit succeeds");
    *p1_out = pad_open(1, 0, 0, NULL);
    CHECK(*p1_out == 1, "scePadOpen player 1 succeeds");
}

static int scenario_missing_or_bad(const char *kind, const char *dir) {
    disable_physical_backends();
    char db[600];
    snprintf(db, sizeof(db), "%s/gamecontrollerdb.txt", dir);
    if (!strcmp(kind, "empty")) write_text(db, "");
    else if (!strcmp(kind, "invalid-text")) write_text(db, "not a mapping\n1234,bad\n# comment\n");
    else if (!strcmp(kind, "invalid-dir")) CHECK(SDL_CreateDirectory(db), "create blocking directory");

    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL_InitSubSystem(SDL_INIT_GAMEPAD)");
    int32_t p1 = 0;
    init_pad_runtime(dir, &p1);

    Virtual mapped = attach_virtual(SDL_JOYSTICK_TYPE_GAMEPAD, 0x045e, 0x02ea, "Builtin Pad");
    Virtual u1 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x1234, 0x5001, "Generic Stick 1");
    scan_for(400, p1);

    CHECK(SDL_IsGamepad(mapped.id) && !SDL_IsGamepad(u1.id), "built-in pad mapped, unknown stick unmapped");
    CHECK(log_lines_with("Unmapped joystick:", "Builtin Pad") == 0, "mapped gamepad is never reported as unmapped");

    if (!strcmp(kind, "missing")) {
        CHECK(log_lines_with("user controller mapping", "") == 0, "missing database is silent");
        CHECK(!SDL_GetPathInfo(db, NULL), "missing database is never created automatically");
    } else if (!strcmp(kind, "invalid-dir")) {
        CHECK(log_lines_with("could not load user controller mappings from user/gamecontrollerdb.txt:", "") == 1,
              "directory read failure logs one concise diagnostic");
    } else {
        CHECK(log_lines_with("loaded 0 user controller mappings from user/gamecontrollerdb.txt", "") == 1,
              "empty or invalid file logs 0 mappings concisely");
    }
    CHECK(log_lines_with(dir, "") == 0, "logs never expose local temp/user directory path");

    char guid[33];
    guid_text(u1.id, guid);
    CHECK(strlen(guid) == 32 && strspn(guid, "0123456789abcdef") == 32, "unmapped GUID is 32 lowercase hex chars");
    CHECK(log_lines_with("Unmapped joystick: Generic Stick 1, GUID=", guid) == 1, "unmapped diagnostic contains name and GUID");
    CHECK(log_lines_with("Unmapped joystick: Generic Stick 1", "VID:PID=1234:5001") == 1, "unmapped diagnostic contains VID:PID");

    /* Repeated events and scans must not log again. */
    for (int i = 0; i < 10; ++i) {
        SDL_SetJoystickVirtualButton(u1.joystick, 0, (i & 1) != 0);
        SDL_SetJoystickVirtualAxis(u1.joystick, 0, (Sint16)(i * 1000));
        read_buttons(p1);
    }
    scan_for(600, p1);
    CHECK(log_lines_with("Unmapped joystick:", "") == 1, "repeated events and scans do not re-log unmapped joystick");

    /* Second unmapped joystick logs once without repeating first. */
    Virtual u2 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x1234, 0x5002, "Generic Stick 2");
    scan_for(400, p1);
    CHECK(log_lines_with("Unmapped joystick: Generic Stick 1", "") == 1, "first unmapped joystick still logged once");
    CHECK(log_lines_with("Unmapped joystick: Generic Stick 2", "VID:PID=1234:5002") == 1, "second unmapped joystick logged once");

    /* Removal cleans state; reconnection logs deterministically once. */
    detach_virtual(&u1);
    scan_for(350, p1);
    CHECK(log_lines_with("Unmapped joystick:", "") == 2, "removal does not emit spurious diagnostic");
    u1 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x1234, 0x5001, "Generic Stick 1");
    scan_for(400, p1);
    CHECK(log_lines_with("Unmapped joystick: Generic Stick 1", "") == 2, "reconnected unmapped joystick logs once again");

    /* Built-in mapped gamepad input still works unaffected. */
    SDL_SetJoystickVirtualButton(mapped.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK((read_buttons(p1) & BTN_CROSS) != 0, "built-in gamepad button reaches game");
    SDL_SetJoystickVirtualButton(mapped.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK((read_buttons(p1) & BTN_CROSS) == 0, "built-in gamepad release reaches game");

    detach_virtual(&mapped);
    detach_virtual(&u1);
    detach_virtual(&u2);
    return 0;
}

static int scenario_missing_vid_pid(const char *dir) {
    disable_physical_backends();
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL_InitSubSystem");
    int32_t p1 = 0;
    init_pad_runtime(dir, &p1);

    Virtual u = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0, 0, "NoId Stick");
    scan_for(400, p1);
    CHECK(log_lines_with("Unmapped joystick: NoId Stick", "VID:PID=unavailable") == 1,
          "unavailable VID:PID reported cleanly");
    CHECK(log_lines_with("0000:0000", "") == 0, "unavailable VID:PID does not fabricate 0000:0000");

    detach_virtual(&u);
    return 0;
}

static int scenario_valid_and_hotplug(const char *dir) {
    disable_physical_backends();
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL_InitSubSystem");

    /* Probe GUID for a hotplug device before writing DB, then detach it so it arrives after startup. */
    Virtual hotplug_probe = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x2222, 0x6003, "Hotplug Stick");
    char hotplug_line[512];
    db_line(hotplug_line, sizeof(hotplug_line), hotplug_probe.id, "Mapped Hotplug");
    detach_virtual(&hotplug_probe);
    SDL_UpdateJoysticks();

    /* Startup-connected unmapped devices. */
    Virtual u1 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x2222, 0x6001, "Startup Stick 1");
    Virtual u2 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x2222, 0x6002, "Startup Stick 2");
    Virtual extra = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x2222, 0x6099, "Still Unmapped");
    SDL_UpdateJoysticks();
    CHECK(!SDL_IsGamepad(u1.id) && !SDL_IsGamepad(u2.id) && !SDL_IsGamepad(extra.id),
          "devices are unmapped before user database loads");

    char text[4096] = "# User mappings\n\nmalformed line here\n0300000000000000000000000000dead,Other,a:b0,platform:NotThisOS,\n";
    char line[512];
    db_line(line, sizeof(line), u1.id, "Mapped Startup 1");
    strncat(text, line, sizeof(text) - strlen(text) - 1);
    db_line(line, sizeof(line), u2.id, "Mapped Startup 2");
    strncat(text, line, sizeof(text) - strlen(text) - 1);
    strncat(text, hotplug_line, sizeof(text) - strlen(text) - 1);

    char db[600];
    snprintf(db, sizeof(db), "%s/gamecontrollerdb.txt", dir);
    write_text(db, text);

    int32_t p1 = 0;
    init_pad_runtime(dir, &p1);
    read_buttons(p1);

    /* Database loaded once before startup device classification. */
    CHECK(log_lines_with("loaded 3 user controller mappings from user/gamecontrollerdb.txt", "") == 1,
          "partially malformed file loads all 3 valid mappings once");
    CHECK(log_lines_with(dir, "") == 0, "load log never exposes local path");
    CHECK(SDL_IsGamepad(u1.id) && SDL_IsGamepad(u2.id), "startup devices recognized immediately via user DB");
    CHECK(!SDL_IsGamepad(extra.id), "unlisted device remains unmapped");
    CHECK(log_lines_with("Unmapped joystick:", "Startup Stick 1") == 0 &&
          log_lines_with("Unmapped joystick:", "Startup Stick 2") == 0,
          "user-mapped devices are never reported as unmapped");
    CHECK(log_lines_with("Unmapped joystick: Still Unmapped", "VID:PID=2222:6099") == 1,
          "unlisted joystick is reported once");

    /* Verify input from startup user-mapped pads. */
    const int32_t p2 = pad_open(2, 0, 0, NULL);
    CHECK(p2 == 2, "player 2 opened for second mapped startup pad");
    SDL_SetJoystickVirtualButton(u1.joystick, 0, true);
    CHECK((read_buttons(p1) & BTN_CROSS) != 0, "startup mapped pad 1 delivers Cross");
    SDL_SetJoystickVirtualButton(u1.joystick, 0, false);
    SDL_SetJoystickVirtualButton(u2.joystick, 0, true);
    CHECK((read_buttons(p2) & BTN_CROSS) != 0, "startup mapped pad 2 delivers Cross");
    SDL_SetJoystickVirtualButton(u2.joystick, 0, false);

    /* Built-in SDL mapping remains intact alongside user mappings. */
    Virtual builtin = attach_virtual(SDL_JOYSTICK_TYPE_GAMEPAD, 0x045e, 0x02ea, "Builtin Pad");
    /* Hotplug a device whose mapping came from user/gamecontrollerdb.txt. */
    Virtual u3 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x2222, 0x6003, "Hotplug Stick");
    scan_for(400, p1);
    CHECK(SDL_IsGamepad(builtin.id), "built-in SDL mapping remains intact");
    CHECK(SDL_IsGamepad(u3.id), "hotplugged device recognized automatically without reloading file");
    CHECK(log_lines_with("Unmapped joystick:", "Hotplug Stick") == 0, "hotplugged user-mapped device is not logged as unmapped");
    CHECK(log_lines_with("user controller mapping", "") == 1, "database was not reloaded on hotplug");

    const int32_t p3 = pad_open(3, 0, 0, NULL);
    const int32_t p4 = pad_open(4, 0, 0, NULL);
    CHECK(p3 == 3 && p4 == 4, "built-in and hotplugged mapped pads occupy slots 3 and 4");
    SDL_SetJoystickVirtualButton(u3.joystick, 0, true);
    CHECK(((read_buttons(p3) | read_buttons(p4)) & BTN_CROSS) != 0, "hotplugged user-mapped pad delivers input");
    SDL_SetJoystickVirtualButton(u3.joystick, 0, false);

    detach_virtual(&u1);
    detach_virtual(&u2);
    detach_virtual(&extra);
    detach_virtual(&builtin);
    detach_virtual(&u3);
    return 0;
}

static int scenario_env_priority(const char *dir) {
    disable_physical_backends();
    CHECK(SDL_InitSubSystem(SDL_INIT_JOYSTICK), "init joystick to probe GUID");
    Virtual probe = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x3333, 0x7001, "Env Stick");
    char guid1[33];
    guid_text(probe.id, guid1);
    detach_virtual(&probe);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);

    char env_map[512];
    snprintf(env_map, sizeof(env_map), "%s,FROM ENV,a:b0,b:b1,leftx:a0,lefty:a1,platform:%s,", guid1, SDL_GetPlatform());
    CHECK(SDL_setenv_unsafe("SDL_GAMECONTROLLERCONFIG", env_map, 1) == 0, "set SDL_GAMECONTROLLERCONFIG");
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL_InitSubSystem");

    Virtual u1 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x3333, 0x7001, "Env Stick");
    Virtual u2 = attach_virtual(SDL_JOYSTICK_TYPE_UNKNOWN, 0x3333, 0x7002, "File Stick");
    SDL_UpdateJoysticks();
    CHECK(SDL_IsGamepad(u1.id) && !SDL_IsGamepad(u2.id), "env mapping active before file load");

    char text[1200] = "", line[512], db[600];
    db_line(line, sizeof(line), u1.id, "FROM FILE");
    strncat(text, line, sizeof(text) - strlen(text) - 1);
    db_line(line, sizeof(line), u2.id, "FILE ONLY");
    strncat(text, line, sizeof(text) - strlen(text) - 1);
    snprintf(db, sizeof(db), "%s/gamecontrollerdb.txt", dir);
    write_text(db, text);

    int32_t p1 = 0;
    init_pad_runtime(dir, &p1);
    read_buttons(p1);

    char *m1 = SDL_GetGamepadMappingForID(u1.id);
    CHECK(m1 && strstr(m1, "FROM ENV") && !strstr(m1, "FROM FILE"), "SDL_GAMECONTROLLERCONFIG keeps priority over user file");
    SDL_free(m1);
    CHECK(SDL_IsGamepad(u2.id), "second device still mapped from user file");
    char *m2 = SDL_GetGamepadMappingForID(u2.id);
    CHECK(m2 && strstr(m2, "FILE ONLY"), "second device uses user file mapping");
    SDL_free(m2);
    CHECK(log_lines_with("Unmapped joystick:", "") == 0, "neither mapped device logged as unmapped");

    detach_virtual(&u1);
    detach_virtual(&u2);
    return 0;
}

static int run_scenario(const char *name, const char *dir) {
    int rc = 0;
    if (!strcmp(name, "missing-vid-pid")) rc = scenario_missing_vid_pid(dir);
    else if (!strcmp(name, "valid-and-hotplug")) rc = scenario_valid_and_hotplug(dir);
    else if (!strcmp(name, "env-priority")) rc = scenario_env_priority(dir);
    else rc = scenario_missing_or_bad(name, dir);
    printf("  scenario %-18s %s (%d checks, %d failed)\n", name, failures ? "FAIL" : "ok", checks, failures);
    SDL_Quit();
    return (rc || failures) ? 1 : 0;
}

static void cleanup_dir(const char *dir) {
    char path[600];
    snprintf(path, sizeof(path), "%s/gamecontrollerdb.txt", dir);
    SDL_RemovePath(path);
    snprintf(path, sizeof(path), "%s/sons_of_sparta.ini", dir);
    SDL_RemovePath(path);
    SDL_RemovePath(dir);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "scenario")) {
        return run_scenario(argv[2], argv[3]);
    }
    static const char *const scenarios[] = {
        "missing", "missing-vid-pid", "empty", "invalid-text", "invalid-dir", "valid-and-hotplug", "env-priority"
    };
    const char *temp = getenv("TEMP");
    if (!temp || !*temp) temp = getenv("TMPDIR");
    if (!temp || !*temp) temp = "/tmp";
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(*scenarios); ++i) {
        char dir[600];
        snprintf(dir, sizeof(dir), "%s/sos_pad_db_%llu_%s", temp, (unsigned long long)SDL_GetPerformanceCounter(), scenarios[i]);
        cleanup_dir(dir);
        CHECK(SDL_CreateDirectory(dir), "create scenario directory");
        const char *args[] = {argv[0], "scenario", scenarios[i], dir, NULL};
        SDL_Process *child = SDL_CreateProcess(args, false);
        CHECK(child != NULL, "launch scenario child process");
        int code = -1;
        if (child) {
            CHECK(SDL_WaitProcess(child, true, &code), "wait for scenario child");
            SDL_DestroyProcess(child);
        }
        CHECK(code == 0, scenarios[i]);
        cleanup_dir(dir);
    }
    printf("Controller database SDL integration: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
