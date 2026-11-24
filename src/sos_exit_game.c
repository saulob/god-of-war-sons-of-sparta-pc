#include "sos_exit_game.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "sos_exit_game_sites.h"
#include "sos_skip_intro.h"

enum {
    LANG_LAMS_ID = 1,
    LANG_LAMS_CHECK = 2,
    MAX_NAME_CHARS = 64,
    MAX_LABEL_UTF16 = 64
};

typedef struct {
    const char *build;
    const SosExitGameSite *site;
} BuildEntry;

static const BuildEntry kBuilds[] = {
    {"CUSA57767-1.00", &kExitGame_1_00},
    {"CUSA57767-1.01", &kExitGame_1_01},
};

/* Settings.TextLanguageOptions values (0, 3..24); 1 and 2 are internal LAMS debug modes. */
static const SosExitGameLanguage kLanguages[SOS_EXIT_GAME_LANGUAGE_COUNT] = {
    {0, "English", "EXIT", "EXIT GAME"},
    {3, "Arabic", "خروج", "خروج من اللعبة"},
    {4, "Portuguese (Brazil)", "SAIR", "SAIR DO JOGO"},
    {5, "Croatian", "IZAĐI", "IZLAZ IZ IGRE"},
    {6, "Czech", "ODEJÍT", "UKONČIT HRU"},
    {7, "Dutch", "AFSLUITEN", "GAME AFSLUITEN"},
    {8, "English (UK)", "EXIT", "EXIT GAME"},
    {9, "French", "QUITTER", "QUITTER LE JEU"},
    {10, "German", "VERLASSEN", "SPIEL BEENDEN"},
    {11, "Greek", "ΕΞΟΔΟΣ", "ΕΞΟΔΟΣ"},
    {12, "Hungarian", "KILÉPÉS", "KILÉPÉS A JÁTÉKBÓL"},
    {13, "Italian", "ESCI", "ESCI DAL GIOCO"},
    {14, "Japanese", "終了", "ゲームを終了"},
    {15, "Korean", "나가기", "게임 종료"},
    {16, "Spanish (Latin America)", "SALIR", "SALIR DEL JUEGO"},
    {17, "Polish", "WYJDŹ", "WYJŚCIE Z GRY"},
    {18, "Portuguese (Portugal)", "SAIR", "SAIR DO JOGO"},
    {19, "Russian", "ВЫХОД", "ВЫХОД ИЗ ИГРЫ"},
    {20, "Simplified Chinese", "退出", "退出游戏"},
    {21, "Spanish (Spain)", "SALIR", "SALIR DEL JUEGO"},
    {22, "Thai", "ออก", "ออกจากเกม"},
    {23, "Traditional Chinese", "離開", "離開遊戲"},
    {24, "Turkish", "ÇIKIŞ", "OYUNDAN ÇIK"},
};

static struct {
    int armed;
    SosExitGameSite site;
    SosExitGameBindings bindings;
    SosExitGameHostHandler host_exit;
    atomic_int requested;
    atomic_int installed_logged;
    atomic_int logged_language;
} g_exit = {.logged_language = -1};

const SosExitGameSite *sos_exit_game_site(const char *build_key) {
    for (size_t i = 0; build_key && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(build_key, kBuilds[i].build)) return kBuilds[i].site;
    return NULL;
}

const SosExitGameLanguage *sos_exit_game_languages(size_t *count) {
    if (count) *count = SOS_EXIT_GAME_LANGUAGE_COUNT;
    return kLanguages;
}

static const SosExitGameLanguage *find_language_by_option(int option) {
    for (size_t i = 0; i < SOS_EXIT_GAME_LANGUAGE_COUNT; ++i)
        if (kLanguages[i].option == option) return &kLanguages[i];
    return NULL;
}

const char *sos_exit_game_translation(int language_option, const char **language_name) {
    const SosExitGameLanguage *entry = find_language_by_option(language_option);
    if (!entry) return NULL;
    if (language_name) *language_name = entry->name;
    return entry->text;
}

