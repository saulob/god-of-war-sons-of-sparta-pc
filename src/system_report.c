#include "system_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

static const char *value_of(SosEnvironment environment, const char *name) {
    const char *value = environment ? environment(name) : NULL;
    return value && value[0] ? value : NULL;
}

static int contains_word(const char *text, const char *word) {
    if (!text) return 0;
    for (const char *at = text; (at = strstr(at, word)); at += strlen(word)) {
        const int starts = at == text || at[-1] == ':' || at[-1] == ' ' || at[-1] == ',' || at[-1] == ';';
        const char end = at[strlen(word)];
        if (starts && (!end || end == ':' || end == ' ' || end == ',' || end == ';')) return 1;
    }
    return 0;
}

/* A short environment value that cannot be a path: letters, digits and . _ - : , only, else "(withheld)". */
static const char *clean(SosEnvironment environment, const char *name, char *out, size_t size) {
    const char *value = value_of(environment, name);
    if (!value) return "(unset)";
    size_t used = 0;
    for (const char *c = value; *c; ++c) {
        const int ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' ||
                       *c == '_' || *c == '-' || *c == ':' || *c == ',';
        if (!ok || used + 1 >= size || used >= 64) return "(withheld)";
        out[used++] = *c;
    }
    out[used] = 0;
    return out;
}

/* The value of KEY= in os-release text, unquoted, bounded, or "". */
static void os_release_value(const char *text, const char *key, char *out, size_t size) {
    out[0] = 0;
    if (!text || !size) return;
    const size_t key_length = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        const size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > key_length && !strncmp(line, key, key_length) && line[key_length] == '=') {
            const char *value = line + key_length + 1;
            size_t value_length = length - key_length - 1;
            if (value_length && (value[0] == '"' || value[0] == '\'')) {
                ++value;
                --value_length;
                if (value_length && (value[value_length - 1] == '"' || value[value_length - 1] == '\'')) --value_length;
            }
            size_t used = 0;
            for (size_t i = 0; i < value_length && used + 1 < size && used < 80; ++i)
                if (value[i] >= ' ' && value[i] != '/' && value[i] != '\\') out[used++] = value[i];
            out[used] = 0;
            return;
        }
        line = end ? end + 1 : NULL;
    }
}

/* kB of a /proc/meminfo field, or -1. */
static long long meminfo_kb(const char *text, const char *key) {
    if (!text) return -1;
    const char *at = strstr(text, key);
    while (at && at != text && at[-1] != '\n') at = strstr(at + 1, key);
    if (!at) return -1;
    at += strlen(key);
    if (*at != ':') return -1;
    return strtoll(at + 1, NULL, 10);
}

SosSystemFacts sos_system_facts(SosEnvironment environment, const char *os_release) {
    SosSystemFacts facts;
    memset(&facts, 0, sizeof(facts));
    const char *desktop = value_of(environment, "XDG_CURRENT_DESKTOP");
    const int gamescope_desktop = desktop && contains_word(desktop, "gamescope");
    facts.gamescope = value_of(environment, "GAMESCOPE_WAYLAND_DISPLAY") != NULL || gamescope_desktop;
    facts.steam = value_of(environment, "SteamGameId") || value_of(environment, "SteamAppId") ||
                  value_of(environment, "SteamOverlayGameId") || value_of(environment, "SteamClientLaunch");
    const char *deck = value_of(environment, "SteamDeck");
    facts.steam_deck = deck && deck[0] == '1';
    /* Gaming Mode: Gamescope is the session itself (its desktop name, or Steam's Gamepad UI inside it). */
    facts.gaming_mode = facts.gamescope && (gamescope_desktop || value_of(environment, "SteamGamepadUI") != NULL ||
                                            value_of(environment, "STEAM_GAMESCOPE_NIS_SUPPORTED") != NULL);
    facts.wayland = value_of(environment, "WAYLAND_DISPLAY") != NULL;
    facts.x11 = value_of(environment, "DISPLAY") != NULL;
    const char *preload = value_of(environment, "LD_PRELOAD");
    facts.steam_overlay = preload && strstr(preload, "gameoverlayrenderer") != NULL;
    facts.appimage = value_of(environment, "APPIMAGE") != NULL || value_of(environment, "APPDIR") != NULL;
    char id[64];
    os_release_value(os_release, "ID", id, sizeof(id));
    facts.steamos = !strcmp(id, "steamos");
    return facts;
}

const char *sos_system_mode_name(const SosSystemFacts *facts) {
    if (facts->gaming_mode) return "Gaming Mode";
    if (facts->gamescope) return facts->steam ? "nested Gamescope (Steam)" : "nested Gamescope";
    return facts->steam ? "Desktop Mode (Steam)" : "desktop";
}

typedef struct {
    char *out;
    size_t size, used;
} Text;

static void put(Text *text, const char *format, ...) {
    if (text->used >= text->size) return;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(text->out + text->used, text->size - text->used, format, args);
    va_end(args);
    if (written > 0) text->used += (size_t)written < text->size - text->used ? (size_t)written : text->size - text->used - 1;
}

