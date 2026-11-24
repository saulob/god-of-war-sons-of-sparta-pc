#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>

#include "controller_prompts.h"
#include "prompt_dialog.h"
#include "sos_exit_game.h"
#include "sos_skip_intro.h"

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

constexpr const char* kBuilds[] = {"CUSA57767-1.00", "CUSA57767-1.01"};

struct FakeObj {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x100, 0);
    void* ptr() { return bytes.data(); }
    void SetAlive(bool alive) {
        void* marker = alive ? reinterpret_cast<void*>(0x1234) : nullptr;
        std::memcpy(bytes.data() + 0x10, &marker, sizeof(marker));
    }
    void SetPtr(size_t offset, const void* value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
    void SetInt(size_t offset, int32_t value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
};

struct FakeIl2CppString {
    uint8_t header[0x10] = {};
    int32_t length = 0;
    uint16_t chars[128] = {};
};

std::u16string Utf8ToU16(const std::string& utf8) {
    std::u16string out;
    const auto* p = reinterpret_cast<const uint8_t*>(utf8.data());
    const auto* end = p + utf8.size();
    while (p < end) {
        uint32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && p + 1 < end) {
            cp = ((uint32_t)(p[0] & 0x1f) << 6) | (uint32_t)(p[1] & 0x3f);
            p += 2;
        } else if ((*p & 0xf0) == 0xe0 && p + 2 < end) {
            cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (uint32_t)(p[2] & 0x3f);
            p += 3;
        } else if ((*p & 0xf8) == 0xf0 && p + 3 < end) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) | ((uint32_t)(p[2] & 0x3f) << 6) | (uint32_t)(p[3] & 0x3f);
            p += 4;
        } else {
            break;
        }
        if (cp <= 0xffff) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800u | (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00u | (cp & 0x3ffu)));
        }
    }
    return out;
}

FakeIl2CppString MakeFakeString(const std::string& utf8) {
    FakeIl2CppString s;
    const std::u16string u16 = Utf8ToU16(utf8);
    s.length = static_cast<int32_t>(u16.size());
    for (size_t i = 0; i < u16.size() && i < 127; ++i) s.chars[i] = static_cast<uint16_t>(u16[i]);
    s.chars[std::min<size_t>(u16.size(), 127)] = 0;
    return s;
}

std::u16string ReadFakeString(const void* ptr) {
    if (!ptr) return {};
    const auto* s = static_cast<const FakeIl2CppString*>(ptr);
    if (s->length <= 0 || s->length > 127) return {};
    std::u16string out;
    out.reserve(static_cast<size_t>(s->length));
    for (int32_t i = 0; i < s->length; ++i) out.push_back(static_cast<char16_t>(s->chars[i]));
    return out;
}

struct FakeWorld {
    std::map<const void*, void*> compToGo;
    std::map<const void*, void*> compToTf;
    std::map<const void*, void*> tfToParent;
    std::map<const void*, FakeIl2CppString> objNames;
    std::vector<FakeIl2CppString> allocated;
    int hostExitCalls = 0;
    int saveWrites = 0;
    int badArgs = 0;
};

FakeWorld* g_world = nullptr;

SOS_EXIT_GAME_SYSV void* FakeGetGameObject(void* comp, const void* method) {
    g_world->badArgs += method != nullptr;
    const auto it = g_world->compToGo.find(comp);
    return it != g_world->compToGo.end() ? it->second : nullptr;
}

SOS_EXIT_GAME_SYSV void* FakeGetTransform(void* comp, const void* method) {
    g_world->badArgs += method != nullptr;
    const auto it = g_world->compToTf.find(comp);
    return it != g_world->compToTf.end() ? it->second : nullptr;
}

SOS_EXIT_GAME_SYSV void* FakeGetParent(void* tf, const void* method) {
    g_world->badArgs += method != nullptr;
    const auto it = g_world->tfToParent.find(tf);
    return it != g_world->tfToParent.end() ? it->second : nullptr;
}

