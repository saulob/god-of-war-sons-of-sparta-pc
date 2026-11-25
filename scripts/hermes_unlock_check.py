"""Static proof of the Hermes Mode early unlock from the game's IL2CPP metadata and boot images.

    python scripts/hermes_unlock_check.py

Per installed build: the game has a global isHermesModeUnlocked flag next to hasBeatenGame, the game-mode selection
tests those two bytes in one verified block, the patch site is the branch that hides the original Hermes button, the
selected mode is written to and read from GameSaveDataV2.currentGameMode, and a save with mode 8 starts the game's own
speedrun. The launcher and loader default unlock_hermes_mode to off. Nothing is executed; exit status 0 when every
check passes.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from il2cpp_index import open_reported, relative_calls  # noqa: E402

STATE = "StateMachineStateTypes.UI.MainMenu.MainMenuState_GameModeSelection"
LOAD_GAME = "StateMachineStateTypes.UI.SaveLoadMenu.SaveLoadMenuState_LoadGame"
GATE = bytes.fromhex("807f4000750a807f41000f8405010000")
HERMES_MODE = 8
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def sites():
    text = (ROOT / "src/sos_hermes_unlock.c").read_text()
    return {version: (int(size), int(gate, 16), int(hide, 16))
            for version, size, gate, hide in re.findall(
                r'\{"CUSA57767-([\d.]+)", (\d+)u, (0x[0-9a-f]+), (0x[0-9a-f]+)\}', text)}


def body(index, type_name, method):
    start = index.method_address(type_name, method)
    end = index.method_bounds(start)[1]
    return start, bytes(index.boot.read(start, end - start))


def check_build(version, site):
    print("CUSA57767-" + version)
    index = open_reported(version, "the Hermes Mode site")
    if index is None:
        return
    index.all_method_names()
    size, gate, hide = site
    check(size == index.boot.size, "the site's image size is the real boot image size")
    check(index.field_offset("GlobalSaveDataV1", "hasBeatenGame") == 0x40, "hasBeatenGame is at +0x40")
    check(index.field_offset("GlobalSaveDataV1", "isHermesModeUnlocked") == 0x41, "isHermesModeUnlocked is at +0x41")

    start, selection = body(index, STATE, "CheckSpeedRunModePossible")
    check(start <= gate - 10 and gate + 6 <= start + len(selection), "the patch site is inside CheckSpeedRunModePossible")
    check(selection[gate - 10 - start:gate + 6 - start] == GATE, "the site tests hasBeatenGame then isHermesModeUnlocked")
    check(gate + 6 + int.from_bytes(GATE[12:16], "little", signed=True) == hide, "the je branches to the hidden-button path")

    module = max(index.boot.modules, key=lambda m: m[1])
    image = bytes(index.boot.read(module[0], module[1]))
    check(image.count(GATE) == 1, "the verified two-flag gate occurs exactly once in the game code")

    selected_start, selected = body(index, "MenuSystem.Main.GameModeButton", "OnSubmit")
    submits = relative_calls(selected, selected_start, {index.method_address(STATE, "OnSelectedGameMode")})
    check(bytes.fromhex("8b7350") in selected and len(submits) == 1,
          "the button submits its own serialized gameMode value to OnSelectedGameMode")
    check(bytes.fromhex("4489b7c0000000") in body(index, STATE, "OnSelectedGameMode")[1],
          "selecting a mode writes currentGameMode (+0xc0) through the game's own handler")
    check(bytes.fromhex("8b87c0000000") in body(index, "SaveUtils", "GetCurrentGameMode")[1],
          "the current game mode is read back from currentGameMode (+0xc0)")

    load_start, load = body(index, LOAD_GAME, "OnLoadedNewGameSave")
    calls = relative_calls(load, load_start, {index.method_address("GameMode", "IsInMode")})
    check(len(calls) == 1 and load[calls[0][0] - load_start - 7:calls[0][0] - load_start] == bytes.fromhex("bf0800000031f6"),
          "loading a save calls GameMode.IsInMode(8): Hermes is the speedrun mode")

    unlocks = []
    for type_index in index.type_indices("MenuSystem.PitModeInputSequence"):
        for method_index, method in index.type_methods(type_index):
            if index.meta.string(method[0]) == "GrantHermesUnlocks":
                unlocks.append(method_index)
    writes = []
    for method_index in unlocks:
        at = index._address(method_index)
        end = index.method_bounds(at)[1]
        if b"\xc6\x46\x41\x01" in bytes(index.boot.read(at, end - at)):
            writes.append(method_index)
    check(len(unlocks) == 2 and len(writes) == 1, "the cheat code's GrantHermesUnlocks sets isHermesModeUnlocked")
    check(index.method_address("MenuSystem.PitModeInputSequence", "OnTouchPad") != 0, "the original sequence input path is present")


def check_settings():
    header = (ROOT / "src/sos_hermes_unlock.h").read_text()
    source = (ROOT / "src/sos_hermes_unlock.c").read_text()
    settings_h = (ROOT / "launcher/settings.h").read_text()
    settings_cpp = (ROOT / "launcher/settings.cpp").read_text()
    launcher = (ROOT / "launcher/launcher.cpp").read_text()
    loader = (ROOT / "src/loader.c").read_text()
    check('#define SOS_HERMES_UNLOCK_KEY "unlock_hermes_mode"' in header, "the shared setting name is unlock_hermes_mode")
    check("int on = 0;" in source and "if (!file) return 0;" in source, "the loader default is off")
    check("kUnlockHermesModeKey[] = SOS_HERMES_UNLOCK_KEY" in settings_h, "the launcher uses the shared setting name")
    check("{kUnlockHermesModeKey, \"0\"}" in settings_cpp, "the launcher default is off")
    check('GetBool(kUnlockHermesModeKey, false)' in settings_cpp, "the settings summary reads the default off")
    toggle = 'ui.Toggle("Unlock Hermes Mode", kUnlockHermesModeHelp, kUnlockHermesModeKey, false, "Off", "On")'
    check(toggle in launcher, "Gameplay has the Unlock Hermes Mode toggle, default Off")
    skip = launcher.find('ui.Toggle("Skip Intro"')
    hermes = launcher.find('ui.Toggle("Unlock Hermes Mode"')
    overlay = launcher.find('ui.Toggle("Allow overlay programs"')
    check(0 <= skip < hermes < overlay, "Hermes follows Skip Intro and precedes overlay programs")
    check("sos_hermes_unlock_from_file(settings_file)" in loader, "the loader reads unlock_hermes_mode")
    check("if (hermes_unlock_wanted)" in loader, "the loader applies the early unlock only when it is on")


def main():
    table = sites()
    supported = {"1.00", "1.01"}
    check_settings()
    check(set(table) == supported, "the site table covers CUSA57767 1.00 and 1.01 only")
    for version in sorted(supported):
        if version in table:
            check_build(version, table[version])
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
