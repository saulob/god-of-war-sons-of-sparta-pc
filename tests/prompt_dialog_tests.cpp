// Settings confirmation dialog and campfire menu glyphs: the hook tables, the handlers on fake game objects and the hooks on the real
// boot images of CUSA57767 1.00 and 1.01 (verification, installation, the stubs, and every other patch of the launch).
// Build: see tools/test_prompt_dialog.ps1. No game, launcher or window is started.

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

#include <Zydis/Zydis.h>

#include "build_images.h"
#include "controller_prompts.h"
#include "prompt_dialog.h"
extern "C" {
#include "redzone_analysis.h"
}
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
constexpr int kGamepad = 2;
constexpr int kKeyboard = 1;

// ---- the table -------------------------------------------------------------------------------------------------------------

void TestTable(const char* key) {
    const std::string name = key;
    const SosPromptDialogSite* site = sos_prompt_dialog_site(key);
    Check(site != nullptr, name + ": has a verified dialog hook");
    if (site == nullptr) return;
    Check(sos_prompt_dialog_site("CUSA57767-9.99") == nullptr && sos_prompt_dialog_site(nullptr) == nullptr, name + ": other builds have no dialog hook");
    static const uint8_t prefix[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x80, 0x3d};
    Check(std::memcmp(site->entry, prefix, sizeof(prefix)) == 0 && site->entry[17] == 0x00,
          name + ": the entry is six pushes/moves and cmp byte [rip+x],0 (18 bytes)");
    Check(site->text_list == 0x20 && site->icon_data == 0x20 && site->input_device == 0x30 && site->override_sprite == 0xe0 && site->xbox_confirm == 0x168 &&
              site->xbox_back == 0x178,
          name + ": object layout: textList, gamepadIcons, input device, overrideSprite, XboxSeries South and East");
    Check(site->hash != 0 && site->method == site->ranges[0].begin, name + ": the first hashed range is the method");
    std::set<uint32_t> starts;
    for (const SosPromptDialogRange& range : site->ranges) starts.insert(range.begin);
    Check(starts.count(site->get_component) && starts.count(site->get_overseer) && starts.count(site->set_override_sprite) && starts.count(site->transform_of) &&
              starts.count(site->parent_of) && starts.count(site->child_count) && starts.count(site->child_at),
          name + ": every game function the handler calls is in a hashed range");
    Check(site->image_slot != 0 && site->overseer_slot != 0 && site->image_slot != site->overseer_slot, name + ": the two method info slots are distinct");
    Check(sizeof(void*) * 6 % 16 == 0 && (8 + 6 * 8 + 8) % 16 == 0, name + ": the stub keeps the stack 16-byte aligned at its call");
}

// ---- the handler on fake game objects --------------------------------------------------------------------------------------