SOS_EXIT_GAME_SYSV void* FakeGetName(void* obj, const void* method) {
    g_world->badArgs += method != nullptr;
    const auto it = g_world->objNames.find(obj);
    return it != g_world->objNames.end() ? &it->second : nullptr;
}

SOS_EXIT_GAME_SYSV void* FakeAllocString(int32_t length, const void* method) {
    g_world->badArgs += method != nullptr;
    if (length < 0 || length > 127) return nullptr;
    g_world->allocated.emplace_back();
    g_world->allocated.back().length = length;
    return &g_world->allocated.back();
}

int FakeHostExit() {
    ++g_world->hostExitCalls;
    return 1;
}

int g_titleAnimationCancels = 0;
void* g_titleAnimationCancelled = nullptr;

void SOS_SKIP_INTRO_SYSV FakeCancelTitleAnimation(void* component) {
    ++g_titleAnimationCancels;
    g_titleAnimationCancelled = component;
}

void TestTables() {
    for (const char* key : kBuilds) {
        const std::string name = key;
        const SosExitGameSite* site = sos_exit_game_site(key);
        Check(site != nullptr, name + ": site present");
        if (!site) continue;
        Check(site->hash != 0, name + ": fingerprint non-zero");
        Check(site->toggle_method == site->ranges[0].begin && site->confirm_method == site->ranges[1].begin &&
                  site->quit_method == site->ranges[2].begin && site->text_method == site->ranges[3].begin,
              name + ": first four hashed ranges are the four hooked methods");
        Check(site->get_game_object == site->ranges[4].begin && site->get_transform == site->ranges[5].begin &&
                  site->get_parent == site->ranges[6].begin && site->get_name == site->ranges[7].begin &&
                  site->alloc_string == site->ranges[8].begin,
              name + ": ranges 4..8 are the five called helpers");
        Check(site->native_object_offset == 0x10 && site->static_fields_offset == 0xb8 && site->text_language_offset == 0x40 &&
                  site->string_length_offset == 0x10 && site->string_chars_offset == 0x14,
              name + ": object field offsets verified");
    }
    Check(sos_exit_game_site("CUSA57767-9.99") == nullptr && sos_exit_game_site("") == nullptr && sos_exit_game_site(nullptr) == nullptr,
          "unknown builds have no site");
}

void TestTranslations() {
    size_t count = 0;
    const SosExitGameLanguage* langs = sos_exit_game_languages(&count);
    Check(count == 23 && langs != nullptr, "all 23 game text languages present");

    const std::map<int, std::string> expected = {
        {0, "EXIT GAME"}, {3, "خروج من اللعبة"}, {4, "SAIR DO JOGO"}, {5, "IZLAZ IZ IGRE"},
        {6, "UKONČIT HRU"}, {7, "GAME AFSLUITEN"}, {8, "EXIT GAME"}, {9, "QUITTER LE JEU"},
        {10, "SPIEL BEENDEN"}, {11, "ΕΞΟΔΟΣ"}, {12, "KILÉPÉS A JÁTÉKBÓL"}, {13, "ESCI DAL GIOCO"},
        {14, "ゲームを終了"}, {15, "게임 종료"}, {16, "SALIR DEL JUEGO"}, {17, "WYJŚCIE Z GRY"},
        {18, "SAIR DO JOGO"}, {19, "ВЫХОД ИЗ ИГРЫ"}, {20, "退出游戏"}, {21, "SALIR DEL JUEGO"},
        {22, "ออกจากเกม"}, {23, "離開遊戲"}, {24, "OYUNDAN ÇIK"},
    };
    for (const auto& [opt, exp] : expected) {
        const char* lname = nullptr;
        const char* got = sos_exit_game_translation(opt, &lname);
        Check(got != nullptr && std::string(got) == exp && lname != nullptr && lname[0] != '\0',
              "translation option " + std::to_string(opt) + " matches " + exp);
    }
    Check(sos_exit_game_translation(1, nullptr) == nullptr && sos_exit_game_translation(2, nullptr) == nullptr,
          "LamsId and LamsCheck debug options are left untouched");
    Check(sos_exit_game_translation(-1, nullptr) == nullptr && sos_exit_game_translation(99, nullptr) == nullptr,
          "out-of-range language options return nullptr");
    Check(std::string(sos_exit_game_translation_for_original("SAIR", nullptr)) == "SAIR DO JOGO" &&
              std::string(sos_exit_game_translation_for_original("終了", nullptr)) == "ゲームを終了" &&
              std::string(sos_exit_game_translation_for_original("나가기", nullptr)) == "게임 종료" &&
              std::string(sos_exit_game_translation_for_original("退出", nullptr)) == "退出游戏" &&
              std::string(sos_exit_game_translation_for_original("離開", nullptr)) == "離開遊戲",
          "original key 9203 fallback resolves localized strings");
}