const char *sos_exit_game_translation_for_original(const char *original_utf8, const char **language_name) {
    if (!original_utf8 || !original_utf8[0]) return NULL;
    for (size_t i = 0; i < SOS_EXIT_GAME_LANGUAGE_COUNT; ++i) {
        if (!strcmp(kLanguages[i].original_9203, original_utf8)) {
            if (language_name) *language_name = kLanguages[i].name;
            return kLanguages[i].text;
        }
    }
    return NULL;
}

static uint64_t fnv1a(uint64_t hash, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

static int ranges_overlap(uint64_t a_begin, uint64_t a_len, uint64_t b_begin, uint64_t b_len) {
    return a_begin < b_begin + b_len && b_begin < a_begin + a_len;
}

int sos_exit_game_verify(const uint8_t *image, uint64_t size, const char *build_key, const char **why) {
    const SosExitGameSite *site = sos_exit_game_site(build_key);
    const char *reason = NULL;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    if (!site) reason = "not a supported build";
    else if (!image) reason = "missing guest image";
    for (int i = 0; !reason && i < SOS_EXIT_GAME_RANGES; ++i) {
        const SosExitGameRange r = site->ranges[i];
        if (r.begin >= r.end || r.end > size) reason = "image ends before exit-game code";
        else hash = fnv1a(hash, image + r.begin, r.end - r.begin);
    }
    if (!reason && (uint64_t)site->localization_class_slot + 8 > size) reason = "localization slot outside image";
    if (!reason) {
        const uint64_t sites[4] = {site->toggle_method, site->confirm_method, site->quit_method, site->text_method};
        const uint64_t lens[4] = {SOS_EXIT_GAME_TOGGLE_STOLEN, SOS_EXIT_GAME_CONFIRM_STOLEN,
                                  SOS_EXIT_GAME_QUIT_ENTRY, SOS_EXIT_GAME_TEXT_STOLEN};
        for (int i = 0; !reason && i < 4; ++i) {
            if (sites[i] + lens[i] > size) reason = "hook site outside image";
            for (int j = i + 1; !reason && j < 4; ++j)
                if (ranges_overlap(sites[i], lens[i], sites[j], lens[j])) reason = "overlapping hook sites";
        }
    }
    if (!reason && memcmp(image + site->toggle_method, site->toggle_entry, SOS_EXIT_GAME_TOGGLE_STOLEN))
        reason = "PlatformSpecificToggler::OnEnable bytes mismatch";
    else if (!reason && memcmp(image + site->confirm_method, site->confirm_entry, SOS_EXIT_GAME_CONFIRM_STOLEN))
        reason = "ExecuteEventOnAnimationFinished::PlayAnimation bytes mismatch";
    else if (!reason && memcmp(image + site->quit_method, site->quit_entry, SOS_EXIT_GAME_QUIT_ENTRY))
        reason = "QuitApplication::Quit bytes mismatch";
    else if (!reason && memcmp(image + site->text_method, site->text_entry, SOS_EXIT_GAME_TEXT_STOLEN))
        reason = "LocalizeStringEvent::UpdateString bytes mismatch";
    else if (!reason && hash != site->hash)
        reason = "code fingerprint mismatch";
    if (reason && why) *why = reason;
    return reason == NULL;
}

/* System V entry has rsp % 16 == 8; six pushes keep it 8 mod 16, sub rsp,8 aligns to 16 before call. */
static const uint8_t kSaveHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
static const uint8_t kRestoreTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
static const uint8_t kRetIfNonZero[] = {0x85, 0xc0, 0x74, 0x01, 0xc3};
static const uint8_t kSwapRsiIfNonNull[] = {0x48, 0x85, 0xc0, 0x74, 0x03, 0x48, 0x89, 0xc6};
static const uint8_t kQuitHead[] = {0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
static const uint8_t kQuitTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0xc3};

enum {
    TOGGLE_STUB_SIZE = sizeof(kSaveHead) + 8 + sizeof(kRestoreTail) + sizeof(kRetIfNonZero) + SOS_EXIT_GAME_TOGGLE_STOLEN + 5,
    CONFIRM_STUB_SIZE = sizeof(kSaveHead) + 8 + sizeof(kRestoreTail) + sizeof(kRetIfNonZero) + SOS_EXIT_GAME_CONFIRM_STOLEN + 5,
    QUIT_STUB_SIZE = sizeof(kQuitHead) + 8 + sizeof(kQuitTail),
    TEXT_STUB_SIZE = sizeof(kSaveHead) + 8 + sizeof(kRestoreTail) + sizeof(kSwapRsiIfNonNull) + SOS_EXIT_GAME_TEXT_STOLEN + 5
};
_Static_assert(TOGGLE_STUB_SIZE <= SOS_EXIT_GAME_STUB_SLOT, "toggle stub fits slot");
_Static_assert(CONFIRM_STUB_SIZE <= SOS_EXIT_GAME_STUB_SLOT, "confirm stub fits slot");
_Static_assert(QUIT_STUB_SIZE <= SOS_EXIT_GAME_STUB_SLOT, "quit stub fits slot");
_Static_assert(TEXT_STUB_SIZE <= SOS_EXIT_GAME_STUB_SLOT, "text stub fits slot");

static int reaches(const uint8_t *from_end, const uint8_t *to) {
    const int64_t delta = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from_end;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

static void write_jmp(uint8_t *at, const uint8_t *target) {
    const int32_t rel = (int32_t)((int64_t)(intptr_t)target - (int64_t)(intptr_t)(at + 5));
    at[0] = 0xe9;
    memcpy(at + 1, &rel, sizeof(rel));
}

static void emit_skip_stub(uint8_t *stub, uint64_t handler, const uint8_t *stolen, size_t stolen_len, const uint8_t *resume) {
    uint8_t *at = stub;
    memcpy(at, kSaveHead, sizeof(kSaveHead));
    at += sizeof(kSaveHead);
    memcpy(at, &handler, sizeof(handler));
    at += sizeof(handler);
    memcpy(at, kRestoreTail, sizeof(kRestoreTail));
    at += sizeof(kRestoreTail);
    memcpy(at, kRetIfNonZero, sizeof(kRetIfNonZero));
    at += sizeof(kRetIfNonZero);
    memcpy(at, stolen, stolen_len);
    at += stolen_len;
    write_jmp(at, resume);
}

static void emit_text_stub(uint8_t *stub, uint64_t handler, const uint8_t *stolen, size_t stolen_len, const uint8_t *resume) {
    uint8_t *at = stub;
    memcpy(at, kSaveHead, sizeof(kSaveHead));
    at += sizeof(kSaveHead);
    memcpy(at, &handler, sizeof(handler));
    at += sizeof(handler);
    memcpy(at, kRestoreTail, sizeof(kRestoreTail));
    at += sizeof(kRestoreTail);
    memcpy(at, kSwapRsiIfNonNull, sizeof(kSwapRsiIfNonNull));
    at += sizeof(kSwapRsiIfNonNull);
    memcpy(at, stolen, stolen_len);
    at += stolen_len;
    write_jmp(at, resume);
}

static void emit_quit_stub(uint8_t *stub, uint64_t handler) {
    uint8_t *at = stub;
    memcpy(at, kQuitHead, sizeof(kQuitHead));
    at += sizeof(kQuitHead);
    memcpy(at, &handler, sizeof(handler));
    at += sizeof(handler);
    memcpy(at, kQuitTail, sizeof(kQuitTail));
}

static void patch_entry(uint8_t *method, size_t stolen_len, const uint8_t *stub) {
    write_jmp(method, stub);
    if (stolen_len > 5) memset(method + 5, 0x90, stolen_len - 5);
}

int sos_exit_game_install(uint8_t *image, uint64_t size, const char *build_key, uint8_t *stubs, size_t stubs_capacity,
                          char *message, size_t message_size) {
    const char *why = "";
    if (!sos_exit_game_verify(image, size, build_key, &why)) {
        if (message && message_size)
            snprintf(message, message_size, "unavailable: %s (%s)", why, build_key ? build_key : "unknown");
        return 0;
    }
    if (!stubs || stubs_capacity < SOS_EXIT_GAME_STUB_BYTES) {
        if (message && message_size) snprintf(message, message_size, "unavailable: stub buffer too small");
        return 0;
    }
    const SosExitGameSite *site = sos_exit_game_site(build_key);
    uint8_t *const m_toggle = image + site->toggle_method;
    uint8_t *const m_confirm = image + site->confirm_method;
    uint8_t *const m_quit = image + site->quit_method;
    uint8_t *const m_text = image + site->text_method;
    uint8_t *const s_toggle = stubs + 0 * SOS_EXIT_GAME_STUB_SLOT;
    uint8_t *const s_confirm = stubs + 1 * SOS_EXIT_GAME_STUB_SLOT;
    uint8_t *const s_quit = stubs + 2 * SOS_EXIT_GAME_STUB_SLOT;
    uint8_t *const s_text = stubs + 3 * SOS_EXIT_GAME_STUB_SLOT;

    if (!reaches(m_toggle + 5, s_toggle) || !reaches(s_toggle + TOGGLE_STUB_SIZE, m_toggle + SOS_EXIT_GAME_TOGGLE_STOLEN) ||
        !reaches(m_confirm + 5, s_confirm) || !reaches(s_confirm + CONFIRM_STUB_SIZE, m_confirm + SOS_EXIT_GAME_CONFIRM_STOLEN) ||
        !reaches(m_quit + 5, s_quit) ||
        !reaches(m_text + 5, s_text) || !reaches(s_text + TEXT_STUB_SIZE, m_text + SOS_EXIT_GAME_TEXT_STOLEN)) {
        if (message && message_size) snprintf(message, message_size, "unavailable: stubs out of rel32 range");
        return 0;
    }

    emit_skip_stub(s_toggle, (uint64_t)(uintptr_t)sos_exit_game_on_toggle, m_toggle, SOS_EXIT_GAME_TOGGLE_STOLEN,
                   m_toggle + SOS_EXIT_GAME_TOGGLE_STOLEN);
    emit_skip_stub(s_confirm, (uint64_t)(uintptr_t)sos_exit_game_on_confirm, m_confirm, SOS_EXIT_GAME_CONFIRM_STOLEN,
                   m_confirm + SOS_EXIT_GAME_CONFIRM_STOLEN);
    emit_quit_stub(s_quit, (uint64_t)(uintptr_t)sos_exit_game_on_quit_app);
    emit_text_stub(s_text, (uint64_t)(uintptr_t)sos_exit_game_on_text, m_text, SOS_EXIT_GAME_TEXT_STOLEN,
                   m_text + SOS_EXIT_GAME_TEXT_STOLEN);

    patch_entry(m_toggle, SOS_EXIT_GAME_TOGGLE_STOLEN, s_toggle);
    patch_entry(m_confirm, SOS_EXIT_GAME_CONFIRM_STOLEN, s_confirm);
    patch_entry(m_quit, 5, s_quit);
    patch_entry(m_text, SOS_EXIT_GAME_TEXT_STOLEN, s_text);

    if (message && message_size)
        snprintf(message, message_size, "hooks installed (%s, %d languages)", build_key, SOS_EXIT_GAME_LANGUAGE_COUNT);
    return 1;
}

void sos_exit_game_reset(void) {
    memset(&g_exit, 0, sizeof(g_exit));
    atomic_store(&g_exit.requested, 0);
    atomic_store(&g_exit.installed_logged, 0);
    atomic_store(&g_exit.logged_language, -1);
}

void sos_exit_game_arm_with(const SosExitGameSite *site, const SosExitGameBindings *bindings,
                            SosExitGameHostHandler host_exit) {
    sos_exit_game_reset();
    if (!site || !bindings) return;
    g_exit.site = *site;
    g_exit.bindings = *bindings;
    g_exit.host_exit = host_exit;
    g_exit.armed = 1;
}

void sos_exit_game_arm(const uint8_t *image, const char *build_key, SosExitGameHostHandler host_exit) {
    const SosExitGameSite *site = sos_exit_game_site(build_key);
    if (!site || !image) {
        sos_exit_game_reset();
        return;
    }
    SosExitGameBindings bindings;
    bindings.get_game_object = (void *(SOS_EXIT_GAME_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_game_object);
    bindings.get_transform = (void *(SOS_EXIT_GAME_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_transform);
    bindings.get_parent = (void *(SOS_EXIT_GAME_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_parent);
    bindings.get_name = (void *(SOS_EXIT_GAME_SYSV *)(void *, const void *))(uintptr_t)(image + site->get_name);
    bindings.alloc_string = (void *(SOS_EXIT_GAME_SYSV *)(int32_t, const void *))(uintptr_t)(image + site->alloc_string);
    bindings.localization_class_slot = (const uintptr_t *)(image + site->localization_class_slot);
    sos_exit_game_arm_with(site, &bindings, host_exit);
}

int sos_exit_game_requested(void) {
    return atomic_load(&g_exit.requested);
}

static void *pointer_at(const void *object, size_t offset) {
    void *value;
    memcpy(&value, (const uint8_t *)object + offset, sizeof(value));
    return value;
}

static int32_t int_at(const void *object, size_t offset) {
    int32_t value;
    memcpy(&value, (const uint8_t *)object + offset, sizeof(value));
    return value;
}

static int alive(const void *object) {
    return object != NULL && pointer_at(object, g_exit.site.native_object_offset) != NULL;
}

static size_t utf8_to_utf16(const char *utf8, uint16_t *out, size_t capacity) {
    size_t count = 0;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1f) << 6) | (uint32_t)(p[1] & 0x3f);
            p += 2;
        } else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (uint32_t)(p[2] & 0x3f);
            p += 3;
        } else if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                 ((uint32_t)(p[2] & 0x3f) << 6) | (uint32_t)(p[3] & 0x3f);
            p += 4;
        } else {
            return 0;
        }
        if (cp <= 0xffff) {
            if (count >= capacity) return 0;
            out[count++] = (uint16_t)cp;
        } else {
            if (count + 1 >= capacity) return 0;
            cp -= 0x10000;
            out[count++] = (uint16_t)(0xd800u | (cp >> 10));
            out[count++] = (uint16_t)(0xdc00u | (cp & 0x3ffu));
        }
    }
    return count;
}