struct Object {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x200, 0);
    void* ptr() { return bytes.data(); }
    void Set(size_t offset, const void* value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
    void SetInt(size_t offset, int32_t value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
    void* Get(size_t offset) const {
        void* value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
};

struct World {
    std::map<const void*, void*> transformOf, parentOf, imageOf;
    std::map<const void*, std::vector<void*>> children;
    void* overseer = nullptr;
    const void* imageMethod = nullptr;
    const void* overseerMethod = nullptr;
    int setCalls = 0;
    int badArguments = 0;
};
World* g_world = nullptr;

SOS_PROMPT_DIALOG_SYSV void* FakeGetComponent(void* self, const void* method) {
    const auto found = g_world->imageOf.find(self);
    return method == g_world->imageMethod && found != g_world->imageOf.end() ? found->second : nullptr;
}
SOS_PROMPT_DIALOG_SYSV void* FakeGetOverseer(const void* method) { return method == g_world->overseerMethod ? g_world->overseer : nullptr; }
SOS_PROMPT_DIALOG_SYSV void FakeSetOverride(void* image, void* sprite, const void* method) {
    g_world->badArguments += method != nullptr;
    std::memcpy(static_cast<uint8_t*>(image) + 0xe0, &sprite, sizeof(sprite));
    ++g_world->setCalls;
}
SOS_PROMPT_DIALOG_SYSV void* FakeTransformOf(void* component, const void* method) {
    g_world->badArguments += method != nullptr;
    const auto found = g_world->transformOf.find(component);
    return found != g_world->transformOf.end() ? found->second : nullptr;
}
SOS_PROMPT_DIALOG_SYSV void* FakeParentOf(void* transform, const void* method) {
    g_world->badArguments += method != nullptr;
    const auto found = g_world->parentOf.find(transform);
    return found != g_world->parentOf.end() ? found->second : nullptr;
}
SOS_PROMPT_DIALOG_SYSV int FakeChildCount(void* transform, const void* method) {
    g_world->badArguments += method != nullptr;
    return static_cast<int>(g_world->children[transform].size());
}
SOS_PROMPT_DIALOG_SYSV void* FakeChildAt(void* transform, int index, const void* method) {
    g_world->badArguments += method != nullptr;
    return g_world->children[transform][static_cast<size_t>(index)];
}

constexpr size_t kLabels = 4;

// A PromptTextSwitch with four labels; the glyph Image of label 2 (Confirm) and label 3 (Back) is the only sibling with an Image.
struct Scene {
    World world;
    std::deque<Object> objects;
    Object* prompt = nullptr;
    Object* list = nullptr;
    Object* array = nullptr;
    Object* overseer = nullptr;
    Object* icons = nullptr;
    Object* xboxA = nullptr;
    Object* xboxB = nullptr;
    Object* confirmImage = nullptr;
    Object* backImage = nullptr;
    Object* label[kLabels] = {};
    Object* labelTransform[kLabels] = {};
    Object* parent[kLabels] = {};
    Object* glyph[kLabels] = {};
    Object imageMethod, overseerMethod;
    uintptr_t imageSlot = 0, overseerSlot = 0;
    SosPromptDialogSite site{};

    Object* New() {
        objects.emplace_back();
        Object* object = &objects.back();
        object->Set(0x10, object->ptr());  // the native object
        return object;
    }

    explicit Scene(const char* key) {
        site = *sos_prompt_dialog_site(key);
        world.imageMethod = imageMethod.ptr();
        world.overseerMethod = overseerMethod.ptr();
        imageSlot = reinterpret_cast<uintptr_t>(imageMethod.ptr());
        overseerSlot = reinterpret_cast<uintptr_t>(overseerMethod.ptr());
        prompt = New();
        list = New();
        array = New();
        list->Set(0x10, array->ptr());
        list->SetInt(0x18, static_cast<int32_t>(kLabels));
        array->SetInt(0x18, static_cast<int32_t>(kLabels));
        prompt->Set(site.text_list, list->ptr());
        for (size_t i = 0; i < kLabels; ++i) {
            label[i] = New();
            labelTransform[i] = New();
            parent[i] = New();
            glyph[i] = New();
            array->Set(0x20 + 8 * i, label[i]->ptr());
            world.transformOf[label[i]->ptr()] = labelTransform[i]->ptr();
            world.parentOf[labelTransform[i]->ptr()] = parent[i]->ptr();
            world.children[parent[i]->ptr()] = {labelTransform[i]->ptr(), glyph[i]->ptr()};
        }
        confirmImage = New();
        backImage = New();
        world.imageOf[glyph[2]->ptr()] = confirmImage->ptr();
        world.imageOf[glyph[3]->ptr()] = backImage->ptr();
        overseer = New();
        icons = New();
        xboxA = New();
        xboxB = New();
        overseer->Set(site.icon_data, icons->ptr());
        overseer->SetInt(site.input_device, kGamepad);
        icons->Set(site.xbox_confirm, xboxA->ptr());
        icons->Set(site.xbox_back, xboxB->ptr());
        world.overseer = overseer->ptr();
    }

    void Arm(int style) {
        SosPromptDialogGame game{};
        game.get_component = FakeGetComponent;
        game.get_overseer = FakeGetOverseer;
        game.set_override_sprite = FakeSetOverride;
        game.transform_of = FakeTransformOf;
        game.parent_of = FakeParentOf;
        game.child_count = FakeChildCount;
        game.child_at = FakeChildAt;
        game.image_slot = &imageSlot;
        game.overseer_slot = &overseerSlot;
        g_world = &world;
        sos_prompt_dialog_arm_with(style, &site, &game);
    }

    void Run() {
        g_world = &world;
        sos_prompt_dialog_handler(prompt->ptr());
    }

    std::vector<uint8_t> Snapshot() const {
        std::vector<uint8_t> all;
        for (const Object& object : objects) all.insert(all.end(), object.bytes.begin(), object.bytes.end());
        return all;
    }
    void* ConfirmOverride() const { return confirmImage->Get(site.override_sprite); }
    void* BackOverride() const { return backImage->Get(site.override_sprite); }
};

void ExpectUntouched(Scene& scene, const std::string& name) {
    const std::vector<uint8_t> before = scene.Snapshot();
    scene.world.setCalls = 0;
    scene.Run();
    Check(scene.Snapshot() == before && scene.world.setCalls == 0 && scene.world.badArguments == 0, name + ": nothing is written");
}

void TestHandler(const char* key) {
    const std::string name = std::string(key) + ": ";
    sos_pad_kind_publish(SOS_PAD_NONE);

    {
        Scene s(key);
        s.Arm(SOS_PROMPTS_XBOX);
        s.Run();
        Check(s.ConfirmOverride() == s.xboxA->ptr() && s.BackOverride() == s.xboxB->ptr(), name + "forced Xbox: Confirm is Xbox A (South), Back is Xbox B (East)");
        Check(s.world.setCalls == 2 && s.world.badArguments == 0, name + "forced Xbox: exactly the two overrideSprite writes");
        s.Run();
        Check(s.ConfirmOverride() == s.xboxA->ptr() && s.BackOverride() == s.xboxB->ptr(), name + "forced Xbox: opening the dialog again keeps A and B");
        Check(s.world.badArguments == 0, name + "the game's transform and child functions receive no method info");
    }
    {
        Scene s(key);
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Arm(SOS_PROMPTS_PLAYSTATION);
        ExpectUntouched(s, name + "forced PlayStation with an Xbox controller keeps the serialized Cross and Circle");
        Check(s.ConfirmOverride() == nullptr && s.BackOverride() == nullptr, name + "forced PlayStation: overrideSprite stays empty");
    }
    {
        Scene s(key);
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Arm(SOS_PROMPTS_AUTOMATIC);
        s.Run();
        Check(s.ConfirmOverride() == s.xboxA->ptr() && s.BackOverride() == s.xboxB->ptr(), name + "Automatic with an Xbox controller: A and B");
        sos_pad_kind_publish(SOS_PAD_PLAYSTATION4);
        s.Run();
        Check(s.ConfirmOverride() == nullptr && s.BackOverride() == nullptr, name + "Automatic switched to a DualShock 4: overrideSprite cleared (serialized Cross and Circle)");
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Run();
        Check(s.ConfirmOverride() == s.xboxA->ptr() && s.BackOverride() == s.xboxB->ptr(), name + "Automatic back to an Xbox controller: A and B restored");
        sos_pad_kind_publish(SOS_PAD_PLAYSTATION5);
        s.Run();
        Check(s.ConfirmOverride() == nullptr && s.BackOverride() == nullptr, name + "Automatic switched to a DualSense: overrideSprite cleared");
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Run();
        s.overseer->SetInt(s.site.input_device, kKeyboard);
        s.Run();
        Check(s.ConfirmOverride() == nullptr && s.BackOverride() == nullptr, name + "Automatic with an Xbox controller but keyboard input: overrideSprite cleared");
        s.overseer->SetInt(s.site.input_device, kGamepad);
        s.Run();
        Check(s.ConfirmOverride() == s.xboxA->ptr() && s.BackOverride() == s.xboxB->ptr(), name + "and A and B again once the gamepad is the input");
    }
    for (const int kind : {SOS_PAD_PLAYSTATION4, SOS_PAD_PLAYSTATION5, SOS_PAD_OTHER, SOS_PAD_NONE}) {
        Scene s(key);
        sos_pad_kind_publish(kind);
        s.Arm(SOS_PROMPTS_AUTOMATIC);
        ExpectUntouched(s, name + "Automatic with controller kind " + std::to_string(kind) + " (PlayStation fallback)");
    }

    // The dialog's own sprite stays when nothing was overridden; a stale override (set by someone else) is only cleared after ours.
    {
        Scene s(key);
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Arm(SOS_PROMPTS_AUTOMATIC);
        s.Run();
        const std::vector<uint8_t> overridden = s.Snapshot();
        sos_pad_kind_publish(SOS_PAD_OTHER);
        s.Run();
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Run();
        Check(s.Snapshot() == overridden, name + "Xbox, other, Xbox again returns to the identical object state");
    }

    // Safe fallbacks: every missing or unexpected piece leaves the dialog exactly as it is.
    sos_pad_kind_publish(SOS_PAD_XBOX);
    const auto fallback = [&](const std::string& label, auto&& damage) {
        Scene s(key);
        damage(s);
        s.Arm(SOS_PROMPTS_AUTOMATIC);
        ExpectUntouched(s, name + label);
    };
    fallback("missing InputOverseer", [](Scene& s) { s.world.overseer = nullptr; });
    fallback("missing InputIconData", [](Scene& s) { s.overseer->Set(s.site.icon_data, nullptr); });
    fallback("InputIconData without its native object", [](Scene& s) { s.icons->Set(0x10, nullptr); });
    fallback("missing Xbox Confirm sprite", [](Scene& s) { s.icons->Set(s.site.xbox_confirm, nullptr); });
    fallback("missing Xbox Back sprite", [](Scene& s) { s.icons->Set(s.site.xbox_back, nullptr); });
    fallback("destroyed Xbox Confirm sprite", [](Scene& s) { s.xboxA->Set(0x10, nullptr); });
    fallback("destroyed Xbox Back sprite", [](Scene& s) { s.xboxB->Set(0x10, nullptr); });
    fallback("missing Confirm label", [](Scene& s) { s.array->Set(0x20 + 8 * 2, nullptr); });
    fallback("missing Back label", [](Scene& s) { s.array->Set(0x20 + 8 * 3, nullptr); });
    fallback("destroyed Confirm label", [](Scene& s) { s.label[2]->Set(0x10, nullptr); });
    fallback("label list too short", [](Scene& s) { s.list->SetInt(0x18, 3); });
    fallback("label array too short", [](Scene& s) { s.array->SetInt(0x18, 3); });
    fallback("no label list", [](Scene& s) { s.prompt->Set(s.site.text_list, nullptr); });
    fallback("no label array", [](Scene& s) { s.list->Set(0x10, nullptr); });
    fallback("missing Image sibling of Confirm", [](Scene& s) { s.world.imageOf.erase(s.glyph[2]->ptr()); });
    fallback("missing Image sibling of Back", [](Scene& s) { s.world.imageOf.erase(s.glyph[3]->ptr()); });
    fallback("two Image siblings of Confirm", [](Scene& s) { s.world.imageOf[s.labelTransform[2]->ptr()] = s.backImage->ptr(); });
    fallback("Confirm and Back share one Image", [](Scene& s) { s.world.imageOf[s.glyph[3]->ptr()] = s.confirmImage->ptr(); });
    fallback("label without a transform", [](Scene& s) { s.world.transformOf.erase(s.label[2]->ptr()); });
    fallback("label without a parent", [](Scene& s) { s.world.parentOf.erase(s.labelTransform[3]->ptr()); });
    fallback("unexpected object: no native pointer", [](Scene& s) { s.prompt->Set(0x10, nullptr); });
    fallback("unexpected object: no label list at all", [](Scene& s) { std::fill(s.prompt->bytes.begin() + 0x18, s.prompt->bytes.end(), 0); });
    fallback("InputOverseer method info not resolved yet", [](Scene& s) { s.overseerSlot |= 1; });
    fallback("Image method info not resolved yet", [](Scene& s) { s.imageSlot |= 1; });
    {
        Scene s(key);
        s.Arm(SOS_PROMPTS_XBOX);
        g_world = &s.world;
        sos_prompt_dialog_handler(nullptr);
        Check(s.world.setCalls == 0, name + "a missing PromptTextSwitch is ignored");
        sos_prompt_dialog_arm_with(SOS_PROMPTS_XBOX, nullptr, nullptr);
        sos_prompt_dialog_handler(s.prompt->ptr());
        Check(s.world.setCalls == 0, name + "a hook that was never armed does nothing");
    }
    {
        // Partial failure: the Back glyph cannot be found while an override of Confirm is already in place.
        Scene s(key);
        s.Arm(SOS_PROMPTS_XBOX);
        s.Run();
        s.world.imageOf.erase(s.glyph[3]->ptr());
        s.icons->Set(s.site.xbox_confirm, s.xboxB->ptr());
        const std::vector<uint8_t> before = s.Snapshot();
        s.Run();
        Check(s.Snapshot() == before, name + "when only one glyph can be found neither is touched");
    }
    {
        Scene s(key);
        sos_pad_kind_publish(SOS_PAD_XBOX);
        s.Arm(SOS_PROMPTS_AUTOMATIC);
        s.Run();
        const void* const other = s.xboxB->ptr();
        s.icons->Set(s.site.xbox_confirm, other);
        s.Run();
        Check(s.ConfirmOverride() == other, name + "the sprite follows the game's icon data, not a copy");
    }
    sos_pad_kind_publish(SOS_PAD_NONE);
    sos_prompt_dialog_arm_with(SOS_PROMPTS_PLAYSTATION, nullptr, nullptr);
}

// ---- the campfire and pit player menus -------------------------------------------------------------------------------------

constexpr const char* kMenuNames[] = {"campfire menu", "pit player menu"};

void CallMenuHandler(int menu, void* object) {
    if (menu == 0) sos_prompt_menu_handler_save(object);
    else sos_prompt_menu_handler_pit(object);
}

void TestMenuTable(const char* key) {
    const std::string name = std::string(key) + ": ";
    const SosPromptDialogSite* site = sos_prompt_dialog_site(key);
    if (site == nullptr) return;
    static const uint8_t entry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    Check(site->services != 0, name + "the code the menu handlers call has its own hash");
    for (int m = 0; m < SOS_PROMPT_MENUS; ++m) {
        const SosPromptMenu& menu = site->menus[m];
        const std::string label = name + kMenuNames[m] + ": ";
        Check(menu.method != 0 && menu.length > 0x100 && menu.hash != 0, label + "has a method, a length and a hash");
        Check(std::memcmp(menu.entry, entry, sizeof(entry)) == 0, label + "the entry is push rbp; mov rbp,rsp; push r15");
        Check(menu.image_count == (m == 0 ? 4u : 2u), label + "lists all of its confirm prompt Images");
        Check(menu.images[0] == 0x198 && menu.images[1] == 0x1a0 && (m != 0 || (menu.images[2] == 0x1a8 && menu.images[3] == 0x1b0)), label + "the Image fields are the layout read from the metadata");
        for (const SosPromptDialogRange& range : site->ranges) {
            Check(uint64_t{menu.method} + menu.length <= range.begin || menu.method >= range.end, label + "does not overlap code the dialog hook hashes");
        }
    }
    Check(site->menus[0].method != site->menus[1].method, name + "the two menus are two methods");
    Check(sos_prompt_menu_verify(nullptr, 0, "CUSA57767-9.99", 0, nullptr) == 0 && sos_prompt_menu_verify(nullptr, 0, nullptr, 0, nullptr) == 0 &&
              sos_prompt_menu_verify(nullptr, 0, key, -1, nullptr) == 0 && sos_prompt_menu_verify(nullptr, 0, key, SOS_PROMPT_MENUS, nullptr) == 0,
          name + "an unknown build or menu has no hook");
}

struct MenuScene {
    World world;
    std::deque<Object> objects;
    Object* menu = nullptr;
    Object* overseer = nullptr;
    Object* icons = nullptr;
    Object* xboxA = nullptr;
    Object* xboxB = nullptr;
    Object* image[SOS_PROMPT_MENU_IMAGES] = {};
    Object overseerMethod;
    uintptr_t overseerSlot = 0;
    SosPromptDialogSite site{};
    int index = 0;
    size_t count = 0;

    Object* New() {
        objects.emplace_back();
        Object* object = &objects.back();
        object->Set(0x10, object->ptr());
        return object;
    }

    MenuScene(const char* key, int menuIndex) : index(menuIndex) {
        site = *sos_prompt_dialog_site(key);
        world.overseerMethod = overseerMethod.ptr();
        overseerSlot = reinterpret_cast<uintptr_t>(overseerMethod.ptr());
        menu = New();
        count = site.menus[index].image_count;
        for (size_t i = 0; i < count; ++i) {
            image[i] = New();
            menu->Set(site.menus[index].images[i], image[i]->ptr());
        }
        overseer = New();
        icons = New();
        xboxA = New();
        xboxB = New();
        overseer->Set(site.icon_data, icons->ptr());
        overseer->SetInt(site.input_device, kGamepad);
        icons->Set(site.xbox_confirm, xboxA->ptr());
        icons->Set(site.xbox_back, xboxB->ptr());
        world.overseer = overseer->ptr();
    }

    void Arm(int style) {
        SosPromptDialogGame game{};
        game.get_overseer = FakeGetOverseer;
        game.set_override_sprite = FakeSetOverride;
        game.overseer_slot = &overseerSlot;
        g_world = &world;
        sos_prompt_dialog_arm_with(style, &site, &game);
    }

    void Run() {
        g_world = &world;
        CallMenuHandler(index, menu->ptr());
    }

    std::vector<uint8_t> Snapshot() const {
        std::vector<uint8_t> all;
        for (const Object& object : objects) all.insert(all.end(), object.bytes.begin(), object.bytes.end());
        return all;
    }

    void* OverrideOf(size_t i) const { return image[i]->Get(site.override_sprite); }
    bool AllOverride(const void* sprite) const {
        for (size_t i = 0; i < count; ++i) {
            if (OverrideOf(i) != sprite) return false;
        }
        return true;
    }
};

void ExpectMenuUntouched(MenuScene& scene, const std::string& name) {
    const std::vector<uint8_t> before = scene.Snapshot();
    scene.world.setCalls = 0;
    scene.Run();
    Check(scene.Snapshot() == before && scene.world.setCalls == 0 && scene.world.badArguments == 0, name + ": nothing is written");
}

void TestMenuHandler(const char* key) {
    sos_pad_kind_publish(SOS_PAD_NONE);
    for (int m = 0; m < SOS_PROMPT_MENUS; ++m) {
        const std::string name = std::string(key) + ": " + kMenuNames[m] + ": ";
        {
            MenuScene s(key, m);
            s.Arm(SOS_PROMPTS_XBOX);
            s.Run();
            Check(s.AllOverride(s.xboxA->ptr()), name + "forced Xbox: every confirm prompt is Xbox A (South)");
            Check(s.world.setCalls == static_cast<int>(s.count) && s.world.badArguments == 0, name + "forced Xbox: exactly one overrideSprite write per prompt Image");
            Check(std::none_of(s.image, s.image + s.count, [&](const Object* image) { return image->Get(s.site.override_sprite) == s.xboxB->ptr(); }), name + "forced Xbox: no prompt shows Xbox B (East, the back action)");
            s.Run();
            Check(s.AllOverride(s.xboxA->ptr()), name + "forced Xbox: opening the menu again keeps A");
        }
        {
            MenuScene s(key, m);
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Arm(SOS_PROMPTS_PLAYSTATION);
            ExpectMenuUntouched(s, name + "forced PlayStation with an Xbox controller keeps the serialized Cross");
            Check(s.AllOverride(nullptr), name + "forced PlayStation: overrideSprite stays empty");
        }
        {
            MenuScene s(key, m);
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Arm(SOS_PROMPTS_AUTOMATIC);
            s.Run();
            Check(s.AllOverride(s.xboxA->ptr()), name + "Automatic with an Xbox controller: A");
            sos_pad_kind_publish(SOS_PAD_PLAYSTATION4);
            s.Run();
            Check(s.AllOverride(nullptr), name + "Automatic switched to a DualShock 4: the serialized Cross is back (no stale Xbox A)");
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Run();
            Check(s.AllOverride(s.xboxA->ptr()), name + "Automatic back to an Xbox controller: A (no stale Cross)");
            sos_pad_kind_publish(SOS_PAD_PLAYSTATION5);
            s.Run();
            Check(s.AllOverride(nullptr), name + "Automatic switched to a DualSense: Cross");
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Run();
            s.overseer->SetInt(s.site.input_device, kKeyboard);
            s.Run();
            Check(s.AllOverride(nullptr), name + "Automatic with an Xbox controller but keyboard input: Cross");
            s.overseer->SetInt(s.site.input_device, kGamepad);
            s.Run();
            Check(s.AllOverride(s.xboxA->ptr()), name + "and A again once the gamepad is the input");
        }
        for (const int kind : {SOS_PAD_PLAYSTATION4, SOS_PAD_PLAYSTATION5, SOS_PAD_OTHER, SOS_PAD_NONE}) {
            MenuScene s(key, m);
            sos_pad_kind_publish(kind);
            s.Arm(SOS_PROMPTS_AUTOMATIC);
            ExpectMenuUntouched(s, name + "Automatic with controller kind " + std::to_string(kind) + " (PlayStation fallback)");
        }
        {
            MenuScene s(key, m);
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Arm(SOS_PROMPTS_AUTOMATIC);
            s.Run();
            const void* const other = s.xboxB->ptr();
            s.icons->Set(s.site.xbox_confirm, other);
            s.icons->Set(s.site.xbox_back, s.menu->ptr());
            s.Run();
            Check(s.AllOverride(other) && s.world.badArguments == 0, name + "the sprite is the game's XboxSeries confirm sprite at its offset, never the back sprite");
        }
        {
            MenuScene s(key, m);
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Arm(SOS_PROMPTS_AUTOMATIC);
            s.Run();
            const std::vector<uint8_t> overridden = s.Snapshot();
            sos_pad_kind_publish(SOS_PAD_OTHER);
            s.Run();
            sos_pad_kind_publish(SOS_PAD_XBOX);
            s.Run();
            Check(s.Snapshot() == overridden, name + "Xbox, other, Xbox again returns to the identical object state");
        }
        sos_pad_kind_publish(SOS_PAD_XBOX);
        const auto fallback = [&](const std::string& label, auto&& damage) {
            MenuScene s(key, m);
            damage(s);
            s.Arm(SOS_PROMPTS_AUTOMATIC);
            ExpectMenuUntouched(s, name + label);
        };
        fallback("missing InputOverseer", [](MenuScene& s) { s.world.overseer = nullptr; });
        fallback("missing InputIconData", [](MenuScene& s) { s.overseer->Set(s.site.icon_data, nullptr); });
        fallback("InputIconData without its native object", [](MenuScene& s) { s.icons->Set(0x10, nullptr); });
        fallback("missing Xbox confirm sprite", [](MenuScene& s) { s.icons->Set(s.site.xbox_confirm, nullptr); });
        fallback("destroyed Xbox confirm sprite", [](MenuScene& s) { s.xboxA->Set(0x10, nullptr); });
        fallback("InputOverseer method info not resolved yet", [](MenuScene& s) { s.overseerSlot |= 1; });
        fallback("menu without its native object", [](MenuScene& s) { s.menu->Set(0x10, nullptr); });
        {
            MenuScene s(key, m);
            s.Arm(SOS_PROMPTS_XBOX);
            s.menu->Set(s.site.menus[m].images[0], nullptr);
            s.Run();
            Check(s.OverrideOf(1) == s.xboxA->ptr() && s.world.setCalls == static_cast<int>(s.count) - 1, name + "a missing prompt Image is skipped and the others still show A");
        }
        {
            MenuScene s(key, m);
            s.Arm(SOS_PROMPTS_XBOX);
            s.image[1]->Set(0x10, nullptr);
            s.Run();
            Check(s.OverrideOf(0) == s.xboxA->ptr() && s.OverrideOf(1) == nullptr, name + "a destroyed prompt Image is skipped and the others still show A");
        }
        {
            MenuScene s(key, m);
            s.Arm(SOS_PROMPTS_XBOX);
            g_world = &s.world;
            CallMenuHandler(m, nullptr);
            Check(s.world.setCalls == 0, name + "a missing menu is ignored");
            sos_prompt_dialog_arm_with(SOS_PROMPTS_XBOX, nullptr, nullptr);
            s.Run();
            Check(s.world.setCalls == 0, name + "a hook that was never armed does nothing");
        }
    }
    sos_pad_kind_publish(SOS_PAD_NONE);
    sos_prompt_dialog_arm_with(SOS_PROMPTS_PLAYSTATION, nullptr, nullptr);
}

// ---- the real boot images --------------------------------------------------------------------------------------------------

using Image = std::vector<uint8_t>;

uint64_t ReadU64(std::istream& in) {
    uint64_t value = 0;
    in.read(reinterpret_cast<char*>(&value), 8);
    return value;
}

struct Boot {
    Image image;
    uint64_t size = 0;
    std::vector<uint64_t> segments;
};

constexpr uint64_t kStubRoom = SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES + SOS_PROMPT_DIALOG_STUB_BYTES + SOS_PROMPT_MENUS * SOS_PROMPT_MENU_STUB_BYTES;

bool ReadBoot(const fs::path& path, Boot& boot) {
    std::ifstream in(path, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "SOSBOOT1", 8) != 0) return false;
    boot.size = ReadU64(in);
    ReadU64(in);
    const uint64_t segments = ReadU64(in), relocs = ReadU64(in), imports = ReadU64(in);
    for (int i = 0; i < 6; ++i) ReadU64(in);
    const uint64_t modules = ReadU64(in);
    in.seekg(static_cast<std::streamoff>(modules * 56), std::ios::cur);
    const uint64_t bindings = ReadU64(in);
    in.seekg(static_cast<std::streamoff>(bindings * 24), std::ios::cur);
    boot.segments.resize(segments * 3);
    for (uint64_t& value : boot.segments) value = ReadU64(in);
    in.seekg(static_cast<std::streamoff>(imports * 128 + relocs * 32), std::ios::cur);
    boot.image.assign(boot.size + kStubRoom, 0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(boot.image.data()), static_cast<std::streamsize>(boot.size)));
}