void TestHeadlessRuntime() {
    FakeWorld world;
    world.allocated.reserve(128);
    g_world = &world;

    // Build fake Unity object graph:
    // MenuChoices -> ContinueButton, NewGameButton, LoadGameButton, SettingsButton, CreditsButton, ThePitButton, QuitButton
    // QuitButton -> Text, ActiveText; CreditsButton -> Text
    // Canvas_ControlConfig -> MainCategories -> ExitGame -> Text
    struct Node {
        FakeObj go, tf, comp;
    };
    std::map<std::string, Node> nodes;
    auto makeNode = [&](const std::string& key, const std::string& unityName, const std::string& parentKey) {
        Node& n = nodes[key];
        n.go.SetAlive(true);
        n.tf.SetAlive(true);
        n.comp.SetAlive(true);
        world.objNames[n.go.ptr()] = MakeFakeString(unityName);
        world.compToGo[n.comp.ptr()] = n.go.ptr();
        world.compToGo[n.tf.ptr()] = n.go.ptr();
        world.compToTf[n.comp.ptr()] = n.tf.ptr();
        world.compToTf[n.tf.ptr()] = n.tf.ptr();
        if (!parentKey.empty()) world.tfToParent[n.tf.ptr()] = nodes[parentKey].tf.ptr();
    };

    makeNode("MenuChoices", "MenuChoices", "");
    const std::vector<std::string> existingRows = {"ContinueButton", "NewGameButton", "LoadGameButton", "SettingsButton", "CreditsButton"};
    for (const auto& r : existingRows) makeNode(r, r, "MenuChoices");
    makeNode("ThePitButton", "ThePitButton", "MenuChoices");
    makeNode("QuitButton", "QuitButton", "MenuChoices");
    makeNode("QuitText", "Text", "QuitButton");
    makeNode("QuitActiveText", "ActiveText", "QuitButton");
    makeNode("CreditsText", "Text", "CreditsButton");
    makeNode("MainCategories", "MainCategories", "");
    makeNode("ControlConfigExit", "ExitGame", "MainCategories");
    makeNode("ControlConfigExitText", "Text", "ControlConfigExit");
    makeNode("TitleIntroAnimation", "IntroState", "");
    makeNode("TitlePromptAnimation", "WaitForInputPressState", "");

    FakeObj locClass, locStatics, locInstance;
    locClass.SetPtr(0xb8, locStatics.ptr());
    locStatics.SetPtr(0x00, locInstance.ptr());
    locInstance.SetInt(0x40, 0);
    uintptr_t locSlot = reinterpret_cast<uintptr_t>(locClass.ptr());

    SosExitGameSite site = *sos_exit_game_site("CUSA57767-1.00");
    SosExitGameBindings bindings{FakeGetGameObject, FakeGetTransform, FakeGetParent, FakeGetName, FakeAllocString, &locSlot};
    sos_exit_game_arm_with(&site, &bindings, FakeHostExit);

    // 1. PlatformSpecificToggler: keeps MenuChoices/QuitButton active, leaves ControlConfig/ExitGame untouched
    Check(sos_exit_game_on_toggle(nodes["QuitButton"].comp.ptr()) == 1, "toggle hook keeps MenuChoices/QuitButton active");
    Check(sos_exit_game_on_toggle(nodes["ControlConfigExit"].comp.ptr()) == 0, "toggle hook leaves ControlConfig/ExitGame untouched");
    Check(sos_exit_game_on_toggle(nodes["CreditsButton"].comp.ptr()) == 0, "toggle hook ignores CreditsButton");
    Check(sos_exit_game_on_toggle(nullptr) == 0, "toggle hook safe on nullptr");

    // 2. Confirm on rows 0..4 and ThePitButton must NEVER request exit
    for (const auto& r : existingRows) {
        Check(sos_exit_game_on_confirm(nodes[r].comp.ptr()) == 0, "confirm on " + r + " does not intercept");
    }
    Check(sos_exit_game_on_confirm(nodes["ThePitButton"].comp.ptr()) == 0, "confirm on ThePitButton does not intercept");
    Check(world.hostExitCalls == 0 && sos_exit_game_requested() == 0, "rows 0..4 never request host exit");

    // 3. Back/cancel while QuitButton is selected does not exit (does not invoke confirm/quit)
    Check(world.hostExitCalls == 0 && sos_exit_game_requested() == 0 && world.saveWrites == 0,
          "selecting/cancelling on QuitButton never exits or writes save data");

    // 4. Localization hook on QuitButton/Text and QuitButton/ActiveText across all 23 languages
    size_t langCount = 0;
    const SosExitGameLanguage* langs = sos_exit_game_languages(&langCount);
    for (size_t i = 0; i < langCount; ++i) {
        locInstance.SetInt(0x40, langs[i].option);
        FakeIl2CppString orig = MakeFakeString(langs[i].original_9203);
        const std::u16string exp = Utf8ToU16(langs[i].text);
        void* resText = sos_exit_game_on_text(nodes["QuitText"].comp.ptr(), &orig);
        void* resActive = sos_exit_game_on_text(nodes["QuitActiveText"].comp.ptr(), &orig);
        Check(resText != nullptr && ReadFakeString(resText) == exp,
              std::string("QuitButton/Text localized for ") + langs[i].name);
        Check(resActive != nullptr && ReadFakeString(resActive) == exp,
              std::string("QuitButton/ActiveText localized for ") + langs[i].name);
    }
    // Non-QuitButton labels stay untouched
    locInstance.SetInt(0x40, 4);
    FakeIl2CppString creditsOrig = MakeFakeString("CRÉDITOS");
    Check(sos_exit_game_on_text(nodes["CreditsText"].comp.ptr(), &creditsOrig) == nullptr,
          "CreditsButton/Text is never modified");
    FakeIl2CppString cfgOrig = MakeFakeString("SAIR");
    Check(sos_exit_game_on_text(nodes["ControlConfigExitText"].comp.ptr(), &cfgOrig) == nullptr,
          "ControlConfig/ExitGame/Text is never modified");
    // Debug LAMS modes untouched
    locInstance.SetInt(0x40, 1);
    Check(sos_exit_game_on_text(nodes["QuitText"].comp.ptr(), &cfgOrig) == nullptr, "LamsId left untouched");
    locInstance.SetInt(0x40, 2);
    Check(sos_exit_game_on_text(nodes["QuitText"].comp.ptr(), &cfgOrig) == nullptr, "LamsCheck left untouched");
    // Fallback when LocalizationSubsystem::Instance is NULL
    locStatics.SetPtr(0x00, nullptr);
    FakeIl2CppString jpOrig = MakeFakeString("終了");
    void* fallbackRes = sos_exit_game_on_text(nodes["QuitText"].comp.ptr(), &jpOrig);
    Check(fallbackRes != nullptr && ReadFakeString(fallbackRes) == Utf8ToU16("ゲームを終了"),
          "fallback resolves Japanese when LocalizationSubsystem::Instance is NULL");
    // Tagged unresolved TypeInfo slot safe
    locSlot = 0x20000001u;
    void* taggedRes = sos_exit_game_on_text(nodes["QuitText"].comp.ptr(), &jpOrig);
    Check(taggedRes != nullptr && ReadFakeString(taggedRes) == Utf8ToU16("ゲームを終了"),
          "tagged unresolved slot handled safely");

    // 5. Confirm on QuitButton requests host exit EXACTLY ONCE; repeated confirms do not duplicate
    Check(sos_exit_game_on_confirm(nodes["QuitButton"].comp.ptr()) == 1, "confirm on QuitButton intercepted");
    Check(world.hostExitCalls == 1 && sos_exit_game_requested() == 1, "first QuitButton confirm requests host exit once");
    Check(sos_exit_game_on_confirm(nodes["QuitButton"].comp.ptr()) == 1, "second QuitButton confirm still skips animation");
    Check(world.hostExitCalls == 1, "repeated QuitButton confirm does not request host exit again");
    sos_exit_game_on_quit_app();
    Check(world.hostExitCalls == 1, "QuitApplication::Quit after confirm does not duplicate exit");

    // 6. Direct QuitApplication::Quit path (freshly re-armed)
    sos_exit_game_arm_with(&site, &bindings, FakeHostExit);
    world.hostExitCalls = 0;
    sos_exit_game_on_quit_app();
    sos_exit_game_on_quit_app();
    Check(world.hostExitCalls == 1 && sos_exit_game_requested() == 1, "QuitApplication::Quit requests host exit exactly once");

    // 7. Skip Intro's title guard: the prompt's hide cancels the pending intro animation watchers
    g_titleAnimationCancels = 0;
    g_titleAnimationCancelled = nullptr;
    sos_skip_intro_arm_title(site.native_object_offset, FakeCancelTitleAnimation);
    sos_skip_intro_arm();
    Check(sos_skip_intro_watching_title() == 1, "title guard: Skip Intro starts watching the title animations");
    Check(sos_exit_game_on_confirm(nodes["TitleIntroAnimation"].comp.ptr()) == 0, "title guard: an intro watcher is not a QuitButton animation");
    Check(sos_exit_game_on_confirm(nodes["TitlePromptAnimation"].comp.ptr()) == 0, "title guard: the prompt's hide is not a QuitButton animation");
    Check(g_titleAnimationCancels == 1 && g_titleAnimationCancelled == nodes["TitleIntroAnimation"].comp.ptr(),
          "title guard: hiding the prompt cancels the pending title intro watcher exactly once");
    Check(sos_skip_intro_watching_title() == 0, "title guard: the guard is over once the prompt is dismissed");
    Check(world.hostExitCalls == 1, "title guard: title animations never request host exit");

    Check(world.badArgs == 0, "all IL2CPP helper calls passed NULL MethodInfo cleanly");
    sos_exit_game_reset();
    g_world = nullptr;
}