static int il2cpp_string_equals_utf8(const void *str, const char *utf8) {
    if (!str || !utf8) return 0;
    const int32_t len = int_at(str, g_exit.site.string_length_offset);
    if (len < 0 || len > MAX_NAME_CHARS) return 0;
    uint16_t expected[MAX_NAME_CHARS];
    const size_t expected_len = utf8_to_utf16(utf8, expected, MAX_NAME_CHARS);
    if ((size_t)len != expected_len) return 0;
    const uint8_t *chars = (const uint8_t *)str + g_exit.site.string_chars_offset;
    return !memcmp(chars, expected, expected_len * sizeof(uint16_t));
}

static int object_name_is(void *object, const char *name) {
    if (!alive(object) || !g_exit.bindings.get_name) return 0;
    void *const str = g_exit.bindings.get_name(object, NULL);
    return il2cpp_string_equals_utf8(str, name);
}

static void *parent_game_object_of_transform(void *transform) {
    if (!alive(transform) || !g_exit.bindings.get_parent || !g_exit.bindings.get_game_object) return NULL;
    void *const parent = g_exit.bindings.get_parent(transform, NULL);
    return alive(parent) ? g_exit.bindings.get_game_object(parent, NULL) : NULL;
}

static int is_menu_quit_button(void *component) {
    if (!alive(component) || !g_exit.bindings.get_game_object || !g_exit.bindings.get_transform) return 0;
    void *const go = g_exit.bindings.get_game_object(component, NULL);
    if (!object_name_is(go, "QuitButton")) return 0;
    void *const tf = g_exit.bindings.get_transform(component, NULL);
    void *const parent_go = parent_game_object_of_transform(tf);
    return object_name_is(parent_go, "MenuChoices");
}

