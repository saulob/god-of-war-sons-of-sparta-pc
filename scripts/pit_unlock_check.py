"""Static proof of The Pit early unlock from the game's IL2CPP metadata, boot images, scenes and packages' prepared files.

    python scripts/pit_unlock_check.py

Per installed build: isPitUnlocked and isHermesModeUnlocked are separate profile flags, the title menu tests each before
its own event, the patch site is the branch over The Pit's event, the cheat code grants both flags separately, nothing but
the title menu reads isPitUnlocked, the event shows the title menu's own ThePitButton, the button loads PitMode, every Pit
scene is in the build and on disk, and the menu with The Pit and Exit Game rows fits. Nothing is executed; exit status 0
when every check passes.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import prompt_assets_check as pac  # noqa: E402
from il2cpp_index import metadata_path, open_build, relative_calls  # noqa: E402

SEQUENCE = "MenuSystem.PitModeInputSequence"
COROUTINE = "<Coroutine_SubscribeToInputEvents>d__15"
TITLE = "StateMachineStateTypes.UI.MainMenu.MainMenuState_TransitionToButtons"
SETTINGS = "Settings.GeneralSettingsUtils"
CHECK = bytes.fromhex("440fb6771e440fb67f414584f67413488bbbe00000004885ff740731f6e8")
PIT_SETTING = 0x59
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def site_table():
    text = (ROOT / "src/sos_pit_unlock.c").read_text()
    return {version: (int(size), int(gate, 16), int(invoke, 16))
            for version, size, gate, invoke in re.findall(
                r'\{"CUSA57767-([\d.]+)", (\d+)u, (0x[0-9a-f]+), (0x[0-9a-f]+)\}', text)}


def scene_ranges():
    text = (ROOT / "src/sos_pit_unlock.c").read_text()
    return [(int(a), int(b)) for a, b in re.findall(r"\{(\d+), (\d+)\}", re.search(r"kScenes\[\] = \{(.*?)\};", text).group(1))]


def methods(index, type_name, method_name):
    found = []
    for type_index in index.type_indices(type_name):
        for method_index, method in index.type_methods(type_index):
            address = index._address(method_index)
            if address and index.meta.string(method[0]) == method_name:
                found.append((address, bytes(index.boot.read(address, index.method_bounds(address)[1] - address))))
    return found


def check_code(index, size, gate, invoke):
    check(size == index.boot.size, "the site's image size is the real boot image size")
    offsets = {name: index.field_offset("GlobalSaveDataV1", name) for name in ("isPitUnlocked", "hasBeatenGame", "isHermesModeUnlocked")}
    check(offsets == {"isPitUnlocked": 0x1e, "hasBeatenGame": 0x40, "isHermesModeUnlocked": 0x41}, "the three unlock flags are separate bytes")
    check(index.field_offset(SEQUENCE, "onInputSequencePitUnlocks") == 0xe0 and index.field_offset(SEQUENCE, "onInputSequenceHermesUnlocks") == 0xe8,
          "The Pit and Hermes Mode have separate events")
    start = index.method_address(COROUTINE, "MoveNext")
    end = index.method_bounds(start)[1]
    code = bytes(index.boot.read(start, end - start))
    check(start <= gate - 13 and code[gate - 13 - start:gate - 13 - start + len(CHECK)] == CHECK,
          "the site tests isPitUnlocked before The Pit's event in the title menu coroutine")
    check(invoke == index.method_address("UnityEngine.Events.UnityEvent", "Invoke", "UnityEngine.CoreModule.dll"),
          "the event call is UnityEvent.Invoke")
    module = max(index.boot.modules, key=lambda m: m[1])
    check(bytes(index.boot.read(module[0], module[1])).count(CHECK) == 1, "the verified check occurs exactly once in the game code")
    targets = {start + m.start() + 2 + struct.unpack_from("b", code, m.start() + 1)[0] for m in re.finditer(rb"[\x70-\x7f\xeb]", code[:-1])}
    targets |= {start + m.start() + 6 + struct.unpack_from("<i", code, m.start() + 2)[0] for m in re.finditer(rb"\x0f[\x80-\x8f]", code[:-5])}
    check(gate + 1 not in targets, "nothing branches into the patched instruction")

    low, game = index.game_code()
    register = methods(index, SEQUENCE, "RegisterInput")
    grants = {a for a, _ in methods(index, SEQUENCE, "GrantPitUnlocks")} | {a for a, _ in methods(index, SEQUENCE, "GrantHermesUnlocks")}
    called = [index.method_at(t).rsplit("::", 1)[1] for a, b in register for _, t in relative_calls(b, a, grants)]
    check(called == ["GrantPitUnlocks", "GrantHermesUnlocks"], "the cheat code grants The Pit and Hermes Mode through separate calls")
    set_bool = index.method_address(SETTINGS, "SetBoolSetting")
    pit = methods(index, SEQUENCE, "GrantPitUnlocks")
    check(all(b"\xbf\x59\x00\x00\x00\xbe\x01\x00\x00\x00" in b and relative_calls(b, a, {set_bool}) and b"\xc6\x47\x41\x01" not in b
              and b"\xc6\x46\x41\x01" not in b for a, b in pit) and len(pit) == 2, "granting The Pit sets only its own setting")
    get_bool = index.method_address(SETTINGS, "GetBoolSetting")
    readers = [a for a, _ in relative_calls(game, low, {get_bool}) if game[a - low - 12:a - low].find(struct.pack("<BI", 0xbf, PIT_SETTING)) >= 0]
    check(not readers, "no game code asks for the isPitUnlocked setting, so only the title menu reads the flag")
    complete = methods(index, "<Coroutine_SetCompletedGameOnce>d__2", "MoveNext")
    check(len(complete) == 1 and bytes.fromhex("66c747400101") in complete[0][1] and b"\xc6\x47\x1e\x01" in complete[0][1],
          "completing the story sets hasBeatenGame, isHermesModeUnlocked and isPitUnlocked")
    check(index.method_address(TITLE, "LoadPitMode") != 0, "the title menu can load The Pit")
    trophies = methods(index, "Subsystems.TrophySubsystem", "CanTriggerTrophyUnlocks")
    pit_class = {struct.unpack_from("<i", b, m.start() + 3)[0] + a + m.start() + 7
                 for a, b in methods(index, "SaveSystem", "CreatePitSaveDataThenSave") for m in re.finditer(rb"\x48\x8b\x3d", b)}
    uses = {struct.unpack_from("<i", b, m.start() + 3)[0] + a + m.start() + 7 for a, b in trophies for m in re.finditer(rb"\x48\x8b\x3d", b)}
    check(len(trophies) == 1 and bool(uses & pit_class), "trophy unlocks check PitModeManager")


def names(data, objects):
    found = {}
    for path_id, (offset, _size, class_id) in objects.items():
        if class_id == 1:
            count = struct.unpack_from("<i", data, offset)[0]
            at = offset + 4 + 12 * count + 4
            length = struct.unpack_from("<i", data, at)[0]
            end = (at + 4 + length + 3) & ~3
            components = [struct.unpack_from("<iq", data, offset + 4 + 12 * k)[1] for k in range(count)]
            found[path_id] = (bytes(data[at + 4:at + 4 + length]).decode("utf-8", "replace"), data[end + 2], components)
    return found


def rect(data, objects, path_id):
    offset = objects[path_id][0] + 52
    count = struct.unpack_from("<i", data, offset)[0]
    children = [struct.unpack_from("<iq", data, offset + 4 + 12 * k)[1] for k in range(count)]
    return struct.unpack_from("<10f", data, offset + 4 + 12 * count + 12), children


def check_menu(media, level):
    data, objects = pac.parse(media / level)
    objects_by_name = names(data, objects)
    buttons = [p for p, (name, _, _) in objects_by_name.items() if name == "ThePitButton"]
    if not check(len(buttons) == 1, f"{level}: the title menu has one ThePitButton"):
        return
    button = buttons[0]
    check(objects_by_name[button][1] == 0, f"{level}: ThePitButton starts hidden")
    body = lambda p: bytes(data[objects[p][0]:objects[p][0] + objects[p][1]])  # noqa: E731
    shows = [p for p, (_, _, c) in objects.items() if c == 114 and p not in objects_by_name[button][2]
             and struct.pack("<iq", 0, button) in body(p)]
    owner = objects_by_name.get(struct.unpack_from("<iq", body(shows[0]), 0)[1], ("",))[0] if len(shows) == 1 else ""
    check(owner == "PitModeInputSequence" and b"SetActive" in body(shows[0]) and b"SetupNavigationOfSelectables" in body(shows[0]),
          f"{level}: only the Pit sequence's event shows ThePitButton and rebuilds the menu navigation")
    check(any(b"LoadPitMode" in body(p) for p in objects_by_name[button][2] if objects[p][2] == 114), f"{level}: ThePitButton loads The Pit")

    menu = next(p for p, (name, _, _) in objects_by_name.items() if name == "MenuChoices")
    transform = next(c for c in objects_by_name[menu][2] if objects[c][2] == 224)
    values, children = rect(data, objects, transform)
    rows = [(objects_by_name[struct.unpack_from("<iq", data, objects[c][0])[1]][0], rect(data, objects, c)[0][7]) for c in children]
    order = [name for name, _ in rows]
    check(order.index("LoadGameButton") < order.index("ThePitButton") < order.index("SettingsButton") < order.index("QuitButton"),
          f"{level}: ThePitButton has its own row between Load Game and Settings, before Exit Game")
    visible = ["ContinueButton", "NewGameButton", "LoadGameButton", "ThePitButton", "SpeedRunButton", "SettingsButton", "CreditsButton", "QuitButton"]
    bottom = values[5] + values[7] / 2 - sum(height for name, height in rows if name in visible) - 5.0 * (len(visible) - 1)
    check(all(name in order for name in visible) and bottom >= -540.0, f"{level}: the longest menu with The Pit and Exit Game fits 1080 rows (bottom {bottom:.0f})")


def check_scenes(media):
    data, objects = pac.parse(media / "globalgamemanagers")
    offset = next(o for o, _, c in objects.values() if c == 141)
    at, scenes = offset + 4, []
    for _ in range(struct.unpack_from("<i", data, offset)[0]):
        length = struct.unpack_from("<i", data, at)[0]
        scenes.append(bytes(data[at + 4:at + 4 + length]).decode())
        at = (at + 4 + length + 3) & ~3
    pit = [i for i, scene in enumerate(scenes) if "/The Pit of Agonies/" in scene or scene.endswith("/PitMode.unity")]
    listed = [i for first, last in scene_ranges() for i in range(first, last + 1)]
    unlisted = [scenes[i] for i in listed if i not in pit]
    check(set(pit) <= set(listed), f"the {len(pit)} Pit scenes of the build are all in the required files")
    check(not unlisted, "the required files name no other scene")
    base = {Path(scene).stem for scene in scenes}
    expected = {"PitMode", "PitofAgonies_Lobby", "PitofAgonies_PitStop", "PitofAgonies_Score"} | {f"PitofAgonies_BossRoom{n}" for n in range(1, 6)} \
        | {f"PitofAgonies_Room{n}" for n in range(1, 19)}
    check(expected <= base, "the lobby, rooms, boss rooms, stop, score and run scenes are in the build")
    check(all((media / f"level{i}").stat().st_size > 1000 and (media / f"sharedassets{i}.assets").is_file() for i in pit), "every Pit scene is on disk")


def check_wiring():
    header = (ROOT / "src/sos_pit_unlock.h").read_text()
    source = (ROOT / "src/sos_pit_unlock.c").read_text()
    settings_cpp = (ROOT / "launcher/settings.cpp").read_text()
    loader = (ROOT / "src/loader.c").read_text()
    check('#define SOS_PIT_UNLOCK_KEY "unlock_the_pit"' in header and "kUnlockThePitKey[] = SOS_PIT_UNLOCK_KEY" in (ROOT / "launcher/settings.h").read_text(),
          "the launcher and loader share the setting name")
    check("int on = 0;" in source and "if (!file) return 0;" in source and '{kUnlockThePitKey, "0"}' in settings_cpp, "the default is off")
    check('ui.Toggle("Unlock The Pit Early", kUnlockThePitHelp, kUnlockThePitKey, false, "Off", "On")' in (ROOT / "launcher/launcher.cpp").read_text(),
          "Gameplay has the Unlock The Pit Early toggle, default Off")
    check("sos_pit_unlock_from_file(settings_file)" in loader and re.search(
        r"if \(pit_unlock_wanted\) \{[^}]*sos_pit_unlock_apply\(image, size, prompt_key, app0,", loader) is not None,
          "the loader applies the early unlock only when it is on")
    check("src/sos_pit_unlock.c" in (ROOT / "CMakeLists.txt").read_text(), "the source is part of the build")


def main():
    table = site_table()
    check_wiring()
    check(set(table) == {"1.00", "1.01"}, "the site table covers CUSA57767 1.00 and 1.01 only")
    for version in sorted(table):
        print("CUSA57767-" + version)
        index = open_build(version)
        if index is None:
            print("  [skip] not installed: the game code and scenes were not read")
            continue
        index.all_method_names()
        check_code(index, *table[version])
        media = metadata_path(version).parent.parent
        for level in ("level3", "level8"):
            check_menu(media, level)
        check_scenes(media)
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