bool ReadBootImage(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8];
    f.read(magic, 8);
    if (!f || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    auto u64 = [&]() {
        uint64_t v = 0;
        f.read(reinterpret_cast<char*>(&v), 8);
        return v;
    };
    const uint64_t size = u64();
    (void)u64();  // entry
    const uint64_t ns = u64();
    const uint64_t nr = u64();
    const uint64_t ic = u64();
    (void)u64();  // caps
    (void)u64();  // procparam
    for (int i = 0; i < 4; ++i) (void)u64();  // main tls
    const uint64_t mc = u64();
    f.seekg(static_cast<std::streamoff>(mc * 7 * 8), std::ios::cur);
    const uint64_t nb = u64();
    f.seekg(static_cast<std::streamoff>(nb * 3 * 8 + ns * 3 * 8 + ic * 128 + nr * 32), std::ios::cur);
    out.resize(size);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

fs::path BootPathFor(const std::string& buildKey) {
    const fs::path root(SOS_SOURCE_DIR);
    const fs::path primary = root / "build" / "image" / buildKey / "boot.bin";
    if (fs::is_regular_file(primary)) return primary;
    const size_t dash = buildKey.rfind('-');
    if (dash != std::string::npos) {
        const fs::path alt = root / "build" / "image" / ("v" + buildKey.substr(dash + 1)) / "boot.bin";
        if (fs::is_regular_file(alt)) return alt;
    }
    return {};
}

bool DecodeFirst(const uint8_t* code, size_t len, ZydisDecodedInstruction& insn, ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]) {
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code, len, &insn, ops));
}