std::set<uint64_t> Changed(const Image& before, const Image& after, uint64_t size) {
    std::set<uint64_t> bytes;
    for (uint64_t i = 0; i < size; ++i) {
        if (before[i] != after[i]) bytes.insert(i);
    }
    return bytes;
}

constexpr uint8_t kExpectedHead[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
constexpr uint8_t kExpectedTail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};

struct Decoded {
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
};

bool Decode(const ZydisDecoder& decoder, const uint8_t* at, size_t room, Decoded& out) {
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, room, &out.instruction, out.operands));
}

bool IsRax(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_RAX || reg == ZYDIS_REGISTER_EAX || reg == ZYDIS_REGISTER_AX || reg == ZYDIS_REGISTER_AL;
}

bool ReadsRax(const Decoded& d) {
    for (int k = 0; k < d.instruction.operand_count; ++k) {
        const ZydisDecodedOperand& op = d.operands[k];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && IsRax(op.reg.value) && (op.actions & ZYDIS_OPERAND_ACTION_READ)) return true;
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && (IsRax(op.mem.base) || IsRax(op.mem.index))) return true;
    }
    return false;
}

bool WritesRaxFully(const Decoded& d) {
    for (int k = 0; k < d.instruction.operand_count; ++k) {
        const ZydisDecodedOperand& op = d.operands[k];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && (op.reg.value == ZYDIS_REGISTER_RAX || op.reg.value == ZYDIS_REGISTER_EAX) &&
            (op.actions & ZYDIS_OPERAND_ACTION_WRITE) && !(op.actions & ZYDIS_OPERAND_ACTION_READ))
            return true;
    }
    return false;
}

