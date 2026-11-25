// Menu glyph Images without a controller follower: the sprite-to-input mapping, the handlers on fake Images in every prompt style
// (controller changes, two pads, late icons, sprite changes, disabled Images) and, when the boot images are installed, the hooks on
// the real code of CUSA57767 1.00 and 1.01. No game, launcher or window is started.
#include <Zydis/Zydis.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "build_images.h"
#include "controller_prompts.h"
#include "image_glyphs.h"
#include "prompt_dialog.h"
#include "sos_exit_game.h"
#include "sos_hermes_unlock.h"
#include "sos_pit_unlock.h"
#include "sos_skip_intro.h"

extern "C" {
#include "redzone_analysis.h"
}

namespace fs = std::filesystem;

namespace {

using Image = std::vector<uint8_t>;
constexpr int kGamepad = 2, kKeyboard = 0;
constexpr size_t kSlots = 20;
const char* const kBuilds[] = {"CUSA57767-1.00", "CUSA57767-1.01"};

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

// ---- the mapping -----------------------------------------------------------------------------------------------------------

SosGlyph Glyph(const std::string& name) {
    const std::u16string wide(name.begin(), name.end());
    return sos_image_glyph_of_name(reinterpret_cast<const uint16_t*>(wide.data()), static_cast<int32_t>(wide.size()));
}

void TestMapping() {
    const std::map<std::string, SosGlyph> expected = {
        {"KS_ButtonPrompt_0", SOS_GLYPH_WEST}, {"KS_ButtonPrompt_1", SOS_GLYPH_EAST}, {"KS_ButtonPrompt_2", SOS_GLYPH_SOUTH},
        {"KS_ButtonPrompt_3", SOS_GLYPH_NORTH}, {"KS_ButtonPrompt_4", SOS_GLYPH_START}, {"KS_ButtonPrompt_5", SOS_GLYPH_SELECT},
        {"KS_ButtonPrompt_11", SOS_GLYPH_LEFT_STICK_PRESS}, {"KS_ButtonPrompt_12", SOS_GLYPH_LEFT_TRIGGER},
        {"KS_ButtonPrompt_13", SOS_GLYPH_RIGHT_TRIGGER}, {"KS_ButtonPrompt_14", SOS_GLYPH_LEFT_SHOULDER},
        {"KS_ButtonPrompt_15", SOS_GLYPH_RIGHT_SHOULDER}, {"KS_ButtonPrompt_18", SOS_GLYPH_RIGHT_STICK_PRESS},
        {"KS_ButtonPrompt_19", SOS_GLYPH_TOUCHPAD}, {"PS_ButtonPrompts_0", SOS_GLYPH_EAST}, {"PS_ButtonPrompts_1", SOS_GLYPH_SOUTH},
        {"PS_ButtonPrompts_2", SOS_GLYPH_NORTH}, {"PS_ButtonPrompts_3", SOS_GLYPH_WEST}, {"PS_ButtonPrompts_4", SOS_GLYPH_SELECT},
        {"PS_ButtonPrompts_5", SOS_GLYPH_START}, {"PS_ButtonPrompts_11", SOS_GLYPH_LEFT_TRIGGER}, {"PS_ButtonPrompts_12", SOS_GLYPH_RIGHT_TRIGGER},
        {"PS_ButtonPrompts_13", SOS_GLYPH_LEFT_SHOULDER}, {"PS_ButtonPrompts_14", SOS_GLYPH_RIGHT_SHOULDER},
        {"PS_ButtonPrompts_17", SOS_GLYPH_RIGHT_STICK_PRESS}, {"PS_ButtonPrompts_18", SOS_GLYPH_LEFT_STICK_PRESS},
        {"PS_ButtonPrompts_19", SOS_GLYPH_TOUCHPAD}, {"PS4_ButtonPrompts_11", SOS_GLYPH_LEFT_TRIGGER}, {"PS4_ButtonPrompts_13", SOS_GLYPH_LEFT_SHOULDER},
        {"PS4_ButtonPrompts_17", SOS_GLYPH_RIGHT_STICK_PRESS}, {"ButtonPromptSheetTemp(256,64)_0", SOS_GLYPH_WEST},
        {"ButtonPromptSheetTemp(256,64)_1", SOS_GLYPH_EAST}, {"ButtonPromptSheetTemp(256,64)_2", SOS_GLYPH_SOUTH},
        {"ButtonPromptSheetTemp(256,64)_3", SOS_GLYPH_NORTH}};
    for (const auto& [name, glyph] : expected) Check(Glyph(name) == glyph, name + " is the expected input");
    for (const char* name : {"KS_ButtonPrompt_6", "KS_ButtonPrompt_10", "KS_ButtonPrompt_16", "KS_ButtonPrompt_17", "KS_ButtonPrompt_20", "KS_ButtonPrompt_012",
                             "KS_ButtonPrompt_02", "KS_ButtonPrompt_", "KS_ButtonPrompt", "KS_ButtonPrompt_1x", "KS_ButtonPrompt_x", "xKS_ButtonPrompt_12",
                             "PS_ButtonPrompts_8", "PS_ButtonPrompts_15", "PS4_ButtonPrompts_16", "PS4_ButtonPrompts_32", "PS4_ButtonPrompts_99",
                             "ButtonPromptSheetTemp(256,64)_4", "ABXY_Controller_Sheet(384,256)_12", "PS5_PromptSheet_Icon(512,256)_21", "PS_LeftStick",
                             "DS5_L2_MASK", "ds4_controller_ButtonMasks_0", "Button_General(396,84)_0", "PS Logo", ""})
        Check(Glyph(name) == SOS_GLYPH_NONE, std::string("'") + name + "' is not a mapped glyph");
    Check(sos_image_glyph_of_name(nullptr, 20) == SOS_GLYPH_NONE, "no name is not a glyph");
}

// ---- fake game -------------------------------------------------------------------------------------------------------------

struct Object {
    alignas(16) std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);
    void* ptr() { return bytes.data(); }
    void Set(size_t at, const void* value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
    void SetInt(size_t at, int32_t value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
    void* Get(size_t at) const {
        void* value;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    }
};

struct World {
    std::map<void*, int32_t> ids;
    std::map<void*, void*> names;
    std::set<void*> inactive;
    void* overseer = nullptr;
    void* overseerMethod = nullptr;
    uint32_t override_offset = 0;
    int nameCalls = 0, overseerCalls = 0, setCalls = 0, bad = 0;
};
World* g_world = nullptr;

SOS_IMAGE_GLYPHS_SYSV int32_t FakeId(void* object, const void* method) {
    if (method != nullptr || !g_world->ids.count(object)) ++g_world->bad;
    return g_world->ids[object];
}
SOS_IMAGE_GLYPHS_SYSV void* FakeName(void* object, const void* method) {
    ++g_world->nameCalls;
    if (method != nullptr) ++g_world->bad;
    return g_world->names.count(object) ? g_world->names[object] : nullptr;
}
SOS_IMAGE_GLYPHS_SYSV uint8_t FakeActive(void* behaviour, const void* method) {
    if (method != nullptr) ++g_world->bad;
    return g_world->inactive.count(behaviour) ? 0 : 1;
}
SOS_IMAGE_GLYPHS_SYSV void FakeSetOverride(void* image, void* sprite, const void* method) {
    ++g_world->setCalls;
    if (method != nullptr) ++g_world->bad;
    std::memcpy(static_cast<uint8_t*>(image) + g_world->override_offset, &sprite, sizeof(sprite));
}
SOS_IMAGE_GLYPHS_SYSV void* FakeOverseer(const void* method) {
    ++g_world->overseerCalls;
    if (method != g_world->overseerMethod) ++g_world->bad;
    return g_world->overseer;
}

struct Scene {
    std::deque<Object> objects;
    World world;
    SosImageGlyphSite site{};
    Object *overseer = nullptr, *icons = nullptr, *runtime = nullptr, *statics = nullptr, *instance = nullptr, *method = nullptr;
    Object* xbox[kSlots] = {};
    uintptr_t overseerSlot = 0, runtimeSlot = 0;
    int32_t nextId = 100;

    Object* New() {
        objects.emplace_back();
        objects.back().Set(0x10, objects.back().ptr());
        return &objects.back();
    }
    Object* Sprite(const std::string& name) {
        Object* sprite = New();
        Object* text = New();
        text->SetInt(0x10, static_cast<int32_t>(name.size()));
        for (size_t i = 0; i < name.size(); ++i) text->bytes[0x14 + 2 * i] = static_cast<uint8_t>(name[i]), text->bytes[0x15 + 2 * i] = 0;
        world.ids[sprite->ptr()] = nextId++;
        world.names[sprite->ptr()] = text->ptr();
        return sprite;
    }
    Object* ImageOf(Object* sprite) {
        Object* image = New();
        image->Set(site.sprite, sprite ? sprite->ptr() : nullptr);
        return image;
    }
    Object* ImageOf(const std::string& name) { return ImageOf(Sprite(name)); }

    Scene(const char* key, int style) {
        site = *sos_image_glyphs_site(key);
        world.override_offset = site.override_sprite;
        method = New();
        overseer = New();
        icons = New();
        runtime = New();
        statics = New();
        instance = New();
        instance->Set(0x20, New()->ptr());
        statics->Set(0, instance->ptr());
        runtime->Set(0xb8, statics->ptr());
        overseer->Set(site.icon_data, icons->ptr());
        overseer->SetInt(site.input_device, kGamepad);
        for (size_t i = 0; i < kSlots; ++i) {
            xbox[i] = Sprite("ABXY_Controller_Sheet(384,256)_" + std::to_string(i));
            icons->Set(site.xbox_icons + 8 * i, xbox[i]->ptr());
        }
        world.overseer = overseer->ptr();
        world.overseerMethod = method->ptr();
        overseerSlot = reinterpret_cast<uintptr_t>(method->ptr());
        runtimeSlot = reinterpret_cast<uintptr_t>(runtime->ptr());
        Arm(style);
    }
    void Arm(int style) {
        SosImageGlyphGame game{};
        game.get_instance_id = FakeId;
        game.get_name = FakeName;
        game.is_active = FakeActive;
        game.set_override_sprite = FakeSetOverride;
        game.get_overseer = FakeOverseer;
        game.overseer_slot = &overseerSlot;
        game.runtime_slot = &runtimeSlot;
        g_world = &world;
        sos_image_glyphs_arm_with(style, &site, &game);
    }
    void Enable(Object* image) { sos_image_glyphs_on_enable(image->ptr()); }
    void Disable(Object* image) { sos_image_glyphs_on_disable(image->ptr()); }
    void SetSprite(Object* image, Object* sprite) {
        sos_image_glyphs_on_set_sprite(image->ptr(), sprite ? sprite->ptr() : nullptr);
        image->Set(site.sprite, sprite ? sprite->ptr() : nullptr);
    }
    void* Override(Object* image) const { return image->Get(site.override_sprite); }
    bool Shows(Object* image, int slot) const { return slot < 0 ? Override(image) == nullptr : Override(image) == xbox[slot]->ptr(); }
};

const std::pair<const char*, int> kPrompts[] = {
    {"KS_ButtonPrompt_12", 6}, {"KS_ButtonPrompt_13", 7}, {"KS_ButtonPrompt_14", 8}, {"KS_ButtonPrompt_15", 9}, {"KS_ButtonPrompt_11", 17},
    {"KS_ButtonPrompt_18", 18}, {"KS_ButtonPrompt_2", 0}, {"KS_ButtonPrompt_1", 2}, {"KS_ButtonPrompt_0", 3}, {"KS_ButtonPrompt_3", 1},
    {"KS_ButtonPrompt_4", 4}, {"KS_ButtonPrompt_5", 5}, {"KS_ButtonPrompt_19", 19}, {"PS_ButtonPrompts_11", 6}, {"PS4_ButtonPrompts_14", 9},
    {"ButtonPromptSheetTemp(256,64)_2", 0}};

void TestStyles(const char* key) {
    const std::string name = std::string(key) + ": ";
    const struct {
        const char* label;
        int style, pad;
        bool xbox;
    } modes[] = {{"forced Xbox without a controller", SOS_PROMPTS_XBOX, SOS_PAD_NONE, true},
                 {"forced Xbox with a DualShock 4", SOS_PROMPTS_XBOX, SOS_PAD_PLAYSTATION4, true},
                 {"Automatic with an Xbox controller", SOS_PROMPTS_AUTOMATIC, SOS_PAD_XBOX, true},
                 {"Automatic with a DualShock 4", SOS_PROMPTS_AUTOMATIC, SOS_PAD_PLAYSTATION4, false},
                 {"Automatic with a DualSense", SOS_PROMPTS_AUTOMATIC, SOS_PAD_PLAYSTATION5, false},
                 {"Automatic with another controller", SOS_PROMPTS_AUTOMATIC, SOS_PAD_OTHER, false},
                 {"Automatic without a controller", SOS_PROMPTS_AUTOMATIC, SOS_PAD_NONE, false},
                 {"forced PlayStation with an Xbox controller", SOS_PROMPTS_PLAYSTATION, SOS_PAD_XBOX, false}};
    for (const auto& mode : modes) {
        sos_pad_kind_publish(mode.pad);
        Scene s(key, mode.style);
        bool right = true, kept = true;
        for (const auto& [sprite, slot] : kPrompts) {
            Object* image = s.ImageOf(sprite);
            void* before = image->Get(s.site.sprite);
            s.Enable(image);
            right &= s.Shows(image, mode.xbox ? slot : -1);
            kept &= image->Get(s.site.sprite) == before;
        }
        Check(right, name + mode.label + (mode.xbox ? ": every glyph shows its Xbox input" : ": every glyph keeps its PlayStation sprite"));
        Check(kept && s.world.bad == 0, name + mode.label + ": the Images' own sprites are never written");
        Object* other = s.ImageOf("Button_General(396,84)_0");
        Object* dpad = s.ImageOf("KS_ButtonPrompt_9");
        Object* empty = s.ImageOf(static_cast<Object*>(nullptr));
        for (Object* image : {other, dpad, empty}) s.Enable(image);
        Check(s.Shows(other, -1) && s.Shows(dpad, -1) && s.Shows(empty, -1), name + mode.label + ": other sprites, the D-pad and empty Images are left alone");
        if (!mode.xbox) Check(s.world.setCalls == 0, name + mode.label + ": nothing is written at all");
        if (mode.style == SOS_PROMPTS_PLAYSTATION) Check(s.world.nameCalls == 0, name + mode.label + ": the handlers do nothing");
    }
}

void TestLifetime(const char* key) {
    const std::string name = std::string(key) + ": ";
    {
        sos_pad_kind_publish(SOS_PAD_XBOX);
        Scene s(key, SOS_PROMPTS_AUTOMATIC);
        Object *left = s.ImageOf("KS_ButtonPrompt_12"), *right = s.ImageOf("KS_ButtonPrompt_13"), *trigger = s.ImageOf("KS_ButtonPrompt_2");
        s.Enable(left);
        s.Enable(right);
        sos_pad_kind_publish(SOS_PAD_PLAYSTATION5);
        s.Enable(trigger);
        Check(s.Shows(left, -1) && s.Shows(right, -1) && s.Shows(trigger, -1), name + "switching to a DualSense clears the open menu at the next Image event");
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.SetSprite(s.ImageOf("Button_General(396,84)_0"), s.Sprite("Button_General(396,84)_1"));
        Check(s.Shows(left, 6) && s.Shows(right, 7) && s.Shows(trigger, 0), name + "switching back shows LT, RT and A again without reopening the menu");
        sos_pad_kind_publish(SOS_PAD_NONE);
        s.Disable(left);
        s.Enable(left);
        Check(s.Shows(left, -1) && s.Shows(right, -1), name + "disconnecting the Xbox controller brings the PlayStation sprites back");
    }
    {
        sos_pad_kind_publish(SOS_PAD_XBOX);
        Scene s(key, SOS_PROMPTS_AUTOMATIC);
        Object* image = s.ImageOf("KS_ButtonPrompt_12");
        s.Enable(image);
        const int calls = s.world.setCalls;
        s.Enable(image);
        s.Enable(image);
        Check(s.Shows(image, 6) && s.world.setCalls == calls, name + "enabling twice writes the override once");
        s.Disable(image);
        Check(s.Shows(image, -1) && s.world.setCalls == calls, name + "disabling drops the override without marking the Image dirty");
        sos_pad_kind_publish(SOS_PAD_PLAYSTATION4);
        std::fill(image->bytes.begin(), image->bytes.end(), 0);
        const auto poison = image->bytes;
        s.Enable(s.ImageOf("KS_ButtonPrompt_13"));
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Enable(s.ImageOf("KS_ButtonPrompt_14"));
        Check(image->bytes == poison, name + "a disabled Image is never touched again, so it may be destroyed");
    }
    {
        sos_pad_kind_publish(SOS_PAD_XBOX);
        Scene s(key, SOS_PROMPTS_AUTOMATIC);
        Object* follower = s.ImageOf("KS_ButtonPrompt_2");
        s.Enable(follower);
        Check(s.Shows(follower, 0), name + "an Image that AutoButtonSprite will fill shows the mapped glyph first");
        s.SetSprite(follower, s.xbox[2]);
        Check(s.Shows(follower, -1), name + "the sprite AutoButtonSprite sets is not covered by the first glyph");
        s.SetSprite(follower, s.Sprite("KS_ButtonPrompt_13"));
        Check(s.Shows(follower, 7), name + "a PlayStation glyph set later becomes the matching Xbox one");
        s.SetSprite(follower, s.Sprite("PS4_ButtonPrompts_13"));
        Check(s.Shows(follower, 8), name + "and changes with the sprite (LB, not LT)");
        s.SetSprite(follower, nullptr);
        Check(s.Shows(follower, -1), name + "clearing the sprite clears the glyph");
        Object* hidden = s.ImageOf("Button_General(396,84)_0");
        s.world.inactive.insert(hidden->ptr());
        s.SetSprite(hidden, s.Sprite("KS_ButtonPrompt_12"));
        Check(s.Shows(hidden, -1), name + "an inactive Image is not tracked");
        s.world.inactive.clear();
        s.Enable(hidden);
        Check(s.Shows(hidden, 6), name + "and shows its glyph when it is enabled");
    }
    {
        sos_pad_kind_publish(SOS_PAD_XBOX);
        Scene s(key, SOS_PROMPTS_AUTOMATIC);
        Object* image = s.ImageOf("KS_ButtonPrompt_2");
        Object* foreign = s.Sprite("foreign");
        image->Set(s.site.override_sprite, foreign->ptr());
        s.Enable(image);
        Check(s.Override(image) == foreign->ptr(), name + "an override set by the game is kept");
        Object* later = s.ImageOf("KS_ButtonPrompt_2");
        s.Enable(later);
        later->Set(s.site.override_sprite, foreign->ptr());
        sos_pad_kind_publish(SOS_PAD_PLAYSTATION4);
        s.Enable(s.ImageOf("KS_ButtonPrompt_1"));
        s.Disable(later);
        Check(s.Override(later) == foreign->ptr(), name + "also when the game sets it after the glyph");
    }
    {
        sos_pad_kind_publish(SOS_PAD_XBOX);
        Scene s(key, SOS_PROMPTS_AUTOMATIC);
        Object* sprite = s.Sprite("Button_General(396,84)_0");
        for (int i = 0; i < 300; ++i) s.Enable(s.ImageOf(sprite));
        Check(s.world.nameCalls == 1 && s.world.overseerCalls == 0, name + "a sprite's name is read once and other Images cost no icon lookup");
        Object* reused = s.Sprite("KS_ButtonPrompt_12");
        Object* one = s.ImageOf(reused);
        s.Enable(one);
        s.Disable(one);
        s.world.ids[reused->ptr()] = 90000;
        s.world.names[reused->ptr()] = s.world.names[sprite->ptr()];
        Object* two = s.ImageOf(reused);
        s.Enable(two);
        Check(s.Shows(one, -1) && s.Shows(two, -1), name + "another sprite at a reused address is not taken for the glyph");
        for (int i = 0; i < 700; ++i) s.Enable(s.ImageOf("KS_ButtonPrompt_12"));
        Check(s.world.bad == 0, name + "more glyphs than the table holds are left alone");
    }
}

void TestLateIcons(const char* key) {
    const std::string name = std::string(key) + ": ";
    sos_pad_kind_publish(SOS_PAD_XBOX);
    const auto late = [&](const std::string& label, auto&& damage, auto&& repair) {
        Scene s(key, SOS_PROMPTS_XBOX);
        damage(s);
        Object* image = s.ImageOf("KS_ButtonPrompt_12");
        s.Enable(image);
        Check(s.Shows(image, -1) && s.world.setCalls == 0, name + label + ": the PlayStation sprite stays");
        repair(s);
        Object* plain = s.ImageOf("Button_General(396,84)_0");
        for (int i = 0; i < 300; ++i) s.Enable(plain);
        Check(s.Shows(image, 6), name + label + ": the open menu follows once it is there");
    };
    late("GameRuntime not created yet", [](Scene& s) { s.statics->Set(0, nullptr); }, [](Scene& s) { s.statics->Set(0, s.instance->ptr()); });
    late("GameRuntime class not initialised", [](Scene& s) { s.runtime->Set(0xb8, nullptr); }, [](Scene& s) { s.runtime->Set(0xb8, s.statics->ptr()); });
    late("GameRuntime class not resolved", [](Scene& s) { s.runtimeSlot |= 1; }, [](Scene& s) { s.runtimeSlot &= ~uintptr_t{1}; });
    late("subsystem lookup not resolved", [](Scene& s) { s.overseerSlot |= 1; }, [](Scene& s) { s.overseerSlot &= ~uintptr_t{1}; });
    late("no InputOverseer", [](Scene& s) { s.world.overseer = nullptr; }, [](Scene& s) { s.world.overseer = s.overseer->ptr(); });
    late("no icon data", [](Scene& s) { s.overseer->Set(s.site.icon_data, nullptr); }, [](Scene& s) { s.overseer->Set(s.site.icon_data, s.icons->ptr()); });
    late("keyboard input", [](Scene& s) { s.overseer->SetInt(s.site.input_device, kKeyboard); }, [](Scene& s) { s.overseer->SetInt(s.site.input_device, kGamepad); });
    {
        Scene s(key, SOS_PROMPTS_XBOX);
        s.statics->Set(0, nullptr);
        for (int i = 0; i < 50; ++i) s.Enable(s.ImageOf("KS_ButtonPrompt_12"));
        Check(s.world.overseerCalls == 0, name + "the throwing lookup is never called before GameRuntime exists");
    }
    {
        Scene s(key, SOS_PROMPTS_XBOX);
        s.icons->Set(s.site.xbox_icons + 8 * 6, nullptr);
        s.xbox[7]->Set(0x10, nullptr);
        Object *left = s.ImageOf("KS_ButtonPrompt_12"), *right = s.ImageOf("KS_ButtonPrompt_13"), *a = s.ImageOf("KS_ButtonPrompt_2");
        for (Object* image : {left, right, a}) s.Enable(image);
        Check(s.Shows(left, -1) && s.Shows(right, -1) && s.Shows(a, 0), name + "a missing or destroyed Xbox sprite keeps that PlayStation glyph, never a blank one");
        sos_image_glyphs_on_enable(nullptr);
        sos_image_glyphs_on_disable(nullptr);
        sos_image_glyphs_on_set_sprite(nullptr, nullptr);
        Object* destroyed = s.ImageOf("KS_ButtonPrompt_2");
        destroyed->Set(0x10, nullptr);
        s.Enable(destroyed);
        Check(s.Shows(destroyed, -1), name + "null and destroyed Images are ignored");
    }
}

// One InputOverseer serves both players: the published pad kind is the first controller's, and using the other pad stores PS4 again.
void TestTwoPads(const char* key) {
    const std::string name = std::string(key) + ": ";
    Object overseer;
    const auto type = [&] {
        int32_t value;
        std::memcpy(&value, overseer.bytes.data() + 0x34, sizeof(value));
        return value;
    };
    sos_pad_kind_publish(SOS_PAD_XBOX);
    sos_prompt_hook_arm(SOS_PROMPTS_AUTOMATIC, key);
    overseer.SetInt(0x30, kGamepad);
    for (int change = 0; change < 4; ++change) {
        overseer.SetInt(0x34, 3);
        sos_prompt_hook_handler(overseer.bytes.data());
        Check(type() == 2, name + "two Xbox pads: the text atlas is the Xbox one after the other pad is used");
    }
    sos_pad_kind_publish(SOS_PAD_PLAYSTATION4);
    sos_prompt_hook_handler(overseer.bytes.data());
    Check(type() == 3, name + "a PlayStation first pad gets the game's own atlas back");
}

// ---- the real code ---------------------------------------------------------------------------------------------------------

bool ReadBootImage(const fs::path& boot, Image& image, uint64_t extra, std::vector<uint64_t>& triples) {
    std::ifstream in(boot, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    uint64_t header[5] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    const uint64_t size = header[0], segments = header[2], relocs = header[3], imports = header[4];
    in.seekg(48, std::ios::cur);
    uint64_t modules = 0, bindings = 0;
    in.read(reinterpret_cast<char*>(&modules), 8);
    in.seekg(static_cast<std::streamoff>(modules * 56), std::ios::cur);
    in.read(reinterpret_cast<char*>(&bindings), 8);
    in.seekg(static_cast<std::streamoff>(bindings * 24), std::ios::cur);
    triples.assign(segments * 3, 0);
    in.read(reinterpret_cast<char*>(triples.data()), static_cast<std::streamsize>(triples.size() * 8));
    in.seekg(static_cast<std::streamoff>(imports * 128 + relocs * 32), std::ios::cur);
    image.assign(size + extra, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size)));
}

std::set<uint64_t> Changed(const Image& a, const Image& b, uint64_t size) {
    std::set<uint64_t> changed;
    for (uint64_t i = 0; i < size; ++i)
        if (a[i] != b[i]) changed.insert(i);
    return changed;
}

void TestHookCode(const std::string& name, const Image& image, const SosGlyphHook& hook) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    const uint8_t* method = image.data() + hook.method;
    size_t at = 0;
    bool whole = true, inside = false;
    while (at < hook.length) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, method + at, hook.length - at, &in, operands))) break;
        if (at < SOS_GLYPH_HOOK_STOLEN) {
            whole &= at + in.length <= SOS_GLYPH_HOOK_STOLEN && !(in.attributes & ZYDIS_ATTRIB_IS_RELATIVE);
            whole &= in.meta.category == ZYDIS_CATEGORY_PUSH || in.mnemonic == ZYDIS_MNEMONIC_MOV;
        }
        for (int k = 0; k < in.operand_count_visible; ++k)
            if (operands[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[k].imm.is_relative) {
                const int64_t target = static_cast<int64_t>(at) + in.length + operands[k].imm.value.s;
                inside |= target > 0 && target < SOS_GLYPH_HOOK_STOLEN;
            }
        at += in.length;
        if (in.meta.category == ZYDIS_CATEGORY_RET && at + 16 >= hook.length) break;
    }
    Check(whole, name + ": the six stolen bytes are whole pushes and register moves");
    Check(!inside, name + ": no branch of the method lands inside the stolen bytes");
}