void TestRealBootImage(const char* buildKey) {
    const std::string name = buildKey;
    const fs::path bootPath = BootPathFor(buildKey);
    Check(!bootPath.empty(), name + ": boot image present");
    if (bootPath.empty()) return;

    std::vector<uint8_t> original;
    Check(ReadBootImage(bootPath, original), name + ": boot image read cleanly");
    if (original.empty()) return;

    const SosExitGameSite* site = sos_exit_game_site(buildKey);
    const char* why = nullptr;
    Check(sos_exit_game_verify(original.data(), original.size(), buildKey, &why), name + ": real boot image verifies");

    // 1. Unknown build refuses and writes nothing
    {
        std::vector<uint8_t> buf(original.size() + 0x2000, 0);
        std::memcpy(buf.data(), original.data(), original.size());
        char msg[200] = {};
        const int ok = sos_exit_game_install(buf.data(), original.size(), "CUSA57767-9.99",
                                             buf.data() + original.size(), SOS_EXIT_GAME_STUB_BYTES, msg, sizeof(msg));
        Check(!ok && std::memcmp(buf.data(), original.data(), original.size()) == 0,
              name + ": unknown build refuses and leaves image untouched");
    }

    // 2. Truncated image refuses and writes nothing
    {
        std::vector<uint8_t> buf(original.data(), original.data() + 0x10000);
        std::vector<uint8_t> snap = buf;
        uint8_t stubs[SOS_EXIT_GAME_STUB_BYTES] = {};
        char msg[200] = {};
        const int ok = sos_exit_game_install(buf.data(), buf.size(), buildKey, stubs, sizeof(stubs), msg, sizeof(msg));
        Check(!ok && buf == snap, name + ": truncated image refuses cleanly");
    }

    // 3. Mutating ANY byte in any of the 9 ranges (4 hooks + 5 helpers) fails ALL-OR-NOTHING with zero bytes modified
    for (int r = 0; r < SOS_EXIT_GAME_RANGES; ++r) {
        std::vector<uint8_t> buf(original.size() + 0x2000, 0);
        std::memcpy(buf.data(), original.data(), original.size());
        const uint32_t flipAt = site->ranges[r].begin + (r < 4 ? 0u : 2u);
        buf[flipAt] ^= 0xff;
        std::vector<uint8_t> expected(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(original.size()));
        char msg[200] = {};
        const int ok = sos_exit_game_install(buf.data(), original.size(), buildKey,
                                             buf.data() + original.size(), SOS_EXIT_GAME_STUB_BYTES, msg, sizeof(msg));
        bool stubsZero = std::all_of(buf.begin() + static_cast<std::ptrdiff_t>(original.size()),
                                     buf.begin() + static_cast<std::ptrdiff_t>(original.size() + SOS_EXIT_GAME_STUB_BYTES),
                                     [](uint8_t b) { return b == 0; });
        Check(!ok && std::memcmp(buf.data(), expected.data(), original.size()) == 0 && stubsZero,
              name + ": flipped byte in range " + std::to_string(r) + " leaves image and stubs completely untouched");
    }

    // 4. Too-small stub buffer refuses with zero bytes written
    {
        std::vector<uint8_t> buf(original.size() + 0x2000, 0);
        std::memcpy(buf.data(), original.data(), original.size());
        char msg[200] = {};
        const int ok = sos_exit_game_install(buf.data(), original.size(), buildKey,
                                             buf.data() + original.size(), SOS_EXIT_GAME_STUB_BYTES - 1, msg, sizeof(msg));
        Check(!ok && std::memcmp(buf.data(), original.data(), original.size()) == 0,
              name + ": undersized stub buffer refused cleanly");
    }

    // 5. Valid installation + Zydis instruction verification + coexistence with Skip Intro, Prompts and Dialog hooks
    {
        const size_t totalStubs = SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES + SOS_PROMPT_DIALOG_STUB_BYTES +
                                  SOS_PROMPT_MENUS * SOS_PROMPT_MENU_STUB_BYTES + SOS_EXIT_GAME_STUB_BYTES;
        std::vector<uint8_t> buf(original.size() + totalStubs + 0x1000, 0);
        std::memcpy(buf.data(), original.data(), original.size());
        uint8_t* stubBase = buf.data() + original.size();
        char msg[200] = {};

        Check(sos_skip_intro_apply(buf.data(), original.size(), buildKey, stubBase, SOS_SKIP_INTRO_STUB_BYTES, msg, sizeof(msg)) != 0,
              name + ": Skip Intro installs alongside Exit Game");
        Check(sos_prompt_hook_install(buf.data(), original.size(), buildKey, stubBase + SOS_SKIP_INTRO_STUB_BYTES,
                                      SOS_PROMPT_HOOK_STUB_BYTES, msg, sizeof(msg)) != 0,
              name + ": Automatic prompt hook installs alongside Exit Game");
        Check(sos_prompt_dialog_install(buf.data(), original.size(), buildKey,
                                        stubBase + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES,
                                        SOS_PROMPT_DIALOG_STUB_BYTES, msg, sizeof(msg)) != 0,
              name + ": Settings dialog hook installs alongside Exit Game");
        for (int m = 0; m < SOS_PROMPT_MENUS; ++m) {
            Check(sos_prompt_menu_install(buf.data(), original.size(), buildKey, m,
                                          stubBase + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES +
                                              SOS_PROMPT_DIALOG_STUB_BYTES + static_cast<size_t>(m) * SOS_PROMPT_MENU_STUB_BYTES,
                                          SOS_PROMPT_MENU_STUB_BYTES, msg, sizeof(msg)) != 0,
                  name + ": menu glyph hook " + std::to_string(m) + " installs alongside Exit Game");
        }

        uint8_t* exitStubs = stubBase + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES +
                             SOS_PROMPT_DIALOG_STUB_BYTES + SOS_PROMPT_MENUS * SOS_PROMPT_MENU_STUB_BYTES;
        Check(sos_exit_game_install(buf.data(), original.size(), buildKey, exitStubs, SOS_EXIT_GAME_STUB_BYTES, msg, sizeof(msg)) == 1,
              name + ": Exit Game installs cleanly alongside all other hooks");

        // Verify each patched method starts with `jmp rel32` to its exact stub slot
        const uint32_t methods[4] = {site->toggle_method, site->confirm_method, site->quit_method, site->text_method};
        for (int i = 0; i < 4; ++i) {
            ZydisDecodedInstruction insn;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            const uint8_t* entry = buf.data() + methods[i];
            const uint8_t* expectedStub = exitStubs + static_cast<size_t>(i) * SOS_EXIT_GAME_STUB_SLOT;
            const bool decoded = DecodeFirst(entry, 16, insn, ops);
            ZyanU64 target = 0;
            const bool resolved = decoded && insn.mnemonic == ZYDIS_MNEMONIC_JMP &&
                                  ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn, &ops[0], reinterpret_cast<ZyanU64>(entry), &target));
            Check(resolved && target == reinterpret_cast<ZyanU64>(expectedStub),
                  name + ": hook " + std::to_string(i) + " jumps directly to its stub");
        }
    }
}

}  // namespace

int main() {
    TestTables();
    TestTranslations();
    TestHeadlessRuntime();
    const char* fast = std::getenv("SOS_TEST_FAST");
    if (!fast || fast[0] != '1') {
        for (const char* key : kBuilds) TestRealBootImage(key);
    }
    if (g_failures) {
        std::printf("Exit Game tests FAILED: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("Exit Game tests passed: CUSA57767 1.00 and CUSA57767 1.01, %d checks\n", g_checks);
    return 0;
}