// Follows every path from `at` until a call, jump, return or rax is written: none may read rax first.
bool RaxDeadFrom(const ZydisDecoder& decoder, const uint8_t* method, size_t length, size_t start) {
    std::vector<size_t> work = {start};
    std::set<size_t> seen;
    while (!work.empty()) {
        size_t at = work.back();
        work.pop_back();
        for (int steps = 0; steps < 40 && at < length && seen.insert(at).second; ++steps) {
            Decoded d;
            if (!Decode(decoder, method + at, length - at, d)) return false;
            if (ReadsRax(d)) return false;
            if (WritesRaxFully(d)) break;
            const auto category = d.instruction.meta.category;
            if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_RET || category == ZYDIS_CATEGORY_UNCOND_BR) break;
            if (category == ZYDIS_CATEGORY_COND_BR) {
                for (int k = 0; k < d.instruction.operand_count_visible; ++k) {
                    if (d.operands[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.operands[k].imm.is_relative)
                        work.push_back(at + d.instruction.length + static_cast<size_t>(static_cast<int64_t>(d.operands[k].imm.value.s)));
                }
            }
            at += d.instruction.length;
        }
    }
    return true;
}

void TestEntryCode(const std::string& name, const Image& image, const SosPromptDialogSite& site) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    const uint8_t* method = image.data() + site.method;
    const size_t length = site.ranges[0].end - site.ranges[0].begin;
    std::vector<size_t> lengths;
    size_t at = 0;
    bool relative = false, position_independent = true;
    while (at < SOS_PROMPT_DIALOG_ENTRY_BYTES) {
        Decoded d;
        if (!Decode(decoder, method + at, length - at, d)) break;
        const bool rip = std::any_of(d.operands, d.operands + d.instruction.operand_count,
                                     [](const ZydisDecodedOperand& op) { return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP; });
        if (rip) relative = true;
        else if (d.instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) position_independent = false;
        const auto category = d.instruction.meta.category;
        if (category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_UNCOND_BR || category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_RET) position_independent = false;
        lengths.push_back(d.instruction.length);
        at += d.instruction.length;
    }
    Check(at == SOS_PROMPT_DIALOG_ENTRY_BYTES && lengths == std::vector<size_t>({1, 3, 2, 2, 2, 1, 7}), name + ": the 18 stolen bytes are seven whole instructions");
    Check(relative && position_independent, name + ": only the final compare is RIP-relative (rebuilt with an absolute address), nothing else depends on its address");

    std::vector<uint64_t> targets;
    size_t decoded = 0;
    for (at = 0; at < length;) {
        Decoded d;
        if (!Decode(decoder, method + at, length - at, d)) break;
        if (d.instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            for (int k = 0; k < d.instruction.operand_count_visible; ++k) {
                if (d.operands[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.operands[k].imm.is_relative) targets.push_back(at + d.instruction.length + static_cast<int64_t>(d.operands[k].imm.value.s));
            }
        }
        at += d.instruction.length;
        ++decoded;
        if (d.instruction.meta.category == ZYDIS_CATEGORY_RET && at + 16 >= length) break;
    }
    Check(decoded > 100, name + ": the method decodes (" + std::to_string(decoded) + " instructions swept)");
    Check(std::none_of(targets.begin(), targets.end(), [](uint64_t target) { return target > 0 && target < SOS_PROMPT_DIALOG_ENTRY_BYTES; }), name + ": no branch of the method lands inside the stolen bytes");

    Decoded first, second, third;
    const bool decodedNext = Decode(decoder, method + SOS_PROMPT_DIALOG_ENTRY_BYTES, length, first) &&
                             Decode(decoder, method + SOS_PROMPT_DIALOG_ENTRY_BYTES + first.instruction.length, length, second) &&
                             Decode(decoder, method + SOS_PROMPT_DIALOG_ENTRY_BYTES + first.instruction.length + second.instruction.length, length, third);
    Check(decodedNext && first.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && second.instruction.mnemonic == ZYDIS_MNEMONIC_MOV && third.instruction.mnemonic == ZYDIS_MNEMONIC_JNZ,
          name + ": the instructions after the entry are mov, mov and jne, so the rebuilt compare's flags reach the branch unchanged");
    Check(RaxDeadFrom(decoder, method, length, SOS_PROMPT_DIALOG_ENTRY_BYTES), name + ": rax is not read after the entry before it is written (the rebuilt compare may use it)");
}