static int is_menu_quit_label(void *component) {
    if (!alive(component) || !g_exit.bindings.get_game_object || !g_exit.bindings.get_transform || !g_exit.bindings.get_parent)
        return 0;
    void *const go = g_exit.bindings.get_game_object(component, NULL);
    if (!object_name_is(go, "Text") && !object_name_is(go, "ActiveText")) return 0;
    void *const tf = g_exit.bindings.get_transform(component, NULL);
    void *const parent_tf = alive(tf) ? g_exit.bindings.get_parent(tf, NULL) : NULL;
    if (!alive(parent_tf)) return 0;
    void *const parent_go = g_exit.bindings.get_game_object(parent_tf, NULL);
    if (!object_name_is(parent_go, "QuitButton")) return 0;
    void *const grand_go = parent_game_object_of_transform(parent_tf);
    return object_name_is(grand_go, "MenuChoices");
}

static int current_text_language(void) {
    if (!g_exit.bindings.localization_class_slot) return -1;
    const uintptr_t klass = *g_exit.bindings.localization_class_slot;
    if (!klass || (klass & 1u)) return -1;
    const void *const statics = pointer_at((const void *)klass, g_exit.site.static_fields_offset);
    if (!statics) return -1;
    const void *const instance = pointer_at(statics, 0);
    if (!instance) return -1;
    return int_at(instance, g_exit.site.text_language_offset);
}