void TestStub(const std::string& name, const Image& image, const Image& original, const SosGlyphHook& hook, const uint8_t* stub, const void* handler) {
    const uint8_t* method = image.data() + hook.method;
    int32_t into = 0, back = 0;
    std::memcpy(&into, method + 1, 4);
    Check(method[0] == 0xe9 && method + 5 + into == stub && method[5] == 0x90, name + ": the method jumps to its stub");
    uint64_t target = 0;
    std::memcpy(&target, stub + 14, 8);
    Check(target == reinterpret_cast<uint64_t>(handler), name + ": the stub calls its own handler");
    Check(!std::memcmp(stub + 36, original.data() + hook.method, SOS_GLYPH_HOOK_STOLEN), name + ": the stub replays the stolen bytes");
    std::memcpy(&back, stub + 43, 4);
    Check(stub[42] == 0xe9 && stub + 47 + back == method + SOS_GLYPH_HOOK_STOLEN, name + ": the stub returns behind them");
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    int depth = 0, atCall = -1;
    for (size_t at = 0; at < 36;) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, stub + at, 47 - at, &in, operands))) break;
        if (in.mnemonic == ZYDIS_MNEMONIC_PUSH) depth += 8;
        if (in.mnemonic == ZYDIS_MNEMONIC_POP) depth -= 8;
        if (in.mnemonic == ZYDIS_MNEMONIC_SUB) depth += 8;
        if (in.mnemonic == ZYDIS_MNEMONIC_ADD) depth -= 8;
        if (in.mnemonic == ZYDIS_MNEMONIC_CALL) atCall = depth;
        at += in.length;
    }
    Check(depth == 0 && atCall % 16 == 8, name + ": the stub keeps the stack balanced and aligned at the call");
}