static double gib(long long kb) { return kb < 0 ? -1.0 : (double)kb / (1024.0 * 1024.0); }

size_t sos_system_report_format(char *out, size_t size, const char *port_version, SosEnvironment environment,
                                const char *os_release, const char *meminfo, const char *kernel_release,
                                const char *machine) {
    if (!out || !size) return 0;
    out[0] = 0;
    Text text = {out, size, 0};
    const SosSystemFacts facts = sos_system_facts(environment, os_release);
    char pretty[96], version[64], variant[64];
    os_release_value(os_release, "PRETTY_NAME", pretty, sizeof(pretty));
    os_release_value(os_release, "VERSION_ID", version, sizeof(version));
    os_release_value(os_release, "VARIANT_ID", variant, sizeof(variant));
    put(&text, "System: port %s, %s %s, kernel %s\n", port_version && *port_version ? port_version : "unknown",
#ifdef _WIN32
        "Windows",
#else
        "Linux",
#endif
        machine && *machine ? machine : "x86_64", kernel_release && *kernel_release ? kernel_release : "unknown");
    if (os_release)
        put(&text, "System: distribution %s (version %s%s%s), SteamOS %s\n", pretty[0] ? pretty : "unknown",
            version[0] ? version : "unknown", variant[0] ? ", variant " : "", variant, facts.steamos ? "yes" : "no");
    char a[72], b[72], c[72], d[72];
    put(&text,
        "System: session mode=%s gamescope=%s steam=%s steam_deck=%s session_type=%s desktop=%s wayland_display=%s "
        "x11_display=%s sdl_videodriver=%s appimage=%s\n",
        sos_system_mode_name(&facts), facts.gamescope ? "yes" : "no", facts.steam ? "yes" : "no",
        facts.steam_deck ? "yes" : "no", clean(environment, "XDG_SESSION_TYPE", a, sizeof(a)),
        clean(environment, "XDG_CURRENT_DESKTOP", b, sizeof(b)), facts.wayland ? "yes" : "no", facts.x11 ? "yes" : "no",
        clean(environment, "SDL_VIDEODRIVER", c, sizeof(c)), facts.appimage ? "yes" : "no");
    put(&text, "System: overlays and layers steam_overlay=%s mangohud=%s vkbasalt=%s vk_instance_layers=%s "
               "custom_vulkan_driver_list=%s\n",
        facts.steam_overlay ? "yes" : "no", value_of(environment, "MANGOHUD") ? "yes" : "no",
        value_of(environment, "ENABLE_VKBASALT") ? "yes" : "no", clean(environment, "VK_INSTANCE_LAYERS", d, sizeof(d)),
        value_of(environment, "VK_ICD_FILENAMES") || value_of(environment, "VK_DRIVER_FILES") ? "yes" : "no");
    if (meminfo) {
        put(&text, "System: memory total %.1f GiB, available %.1f GiB, swap %.1f GiB\n", gib(meminfo_kb(meminfo, "MemTotal")),
            gib(meminfo_kb(meminfo, "MemAvailable")), gib(meminfo_kb(meminfo, "SwapTotal")));
    }
    return text.used;
}

static const char *process_environment(const char *name) { return getenv(name); }

#ifndef _WIN32
static char *read_small_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *text = malloc(16384);
    size_t used = text ? fread(text, 1, 16383, file) : 0;
    fclose(file);
    if (text) text[used] = 0;
    return text;
}
#endif

void sos_system_report_log(const char *port_version) {
    char report[2048];
#ifdef _WIN32
    char kernel[64] = "unknown";
    {
        typedef LONG(WINAPI * GetVersionFunction)(OSVERSIONINFOW *);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const GetVersionFunction get_version =
            ntdll ? (GetVersionFunction)(void (*)(void))GetProcAddress(ntdll, "RtlGetVersion") : NULL;
        OSVERSIONINFOW info;
        memset(&info, 0, sizeof(info));
        info.dwOSVersionInfoSize = sizeof(info);
        if (get_version && get_version(&info) == 0)
            snprintf(kernel, sizeof(kernel), "%lu.%lu build %lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    }
    char meminfo[160] = "";
    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        snprintf(meminfo, sizeof(meminfo), "MemTotal: %llu kB\nMemAvailable: %llu kB\nSwapTotal: 0 kB\n",
                 (unsigned long long)(memory.ullTotalPhys / 1024), (unsigned long long)(memory.ullAvailPhys / 1024));
    sos_system_report_format(report, sizeof(report), port_version, process_environment, NULL, meminfo[0] ? meminfo : NULL, kernel,
                             "x86_64");
#else
    struct utsname name;
    const int named = uname(&name) == 0;
    char *os_release = read_small_file("/etc/os-release");
    if (!os_release) os_release = read_small_file("/usr/lib/os-release");
    char *meminfo = read_small_file("/proc/meminfo");
    sos_system_report_format(report, sizeof(report), port_version, process_environment, os_release, meminfo,
                             named ? name.release : NULL, named ? name.machine : NULL);
    free(os_release);
    free(meminfo);
#endif
    fputs(report, stdout);
    fflush(stdout);
}