static const SosExitGameLanguage *find_language_by_original_il2cpp(const void *str) {
    if (!str) return NULL;
    for (size_t i = 0; i < SOS_EXIT_GAME_LANGUAGE_COUNT; ++i)
        if (il2cpp_string_equals_utf8(str, kLanguages[i].original_9203)) return &kLanguages[i];
    return NULL;
}

static void *alloc_managed_utf8(const char *utf8) {
    if (!utf8 || !g_exit.bindings.alloc_string) return NULL;
    uint16_t units[MAX_LABEL_UTF16];
    const size_t len = utf8_to_utf16(utf8, units, MAX_LABEL_UTF16);
    if (!len) return NULL;
    void *const managed = g_exit.bindings.alloc_string((int32_t)len, NULL);
    if (!managed) return NULL;
    uint8_t *const dst = (uint8_t *)managed + g_exit.site.string_chars_offset;
    memcpy(dst, units, len * sizeof(uint16_t));
    const uint16_t zero = 0;
    memcpy(dst + len * sizeof(uint16_t), &zero, sizeof(zero));
    return managed;
}

static void request_host_exit(void) {
    if (atomic_exchange(&g_exit.requested, 1)) return;
    puts("Exit Game: exit requested");
    if (g_exit.host_exit) (void)g_exit.host_exit();
}