// ---- the contract of a build, without its game code -------------------------------------------------------------------------

uint64_t Fnv(uint64_t hash, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

struct Synthetic {
    SosImageGlyphSite site;
    Image image;
    uint64_t size = 0;
};

// The hooked methods and the functions the handlers call of `real`, with its lengths and first bytes, at offsets packed into a small image;
// the hashes are those of the bytes laid, which are not the game's.
Synthetic MakeSynthetic(const SosImageGlyphSite& real) {
    Synthetic s{real, {}, 0};
    uint64_t at = 0x1000;
    uint64_t seed = 0x2545f4914f6cdd1dull;
    const auto place = [&](uint32_t length) {
        const uint64_t begin = at;
        at += (length + 31) & ~uint64_t{15};
        return begin;
    };
    for (SosGlyphHook& hook : s.site.hooks) hook.method = static_cast<uint32_t>(place(hook.length));
    for (SosGlyphRange& range : s.site.services) {
        const uint32_t length = range.end - range.begin;
        range.begin = static_cast<uint32_t>(place(length));
        range.end = range.begin + length;
    }
    s.size = at + 0x1000;
    s.image.assign(s.size + SOS_IMAGE_GLYPHS_STUB_BYTES, 0);
    const auto fill = [&](uint64_t begin, uint64_t end) {
        for (uint64_t i = begin; i < end; ++i) {
            seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
            s.image[i] = static_cast<uint8_t>(seed);
        }
    };
    for (SosGlyphHook& hook : s.site.hooks) {
        fill(hook.method, hook.method + hook.length);
        std::memcpy(&s.image[hook.method], hook.entry, SOS_GLYPH_HOOK_STOLEN);
        hook.hash = Fnv(UINT64_C(0xcbf29ce484222325), &s.image[hook.method], hook.length);
    }
    uint64_t services = UINT64_C(0xcbf29ce484222325);
    for (const SosGlyphRange& range : s.site.services) {
        fill(range.begin, range.end);
        services = Fnv(services, &s.image[range.begin], range.end - range.begin);
    }
    s.site.services_hash = services;
    return s;
}

void TestContract(const char* key) {
    const std::string name = std::string(key) + " contract";
    const SosImageGlyphSite* table = sos_image_glyphs_site(key);
    Check(table != nullptr, name + ": the build has a site");
    if (!table) return;
    const SosPitUnlockSite* pit = sos_pit_unlock_site(key);

    std::vector<std::pair<uint64_t, uint64_t>> spans;
    for (const SosGlyphHook& hook : table->hooks) {
        Check(hook.length >= SOS_GLYPH_HOOK_STOLEN && hook.entry[0] == 0x55, name + ": a hooked method holds its stolen bytes and starts with push rbp");
        spans.push_back({hook.method, uint64_t{hook.method} + hook.length});
    }
    for (const SosGlyphRange& range : table->services) {
        Check(range.begin < range.end, name + ": a called function has a range");
        spans.push_back({range.begin, range.end});
    }
    std::sort(spans.begin(), spans.end());
    bool disjoint = true, inside = pit != nullptr;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (i && spans[i].first < spans[i - 1].second) disjoint = false;
        if (pit && spans[i].second > pit->image_size) inside = false;
    }
    Check(disjoint, name + ": no hooked method or called function overlaps another");
    Check(inside, name + ": every hooked method and called function lies inside the image");

    Synthetic synthetic = MakeSynthetic(*table);
    const SosImageGlyphSite& site = synthetic.site;
    const Image original = synthetic.image;
    const uint64_t size = synthetic.size;
    const void* const handlers[] = {reinterpret_cast<const void*>(sos_image_glyphs_on_enable), reinterpret_cast<const void*>(sos_image_glyphs_on_disable),
                                    reinterpret_cast<const void*>(sos_image_glyphs_on_set_sprite), reinterpret_cast<const void*>(sos_prompt_hook_handler)};
    const char* why = "";
    char message[240];
    for (const int style : {SOS_PROMPTS_XBOX, SOS_PROMPTS_AUTOMATIC})
        Check(sos_image_glyphs_verify_site(original.data(), size, &site, style, &why), name + ": the laid hooks verify");
    Check(!sos_image_glyphs_verify_site(original.data(), size, &site, SOS_PROMPTS_PLAYSTATION, &why), name + ": PlayStation prompts have no hooks");
    Check(!sos_image_glyphs_verify_site(original.data(), size, table, SOS_PROMPTS_AUTOMATIC, &why), name + ": the game's fingerprints do not match other bytes");

    const auto refuses = [&](Image copy, uint64_t copy_size, const SosImageGlyphSite* with, int style, size_t capacity, uint64_t stub_at, const std::string& what) {
        const Image before = copy;
        Check(sos_image_glyphs_install_site(copy.data(), copy_size, with, style, copy.data() + stub_at, capacity, message, sizeof(message)) == 0 && copy == before,
              name + ": " + what + " writes nothing");
    };
    refuses(original, size, nullptr, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "an unknown build");
    refuses(original, size, &site, SOS_PROMPTS_PLAYSTATION, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "the PlayStation style");
    refuses(original, size, &site, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES - 1, size, "a small stub area");
    refuses(original, site.hooks[0].method + 4, &site, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "a truncated image");
    {
        Image copy = original;
        Check(sos_image_glyphs_install_site(copy.data(), size, &site, SOS_PROMPTS_AUTOMATIC, copy.data() + size + (uint64_t{1} << 32), SOS_IMAGE_GLYPHS_STUB_BYTES,
                                            message, sizeof(message)) == 0 && copy == original, name + ": stubs out of reach write nothing");
    }
    for (const SosGlyphHook& hook : site.hooks)
        for (const uint32_t at : {0u, 5u, 6u, hook.length / 2, hook.length - 1}) {
            Image bad = original;
            bad[hook.method + at] ^= 0x01;
            refuses(bad, size, &site, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "one changed hooked method (no partial install)");
        }
    for (const SosGlyphRange& range : site.services) {
        Image bad = original;
        bad[range.begin + 8] ^= 0x01;
        refuses(bad, size, &site, SOS_PROMPTS_XBOX, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "a changed function the handlers call");
    }
    for (int i = 0; i < SOS_GLYPH_HOOKS; ++i) {
        SosImageGlyphSite bad = site;
        bad.hooks[i].hash ^= 1;
        refuses(original, size, &bad, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "a hook fingerprint that differs");
        bad = site;
        bad.hooks[i].entry[2] ^= 1;
        refuses(original, size, &bad, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "first bytes that differ");
    }
    {
        SosImageGlyphSite bad = site;
        bad.services_hash ^= 1;
        refuses(original, size, &bad, SOS_PROMPTS_XBOX, SOS_IMAGE_GLYPHS_STUB_BYTES, size, "a called-function fingerprint that differs");
    }

    for (const int style : {SOS_PROMPTS_XBOX, SOS_PROMPTS_AUTOMATIC}) {
        const bool automatic = style == SOS_PROMPTS_AUTOMATIC;
        const std::string label = name + (automatic ? " Automatic" : " Xbox");
        const int hooks = automatic ? SOS_GLYPH_HOOKS : SOS_GLYPH_HOOK_TEXT_ATLAS;
        Image image = original;
        uint8_t* stubs = image.data() + size;
        Check(sos_image_glyphs_install_site(image.data(), size, &site, style, stubs, SOS_IMAGE_GLYPHS_STUB_BYTES, message, sizeof(message)) == 1, label + ": the hooks install");
        std::set<uint64_t> allowed;
        for (int i = 0; i < hooks; ++i) {
            for (int k = 0; k < SOS_GLYPH_HOOK_STOLEN; ++k) allowed.insert(site.hooks[i].method + k);
            TestStub(label + " hook " + std::to_string(i), image, original, site.hooks[i], stubs + i * SOS_GLYPH_STUB_SLOT, handlers[i]);
        }
        const std::set<uint64_t> changed = Changed(original, image, size);
        Check(std::includes(allowed.begin(), allowed.end(), changed.begin(), changed.end()) && !changed.empty(), label + ": only the hooked entries change");
        const Image once = image;
        Check(sos_image_glyphs_install_site(image.data(), size, &site, style, stubs, SOS_IMAGE_GLYPHS_STUB_BYTES, message, sizeof(message)) == 0 && image == once,
              label + ": a second install writes nothing");
    }
    sos_test::ReportContract(key, "the hooks and called functions of its site table on a small image: every refusal, each style, stubs, second install");
}