void TestHookReal(const char* key, const fs::path& boot_path) {
    const std::string name = key;
    const SosPromptDialogSite& site = *sos_prompt_dialog_site(key);
    Boot boot;
    if (!ReadBoot(boot_path, boot)) {
        Check(false, name + ": the real image is readable");
        return;
    }
    Image& image = boot.image;
    const uint64_t size = boot.size;
    const Image original = image;
    const char* why = "";
    Check(sos_prompt_dialog_verify(image.data(), size, key, &why), name + ": the real method and every function it relies on are the verified code (every byte hashed)");
    const std::string other = std::string(key) == kBuilds[0] ? kBuilds[1] : kBuilds[0];
    Check(!sos_prompt_dialog_verify(image.data(), size, other.c_str(), &why), name + ": the other build's hook does not match this code");
    Check(site.method != sos_prompt_dialog_site(other.c_str())->method, name + ": the method address is this build's own");
    TestEntryCode(name, image, site);

    uint8_t* stub = image.data() + size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES;
    char message[200];
    Check(sos_prompt_dialog_install(image.data(), size, key, stub, 40, message, sizeof(message)) == 0 && image == original, name + ": a stub area that is too small refuses and writes nothing");
    Check(sos_prompt_dialog_install(image.data(), size, key, image.data() + size + (uint64_t{1} << 32), SOS_PROMPT_DIALOG_STUB_BYTES, message, sizeof(message)) == 0 && image == original,
          name + ": a stub out of 32-bit reach refuses and writes nothing");
    Check(sos_prompt_dialog_install(image.data(), size, key, stub, SOS_PROMPT_DIALOG_STUB_BYTES, message, sizeof(message)) == 1, name + ": the hook installs");

    const std::set<uint64_t> changed = Changed(original, image, size);
    Check(!changed.empty() && *changed.begin() >= site.method && *changed.rbegin() < uint64_t{site.method} + SOS_PROMPT_DIALOG_ENTRY_BYTES,
          name + ": only the method's first 18 bytes of the image change");
    const uint8_t* method = image.data() + site.method;
    int32_t rel = 0;
    std::memcpy(&rel, method + 1, 4);
    Check(method[0] == 0xe9 && method + 5 + rel == stub && std::all_of(method + 5, method + SOS_PROMPT_DIALOG_ENTRY_BYTES, [](uint8_t b) { return b == 0x90; }),
          name + ": the method starts with a jump to the stub and nops up to the resume address");

    int32_t displacement = 0;
    std::memcpy(&displacement, original.data() + site.method + 13, 4);
    const uint64_t flag = reinterpret_cast<uint64_t>(image.data() + site.method + SOS_PROMPT_DIALOG_ENTRY_BYTES + displacement);
    std::vector<uint8_t> expected(kExpectedHead, kExpectedHead + sizeof(kExpectedHead));
    const uint64_t handler = reinterpret_cast<uint64_t>(&sos_prompt_dialog_handler);
    expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&handler), reinterpret_cast<const uint8_t*>(&handler) + 8);
    expected.insert(expected.end(), kExpectedTail, kExpectedTail + sizeof(kExpectedTail));
    expected.insert(expected.end(), original.begin() + site.method, original.begin() + site.method + 11);
    expected.push_back(0x48);
    expected.push_back(0xb8);
    expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&flag), reinterpret_cast<const uint8_t*>(&flag) + 8);
    expected.push_back(0x80);
    expected.push_back(0x38);
    expected.push_back(0x00);
    expected.push_back(0xe9);
    const int32_t back = static_cast<int32_t>((method + SOS_PROMPT_DIALOG_ENTRY_BYTES) - (stub + expected.size() + 4));
    expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&back), reinterpret_cast<const uint8_t*>(&back) + 4);
    Check(std::memcmp(stub, expected.data(), expected.size()) == 0, name + ": the stub saves the argument registers, calls the handler, restores them, replays the entry and jumps back");
    Check(std::all_of(stub + expected.size(), stub + SOS_PROMPT_DIALOG_STUB_BYTES, [](uint8_t b) { return b == 0; }) && expected.size() <= SOS_PROMPT_DIALOG_STUB_BYTES,
          name + ": the stub fits its area and writes nothing past itself");
    Check(flag >= reinterpret_cast<uint64_t>(image.data()) && flag < reinterpret_cast<uint64_t>(image.data()) + size, name + ": the rebuilt compare reads the method's own class flag inside the image");

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t at = 0;
    int calls = 0, pushes = 0, pops = 0;
    uint64_t jumpTarget = 0;
    while (at < expected.size()) {
        Decoded d;
        if (!Decode(decoder, stub + at, expected.size() - at, d)) break;
        calls += d.instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
        pushes += d.instruction.mnemonic == ZYDIS_MNEMONIC_PUSH;
        pops += d.instruction.mnemonic == ZYDIS_MNEMONIC_POP;
        if (d.instruction.mnemonic == ZYDIS_MNEMONIC_JMP) jumpTarget = reinterpret_cast<uint64_t>(stub + at + d.instruction.length) + static_cast<int64_t>(d.operands[0].imm.value.s);
        at += d.instruction.length;
    }
    Check(at == expected.size(), name + ": the stub decodes to whole instructions");
    Check(calls == 1 && pushes == 11 && pops == 6, name + ": the stub has one call, six argument registers saved and restored, and the five replayed pushes");
    Check(jumpTarget == reinterpret_cast<uint64_t>(method + SOS_PROMPT_DIALOG_ENTRY_BYTES), name + ": the stub returns to the instruction after the stolen bytes");
    Check(!sos_prompt_dialog_verify(image.data(), size, key, &why), name + ": a hooked method no longer verifies, so a second install refuses");
    Image twice = image;
    Check(sos_prompt_dialog_install(twice.data(), size, key, twice.data() + size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES, SOS_PROMPT_DIALOG_STUB_BYTES, message, sizeof(message)) == 0 &&
              twice == image,
          name + ": installing twice refuses and writes nothing");

    for (const SosPromptDialogRange& range : site.ranges) {
        for (const uint32_t offset : {range.begin, (range.begin + range.end) / 2, range.end - 1}) {
            Image damaged = original;
            damaged[offset] ^= 0x5a;
            const Image before = damaged;
            Check(sos_prompt_dialog_install(damaged.data(), size, key, damaged.data() + size, 128, message, sizeof(message)) == 0 && damaged == before,
                  name + ": a changed byte at 0x" + std::to_string(offset) + " refuses and writes nothing");
        }
    }
    Image blank(size + kStubRoom, 0);
    Check(!sos_prompt_dialog_verify(blank.data(), size, key, &why) && !sos_prompt_dialog_verify(image.data(), site.method + 8, key, &why) &&
              !sos_prompt_dialog_verify(image.data(), size, "CUSA57767-9.99", &why) && !sos_prompt_dialog_verify(image.data(), size, nullptr, &why),
          name + ": a blank, truncated or unknown image refuses");

    // Every other patch of a launch, in the loader's order: the static Xbox sites or the Automatic hook, Skip Intro, then the dialog hook.
    const SosPromptHook* hook = sos_controller_prompts_hook(key);
    const SosSkipIntroSite* skip = sos_skip_intro_site(key);
    Check(hook != nullptr && skip != nullptr, name + ": the Automatic hook and Skip Intro have sites in this build");
    if (hook == nullptr || skip == nullptr) return;
    const uint64_t begin = site.method, end = uint64_t{site.method} + SOS_PROMPT_DIALOG_ENTRY_BYTES;
    const auto outside = [&](uint64_t from, uint64_t to) { return to <= begin || from >= end; };
    Check(outside(hook->method, uint64_t{hook->method} + hook->length), name + ": the Automatic prompt hook's method does not overlap the dialog hook");
    for (const uint64_t at_skip : {uint64_t{skip->coroutine}, uint64_t{skip->init_save_system}, uint64_t{skip->task_delay}, uint64_t{skip->task_delay_other}, uint64_t{skip->load_save_task},
                                   uint64_t{skip->check_file_exist}, uint64_t{skip->title_check}, uint64_t{skip->base_tick}}) {
        Check(at_skip < site.ranges[0].begin || at_skip >= site.ranges[0].end, name + ": Skip Intro site " + std::to_string(at_skip) + " is outside the dialog method");
    }
    size_t sites = 0;
    const SosPromptSite* statics = sos_controller_prompts_sites(key, &sites);
    bool staticsApart = statics != nullptr && sites == 14;
    for (size_t i = 0; statics != nullptr && i < sites; ++i) {
        for (const SosPromptDialogRange& range : site.ranges) staticsApart &= statics[i].offset + statics[i].length <= range.begin || statics[i].offset >= range.end;
    }
    Check(staticsApart, name + ": none of the 14 static Xbox prompt sites lies in the code the dialog hook hashes");

    struct Mode {
        int style;
        bool skip;
    };
    for (const Mode mode : {Mode{SOS_PROMPTS_XBOX, false}, Mode{SOS_PROMPTS_AUTOMATIC, false}, Mode{SOS_PROMPTS_XBOX, true}, Mode{SOS_PROMPTS_AUTOMATIC, true}}) {
        const std::string label = name + ": " + (mode.style == SOS_PROMPTS_XBOX ? "Xbox" : "Automatic") + " prompts, Skip Intro " + (mode.skip ? "on" : "off");
        Image combined = original;
        char note[240];
        bool applied = true;
        if (mode.style == SOS_PROMPTS_XBOX) applied &= sos_controller_prompts_apply(combined.data(), size, key, SOS_PROMPTS_XBOX, note, sizeof(note)) == SOS_PROMPTS_XBOX;
        if (mode.skip) applied &= sos_skip_intro_apply(combined.data(), size, key, combined.data() + size, SOS_SKIP_INTRO_STUB_BYTES, note, sizeof(note)) == 1;
        if (mode.style == SOS_PROMPTS_AUTOMATIC) {
            applied &= sos_prompt_hook_install(combined.data(), size, key, combined.data() + size + SOS_SKIP_INTRO_STUB_BYTES, SOS_PROMPT_HOOK_STUB_BYTES, note, sizeof(note)) == 1;
        }
        Check(applied, label + ": the other patches apply");
        const Image others = combined;
        Check(sos_prompt_dialog_verify(combined.data(), size, key, &why), label + ": the dialog code still verifies on the patched image");
        Check(sos_prompt_dialog_install(combined.data(), size, key, combined.data() + size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES, SOS_PROMPT_DIALOG_STUB_BYTES, note, sizeof(note)) == 1,
              label + ": the dialog hook installs after them");
        const std::set<uint64_t> dialogBytes = Changed(others, combined, size);
        Check(dialogBytes == changed, label + ": it writes exactly the same 18 bytes as alone");
        const Image dialogFirst = [&] {
            Image reversed = original;
            sos_prompt_dialog_install(reversed.data(), size, key, reversed.data() + size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES, SOS_PROMPT_DIALOG_STUB_BYTES, note, sizeof(note));
            return reversed;
        }();
        Check(sos_skip_intro_verify(dialogFirst.data(), size, skip, &why), label + ": Skip Intro still verifies on an image with the dialog hook");
        Check(sos_prompt_hook_verify(dialogFirst.data(), size, key, &why), label + ": the Automatic hook still verifies on an image with the dialog hook");
        Check(sos_controller_prompts_state(dialogFirst.data(), size, key) == SOS_PROMPTS_PLAYSTATION, label + ": the static prompt sites still read as the game has them");
        const Image& areas = combined;
        const auto emptyArea = [&](uint64_t from, uint64_t count) {
            return std::all_of(areas.begin() + static_cast<std::ptrdiff_t>(size + from), areas.begin() + static_cast<std::ptrdiff_t>(size + from + count), [](uint8_t b) { return b == 0; });
        };
        Check(mode.skip ? !emptyArea(0, SOS_SKIP_INTRO_STUB_BYTES) : emptyArea(0, SOS_SKIP_INTRO_STUB_BYTES), label + ": Skip Intro's area is used only by Skip Intro");
        Check(!emptyArea(SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES, SOS_PROMPT_DIALOG_STUB_BYTES), label + ": the dialog stub is written in its own area");
        Check(mode.style == SOS_PROMPTS_AUTOMATIC ? !emptyArea(SOS_SKIP_INTRO_STUB_BYTES, SOS_PROMPT_HOOK_STUB_BYTES) : emptyArea(SOS_SKIP_INTRO_STUB_BYTES, SOS_PROMPT_HOOK_STUB_BYTES),
              label + ": the Automatic hook's area is used only by the Automatic hook");
    }

    // The red-zone pass runs before the hook and rewrites only functions flagged red or INSERTQ: none of the functions the hook relies on
    // is, so the pass leaves their bytes (and the hashes) alone, on the pristine image and on the hooked one.
    for (const bool hooked : {false, true}) {
        RzaImage analysis{};
        const Image& current = hooked ? image : original;
        Check(rza_enumerate(current.data(), size, boot.segments.data(), boot.segments.size() / 3, &analysis) != 0, name + ": the red-zone analysis reads the " + (hooked ? "hooked" : "pristine") + " image");
        if (analysis.functions == nullptr) continue;
        Check(rza_scan(current.data(), size, &analysis, 1, nullptr, 0) >= 0, name + ": the red-zone analysis scans the " + (hooked ? "hooked" : "pristine") + " image");
        for (const SosPromptDialogRange& range : site.ranges) {
            const RzaFunction* found = nullptr;
            for (size_t i = 0; i < analysis.count; ++i) {
                if (analysis.functions[i].begin <= range.begin && range.begin < analysis.functions[i].end) found = &analysis.functions[i];
            }
            char where[16];
            std::snprintf(where, sizeof(where), "%x", range.begin);
            Check(found != nullptr, name + ": the red-zone analysis knows the function at 0x" + where);
            if (found == nullptr) continue;
            Check((found->flags & (RZA_RED | RZA_INSERTQ)) == 0, name + ": the red-zone pass has no rewrite for the function at 0x" + where + " (flags " + std::to_string(found->flags) + ")");
        }
        rza_free(&analysis);
    }
}