SOS_EXIT_GAME_SYSV int sos_exit_game_on_toggle(void *toggler) {
    if (!g_exit.armed || !is_menu_quit_button(toggler)) return 0;
    if (!atomic_exchange(&g_exit.installed_logged, 1)) puts("Exit Game: title menu installed");
    return 1;
}

static void title_animation_event(void *anim_event) {
    if (!sos_skip_intro_watching_title() || !alive(anim_event) || !g_exit.bindings.get_game_object) return;
    void *const go = g_exit.bindings.get_game_object(anim_event, NULL);
    if (object_name_is(go, "IntroState"))
        sos_skip_intro_watch_title_animation(anim_event);
    else if (object_name_is(go, "WaitForInputPressState"))
        sos_skip_intro_dismiss_title();
}

SOS_EXIT_GAME_SYSV int sos_exit_game_on_confirm(void *anim_event) {
    if (!g_exit.armed) return 0;
    title_animation_event(anim_event);
    if (!is_menu_quit_button(anim_event)) return 0;
    request_host_exit();
    return 1;
}

SOS_EXIT_GAME_SYSV void sos_exit_game_on_quit_app(void) {
    if (!g_exit.armed) return;
    request_host_exit();
}

SOS_EXIT_GAME_SYSV void *sos_exit_game_on_text(void *localize_event, void *value) {
    if (!g_exit.armed || !is_menu_quit_label(localize_event)) return NULL;
    const int option = current_text_language();
    if (option == LANG_LAMS_ID || option == LANG_LAMS_CHECK) return NULL;
    const SosExitGameLanguage *entry = find_language_by_option(option);
    if (!entry && option < 0) entry = find_language_by_original_il2cpp(value);
    if (!entry) return NULL;
    if (atomic_exchange(&g_exit.logged_language, entry->option) != entry->option)
        printf("Exit Game: language %s\n", entry->name);
    return alloc_managed_utf8(entry->text);
}