void TestReal(const char* key, const fs::path& boot) {
    const std::string name = key;
    constexpr uint64_t kOther = SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES + SOS_PROMPT_DIALOG_STUB_BYTES +
                                SOS_PROMPT_MENUS * SOS_PROMPT_MENU_STUB_BYTES + SOS_EXIT_GAME_STUB_BYTES;
    constexpr uint64_t kRoom = kOther + SOS_IMAGE_GLYPHS_STUB_BYTES;
    Image original;
    std::vector<uint64_t> triples;
    if (!ReadBootImage(boot, original, kRoom, triples)) {
        Check(false, name + ": the real image is readable");
        return;
    }
    const uint64_t size = original.size() - kRoom;
    const SosImageGlyphSite& site = *sos_image_glyphs_site(key);
    const std::string other = name == kBuilds[0] ? kBuilds[1] : kBuilds[0];
    const void* const handlers[] = {reinterpret_cast<const void*>(sos_image_glyphs_on_enable), reinterpret_cast<const void*>(sos_image_glyphs_on_disable),
                                    reinterpret_cast<const void*>(sos_image_glyphs_on_set_sprite), reinterpret_cast<const void*>(sos_prompt_hook_handler)};
    const char* why = "";
    char message[240];
    for (const int style : {SOS_PROMPTS_XBOX, SOS_PROMPTS_AUTOMATIC})
        Check(sos_image_glyphs_verify(original.data(), size, key, style, &why), name + ": the real code is the verified code");
    Check(!sos_image_glyphs_verify(original.data(), size, key, SOS_PROMPTS_PLAYSTATION, &why), name + ": PlayStation prompts have no hooks");
    Check(!sos_image_glyphs_verify(original.data(), size, other.c_str(), SOS_PROMPTS_AUTOMATIC, &why), name + ": the other build's hooks do not match");
    for (const SosGlyphHook& hook : site.hooks) TestHookCode(name + " +" + std::to_string(hook.method), original, hook);

    const auto refuses = [&](Image copy, uint64_t copy_size, const char* build, int style, size_t capacity, uint64_t stub_at, const std::string& what) {
        const Image before = copy;
        Check(sos_image_glyphs_install(copy.data(), copy_size, build, style, copy.data() + stub_at, capacity, message, sizeof(message)) == 0 && copy == before,
              name + ": " + what + " writes nothing");
    };
    refuses(original, size, "CUSA57767-9.99", SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size + kOther, "an unknown build");
    refuses(original, size, nullptr, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size + kOther, "no build");
    refuses(original, size, key, SOS_PROMPTS_PLAYSTATION, SOS_IMAGE_GLYPHS_STUB_BYTES, size + kOther, "the PlayStation style");
    refuses(original, size, key, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES - 1, size + kOther, "a small stub area");
    refuses(original, site.hooks[0].method + 4, key, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size + kOther, "a truncated image");
    {
        Image copy = original;
        Check(sos_image_glyphs_install(copy.data(), size, key, SOS_PROMPTS_AUTOMATIC, copy.data() + size + (uint64_t{1} << 32), SOS_IMAGE_GLYPHS_STUB_BYTES, message,
                                       sizeof(message)) == 0 && copy == original, name + ": stubs out of reach write nothing");
    }
    for (const SosGlyphHook& hook : site.hooks)
        for (const uint32_t at : {0u, 5u, 6u, hook.length / 2, hook.length - 1}) {
            Image bad = original;
            bad[hook.method + at] ^= 0x01;
            refuses(bad, size, key, SOS_PROMPTS_AUTOMATIC, SOS_IMAGE_GLYPHS_STUB_BYTES, size + kOther, "one changed hooked method (no partial install)");
        }
    for (const SosGlyphRange& range : site.services) {
        Image bad = original;
        bad[range.begin + 8] ^= 0x01;
        refuses(bad, size, key, SOS_PROMPTS_XBOX, SOS_IMAGE_GLYPHS_STUB_BYTES, size + kOther, "a changed function the handlers call");
    }

    for (const int style : {SOS_PROMPTS_XBOX, SOS_PROMPTS_AUTOMATIC}) {
        const bool automatic = style == SOS_PROMPTS_AUTOMATIC;
        const std::string label = name + (automatic ? " Automatic" : " Xbox");
        const int hooks = automatic ? SOS_GLYPH_HOOKS : SOS_GLYPH_HOOK_TEXT_ATLAS;
        Image image = original;
        uint8_t* stubs = image.data() + size + kOther;
        Check(sos_image_glyphs_install(image.data(), size, key, style, stubs, SOS_IMAGE_GLYPHS_STUB_BYTES, message, sizeof(message)) == 1, label + ": the hooks install");
        std::set<uint64_t> allowed;
        for (int i = 0; i < hooks; ++i) {
            for (int k = 0; k < SOS_GLYPH_HOOK_STOLEN; ++k) allowed.insert(site.hooks[i].method + k);
            TestStub(label + " hook " + std::to_string(i), image, original, site.hooks[i], stubs + i * SOS_GLYPH_STUB_SLOT, handlers[i]);
        }
        const std::set<uint64_t> changed = Changed(original, image, size);
        Check(std::includes(allowed.begin(), allowed.end(), changed.begin(), changed.end()) && !changed.empty(), label + ": only the hooked entries change");
        const Image once = image;
        Check(sos_image_glyphs_install(image.data(), size, key, style, stubs, SOS_IMAGE_GLYPHS_STUB_BYTES, message, sizeof(message)) == 0 && image == once,
              label + ": a second install writes nothing");

        // The loader's order: every other patch first, Exit Game last.
        Image all = original;
        char note[240];
        bool applied = true;
        uint8_t* area = all.data() + size;
        if (!automatic) applied &= sos_controller_prompts_apply(all.data(), size, key, SOS_PROMPTS_XBOX, note, sizeof(note)) == SOS_PROMPTS_XBOX;
        applied &= sos_skip_intro_apply(all.data(), size, key, area, SOS_SKIP_INTRO_STUB_BYTES, note, sizeof(note)) == 1;
        applied &= sos_hermes_unlock_apply(all.data(), size, key, note, sizeof(note)) == 1;
        area += SOS_SKIP_INTRO_STUB_BYTES;
        if (automatic) applied &= sos_prompt_hook_install(all.data(), size, key, area, SOS_PROMPT_HOOK_STUB_BYTES, note, sizeof(note)) == 1;
        area += SOS_PROMPT_HOOK_STUB_BYTES;
        applied &= sos_prompt_dialog_install(all.data(), size, key, area, SOS_PROMPT_DIALOG_STUB_BYTES, note, sizeof(note)) == 1;
        area += SOS_PROMPT_DIALOG_STUB_BYTES;
        for (int menu = 0; menu < SOS_PROMPT_MENUS; ++menu, area += SOS_PROMPT_MENU_STUB_BYTES)
            applied &= sos_prompt_menu_install(all.data(), size, key, menu, area, SOS_PROMPT_MENU_STUB_BYTES, note, sizeof(note)) == 1;
        Check(applied, label + ": the other patches apply");
        Check(sos_image_glyphs_install(all.data(), size, key, style, area + SOS_EXIT_GAME_STUB_BYTES, SOS_IMAGE_GLYPHS_STUB_BYTES, message, sizeof(message)) == 1,
              label + ": the glyph hooks install after them");
        Check(sos_exit_game_install(all.data(), size, key, area, SOS_EXIT_GAME_STUB_BYTES, note, sizeof(note)) == 1, label + ": Exit Game installs after the glyph hooks");
        const SosPitUnlockSite* pit = sos_pit_unlock_site(key);
        Check(sos_pit_unlock_verify(all.data(), size, pit, &why), label + ": The Pit unlock is not touched");
        Check(sos_image_glyphs_install(image.data(), size, key, style, stubs, SOS_IMAGE_GLYPHS_STUB_BYTES, message, sizeof(message)) == 0, label + ": still installed once");

        // Hooked first, the other features still find their code.
        Check(sos_prompt_dialog_verify(once.data(), size, key, &why) && sos_prompt_menu_verify(once.data(), size, key, 0, &why) &&
                  sos_prompt_menu_verify(once.data(), size, key, 1, &why) && sos_prompt_hook_verify(once.data(), size, key, &why) &&
                  sos_skip_intro_verify(once.data(), size, sos_skip_intro_site(key), &why) && sos_exit_game_verify(once.data(), size, key, &why) &&
                  sos_hermes_unlock_verify(once.data(), size, sos_hermes_unlock_site(key), &why),
              label + ": no other hook or patch shares its code");
    }

    RzaImage analysis{};
    if (rza_enumerate(original.data(), size, triples.data(), triples.size() / 3, &analysis) && rza_scan(original.data(), size, &analysis, 8, nullptr, 0) == 1) {
        std::set<uint64_t> methods;
        for (const SosGlyphHook& hook : site.hooks) methods.insert(hook.method);
        for (const SosGlyphRange& range : site.services) methods.insert(range.begin);
        size_t seen = 0;
        bool clear = true;
        for (size_t i = 0; i < analysis.count; ++i)
            if (methods.count(analysis.functions[i].begin)) {
                ++seen;
                clear &= !(analysis.functions[i].flags & (RZA_RED | RZA_INSERTQ));
            }
        Check(seen == methods.size() && clear, name + ": the red-zone pass rewrites none of the hooked or called functions");
        rza_free(&analysis);
    } else {
        Check(false, name + ": the red-zone analysis runs");
    }
}

}  // namespace

int main() {
    TestMapping();
    for (const char* key : kBuilds) {
        Check(sos_image_glyphs_site(key) != nullptr, std::string(key) + ": has a site");
        TestStyles(key);
        TestLifetime(key);
        TestLateIcons(key);
        TestTwoPads(key);
        TestContract(key);
    }
    Check(sos_image_glyphs_site(nullptr) == nullptr && sos_image_glyphs_site("CUSA57767-9.99") == nullptr, "other builds have no site");
    if (!std::getenv("SOS_TEST_FAST")) {
        const fs::path root = SOS_SOURCE_DIR;
        for (const char* key : kBuilds) {
            const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the glyph hooks");
            if (boot.found()) TestReal(key, boot.path);
        }
    }
    std::printf("image glyph tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