uint8_t* MenuStub(Image& image, uint64_t size, int menu) {
    return image.data() + size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES + SOS_PROMPT_DIALOG_STUB_BYTES + static_cast<uint64_t>(menu) * SOS_PROMPT_MENU_STUB_BYTES;
}

void TestMenuHookReal(const char* key, const fs::path& boot_path) {
    const SosPromptDialogSite& site = *sos_prompt_dialog_site(key);
    Boot boot;
    if (!ReadBoot(boot_path, boot)) return;
    Image& image = boot.image;
    const uint64_t size = boot.size;
    const Image original = image;
    const char* why = "";
    char message[200];
    const std::string other = std::string(key) == kBuilds[0] ? kBuilds[1] : kBuilds[0];
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    for (int m = 0; m < SOS_PROMPT_MENUS; ++m) {
        const SosPromptMenu& menu = site.menus[m];
        const std::string name = std::string(key) + ": " + kMenuNames[m] + ": ";
        image = original;
        Check(sos_prompt_menu_verify(image.data(), size, key, m, &why), name + "the real method and the code its handler calls are the verified code (every byte hashed)");
        Check(!sos_prompt_menu_verify(image.data(), size, other.c_str(), m, &why), name + "the other build's hook does not match this code");
        Check(menu.method != sos_prompt_dialog_site(other.c_str())->menus[m].method, name + "the method address is this build's own");

        const uint8_t* method = image.data() + menu.method;
        std::vector<size_t> lengths;
        size_t at = 0;
        bool position_independent = true;
        while (at < SOS_PROMPT_MENU_STOLEN) {
            Decoded d;
            if (!Decode(decoder, method + at, menu.length - at, d)) break;
            const auto category = d.instruction.meta.category;
            if ((d.instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) || category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_UNCOND_BR || category == ZYDIS_CATEGORY_COND_BR ||
                category == ZYDIS_CATEGORY_RET || std::any_of(d.operands, d.operands + d.instruction.operand_count, [](const ZydisDecodedOperand& op) { return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP; }))
                position_independent = false;
            lengths.push_back(d.instruction.length);
            at += d.instruction.length;
        }
        Check(at == SOS_PROMPT_MENU_STOLEN && lengths == std::vector<size_t>({1, 3, 2}) && position_independent, name + "the 6 stolen bytes are push, mov and push with nothing relative");
        std::vector<uint64_t> targets;
        size_t decoded = 0;
        for (at = 0; at < menu.length;) {
            Decoded d;
            if (!Decode(decoder, method + at, menu.length - at, d)) break;
            if (d.instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
                for (int k = 0; k < d.instruction.operand_count_visible; ++k) {
                    if (d.operands[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.operands[k].imm.is_relative) targets.push_back(at + d.instruction.length + static_cast<int64_t>(d.operands[k].imm.value.s));
                }
            }
            at += d.instruction.length;
            ++decoded;
            if (d.instruction.meta.category == ZYDIS_CATEGORY_RET && at + 16 >= menu.length) break;
        }
        Check(decoded > 80, name + "the method decodes (" + std::to_string(decoded) + " instructions swept)");
        Check(std::none_of(targets.begin(), targets.end(), [](uint64_t target) { return target > 0 && target < SOS_PROMPT_MENU_STOLEN; }), name + "no branch of the method lands inside the stolen bytes");

        uint8_t* stub = MenuStub(image, size, m);
        Check(sos_prompt_menu_install(image.data(), size, key, m, stub, 20, message, sizeof(message)) == 0 && image == original, name + "a stub area that is too small refuses and writes nothing");
        Check(sos_prompt_menu_install(image.data(), size, key, m, image.data() + size + (uint64_t{1} << 32), SOS_PROMPT_MENU_STUB_BYTES, message, sizeof(message)) == 0 && image == original,
              name + "a stub out of 32-bit reach refuses and writes nothing");
        Check(sos_prompt_menu_install(image.data(), size, key, m, stub, SOS_PROMPT_MENU_STUB_BYTES, message, sizeof(message)) == 1, name + "the hook installs: " + message);
        const std::set<uint64_t> changed = Changed(original, image, size);
        Check(!changed.empty() && *changed.begin() >= menu.method && *changed.rbegin() < uint64_t{menu.method} + SOS_PROMPT_MENU_STOLEN, name + "only the method's first 6 bytes of the image change");
        int32_t rel = 0;
        const uint8_t* hooked = image.data() + menu.method;
        std::memcpy(&rel, hooked + 1, 4);
        Check(hooked[0] == 0xe9 && hooked + 5 + rel == stub && hooked[5] == 0x90, name + "the method starts with a jump to the stub and a nop up to the resume address");

        std::vector<uint8_t> expected(kExpectedHead, kExpectedHead + sizeof(kExpectedHead));
        const uint64_t handler = reinterpret_cast<uint64_t>(m == 0 ? &sos_prompt_menu_handler_save : &sos_prompt_menu_handler_pit);
        expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&handler), reinterpret_cast<const uint8_t*>(&handler) + 8);
        expected.insert(expected.end(), kExpectedTail, kExpectedTail + sizeof(kExpectedTail));
        expected.insert(expected.end(), original.begin() + menu.method, original.begin() + menu.method + SOS_PROMPT_MENU_STOLEN);
        expected.push_back(0xe9);
        const int32_t back = static_cast<int32_t>((hooked + SOS_PROMPT_MENU_STOLEN) - (stub + expected.size() + 4));
        expected.insert(expected.end(), reinterpret_cast<const uint8_t*>(&back), reinterpret_cast<const uint8_t*>(&back) + 4);
        Check(std::memcmp(stub, expected.data(), expected.size()) == 0, name + "the stub saves the argument registers, calls this menu's handler, restores them, replays the 6 bytes and jumps back");
        Check(std::all_of(stub + expected.size(), stub + SOS_PROMPT_MENU_STUB_BYTES, [](uint8_t b) { return b == 0; }) && expected.size() <= SOS_PROMPT_MENU_STUB_BYTES, name + "the stub fits its area and writes nothing past itself");
        at = 0;
        int calls = 0, pushes = 0, pops = 0;
        uint64_t jumpTarget = 0;
        while (at < expected.size()) {
            Decoded d;
            if (!Decode(decoder, stub + at, expected.size() - at, d)) break;
            calls += d.instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
            pushes += d.instruction.mnemonic == ZYDIS_MNEMONIC_PUSH;
            pops += d.instruction.mnemonic == ZYDIS_MNEMONIC_POP;
            if (d.instruction.mnemonic == ZYDIS_MNEMONIC_JMP) jumpTarget = reinterpret_cast<uint64_t>(stub + at + d.instruction.length) + static_cast<int64_t>(d.operands[0].imm.value.s);
            at += d.instruction.length;
        }
        Check(at == expected.size() && calls == 1 && pushes == 8 && pops == 6, name + "the stub decodes to one call, six argument registers saved and restored and the two replayed pushes");
        Check(jumpTarget == reinterpret_cast<uint64_t>(hooked + SOS_PROMPT_MENU_STOLEN), name + "the stub returns to the instruction after the stolen bytes");
        Check(!sos_prompt_menu_verify(image.data(), size, key, m, &why), name + "a hooked method no longer verifies, so a second install refuses");
        Image twice = image;
        Check(sos_prompt_menu_install(twice.data(), size, key, m, MenuStub(twice, size, m), SOS_PROMPT_MENU_STUB_BYTES, message, sizeof(message)) == 0 && twice == image, name + "installing twice refuses and writes nothing");

        struct Part {
            const char* what;
            uint32_t begin, end;
        };
        const Part parts[] = {{"the method", menu.method, menu.method + menu.length},
                              {"get_overseer", site.ranges[2].begin, site.ranges[2].end},
                              {"set_override_sprite", site.ranges[3].begin, site.ranges[3].end}};
        for (const Part& part : parts) {
            for (const uint32_t offset : {part.begin, (part.begin + part.end) / 2, part.end - 1}) {
                Image damaged = original;
                damaged[offset] ^= 0x5a;
                const Image before = damaged;
                Check(sos_prompt_menu_install(damaged.data(), size, key, m, MenuStub(damaged, size, m), SOS_PROMPT_MENU_STUB_BYTES, message, sizeof(message)) == 0 && damaged == before,
                      name + "a changed byte of " + part.what + " at 0x" + std::to_string(offset) + " refuses and writes nothing");
            }
        }
        Image blank(size + kStubRoom, 0);
        Check(!sos_prompt_menu_verify(blank.data(), size, key, m, &why) && !sos_prompt_menu_verify(image.data(), menu.method + 8, key, m, &why) &&
                  !sos_prompt_menu_verify(image.data(), size, "CUSA57767-9.99", m, &why) && !sos_prompt_menu_verify(image.data(), size, nullptr, m, &why),
              name + "a blank, truncated or unknown image refuses");

        // The loader's order: the other prompt patches, the dialog hook, then both menus; each writes its own bytes and area.
        const SosPromptHook* hook = sos_controller_prompts_hook(key);
        const SosSkipIntroSite* skip = sos_skip_intro_site(key);
        Check(hook != nullptr && skip != nullptr, name + "the Automatic hook and Skip Intro have sites in this build");
        if (hook == nullptr || skip == nullptr) continue;
        for (const int style : {SOS_PROMPTS_XBOX, SOS_PROMPTS_AUTOMATIC}) {
            const std::string label = name + (style == SOS_PROMPTS_XBOX ? "Xbox" : "Automatic") + " prompts, Skip Intro on: ";
            Image combined = original;
            char note[240];
            bool applied = sos_skip_intro_apply(combined.data(), size, key, combined.data() + size, SOS_SKIP_INTRO_STUB_BYTES, note, sizeof(note)) == 1;
            if (style == SOS_PROMPTS_XBOX) applied &= sos_controller_prompts_apply(combined.data(), size, key, SOS_PROMPTS_XBOX, note, sizeof(note)) == SOS_PROMPTS_XBOX;
            else applied &= sos_prompt_hook_install(combined.data(), size, key, combined.data() + size + SOS_SKIP_INTRO_STUB_BYTES, SOS_PROMPT_HOOK_STUB_BYTES, note, sizeof(note)) == 1;
            applied &= sos_prompt_dialog_install(combined.data(), size, key, combined.data() + size + SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES, SOS_PROMPT_DIALOG_STUB_BYTES, note, sizeof(note)) == 1;
            Check(applied, label + "the other patches and the dialog hook apply");
            const Image others = combined;
            Check(sos_prompt_menu_verify(combined.data(), size, key, m, &why), label + "the menu code still verifies after them");
            bool installed = true;
            for (int each = 0; each < SOS_PROMPT_MENUS; ++each) installed &= sos_prompt_menu_install(combined.data(), size, key, each, MenuStub(combined, size, each), SOS_PROMPT_MENU_STUB_BYTES, note, sizeof(note)) == 1;
            Check(installed, label + "both menu hooks install after them");
            const std::set<uint64_t> menuBytes = Changed(others, combined, size);
            Check(!menuBytes.empty() && std::all_of(menuBytes.begin(), menuBytes.end(), [&](uint64_t byte) {
                      for (const SosPromptMenu& each : site.menus) {
                          if (byte >= each.method && byte < uint64_t{each.method} + SOS_PROMPT_MENU_STOLEN) return true;
                      }
                      return false;
                  }),
                  label + "they write only the first 6 bytes of each menu method");
            const auto emptyArea = [&](uint64_t from, uint64_t count) {
                return std::all_of(combined.begin() + static_cast<std::ptrdiff_t>(size + from), combined.begin() + static_cast<std::ptrdiff_t>(size + from + count), [](uint8_t b) { return b == 0; });
            };
            Check(!emptyArea(SOS_SKIP_INTRO_STUB_BYTES + SOS_PROMPT_HOOK_STUB_BYTES + SOS_PROMPT_DIALOG_STUB_BYTES + static_cast<uint64_t>(m) * SOS_PROMPT_MENU_STUB_BYTES, SOS_PROMPT_MENU_STUB_BYTES),
                  label + "the menu stub is written in its own area");
            Image menusFirst = original;
            for (int each = 0; each < SOS_PROMPT_MENUS; ++each) sos_prompt_menu_install(menusFirst.data(), size, key, each, MenuStub(menusFirst, size, each), SOS_PROMPT_MENU_STUB_BYTES, note, sizeof(note));
            Check(sos_prompt_dialog_verify(menusFirst.data(), size, key, &why) && sos_skip_intro_verify(menusFirst.data(), size, skip, &why) && sos_prompt_hook_verify(menusFirst.data(), size, key, &why) &&
                      sos_controller_prompts_state(menusFirst.data(), size, key) == SOS_PROMPTS_PLAYSTATION,
                  label + "the dialog hook, Skip Intro, the Automatic hook and the static sites still verify on an image with the menu hooks");
        }
    }
    size_t sites = 0;
    const SosPromptSite* statics = sos_controller_prompts_sites(key, &sites);
    bool apart = statics != nullptr && sites == 14;
    for (size_t i = 0; statics != nullptr && i < sites; ++i) {
        for (const SosPromptMenu& menu : site.menus) apart &= statics[i].offset + statics[i].length <= menu.method || statics[i].offset >= uint64_t{menu.method} + menu.length;
    }
    Check(apart, std::string(key) + ": none of the 14 static Xbox prompt sites lies in a hooked menu method");

    for (const bool hooked : {false, true}) {
        RzaImage analysis{};
        Image current = original;
        if (hooked) {
            for (int m = 0; m < SOS_PROMPT_MENUS; ++m) sos_prompt_menu_install(current.data(), size, key, m, MenuStub(current, size, m), SOS_PROMPT_MENU_STUB_BYTES, message, sizeof(message));
        }
        const std::string name = std::string(key) + ": menus: ";
        Check(rza_enumerate(current.data(), size, boot.segments.data(), boot.segments.size() / 3, &analysis) != 0, name + "the red-zone analysis reads the " + (hooked ? "hooked" : "pristine") + " image");
        if (analysis.functions == nullptr) continue;
        Check(rza_scan(current.data(), size, &analysis, 1, nullptr, 0) >= 0, name + "the red-zone analysis scans the " + (hooked ? "hooked" : "pristine") + " image");
        for (const SosPromptMenu& menu : site.menus) {
            const RzaFunction* found = nullptr;
            for (size_t i = 0; i < analysis.count; ++i) {
                if (analysis.functions[i].begin <= menu.method && menu.method < analysis.functions[i].end) found = &analysis.functions[i];
            }
            Check(found != nullptr && (found->flags & (RZA_RED | RZA_INSERTQ)) == 0, name + "the red-zone pass has no rewrite for the menu method at 0x" + std::to_string(menu.method));
        }
        rza_free(&analysis);
    }
}

void TestRealImages() {
    const fs::path root = SOS_SOURCE_DIR;
    for (const char* key : {"CUSA57767-1.00", "CUSA57767-1.01"}) {
        const sos_test::BootImage boot = sos_test::ReportImage(root, key, "the dialog and menu hooks");
        if (!boot.found()) continue;
        TestHookReal(key, boot.path);
        TestMenuHookReal(key, boot.path);
    }
}

}  // namespace

int main() {
    for (const char* key : kBuilds) {
        TestTable(key);
        TestHandler(key);
        TestMenuTable(key);
        TestMenuHandler(key);
    }
    if (std::getenv("SOS_TEST_FAST") == nullptr) TestRealImages();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
