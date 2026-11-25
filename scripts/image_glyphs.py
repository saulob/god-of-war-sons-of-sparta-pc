"""Glyph Images without a controller follower: generates launcher/image_glyphs_sites.h and proves, from the game's metadata, code and
assets, what the Image hooks rely on.

    python scripts/image_glyphs.py             check the installed builds against the header, the game code and the scenes
    python scripts/image_glyphs.py generate    print the header for the installed builds
    python scripts/image_glyphs.py --self-test prove that damaged inputs are rejected

Nothing is executed; a build without a boot image or metadata is skipped. Exit status 0 when every check passes.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prompt_assets_check as pac  # noqa: E402
from il2cpp_index import ROOT, open_build, open_reported  # noqa: E402
import build_evidence as evidence  # noqa: E402
from prompt_dialog import Slots, engine_method, function_end, image_field, slot_table, subsystem_body  # noqa: E402
from prompt_sites import fnv64, hexbytes  # noqa: E402

HEADER = ROOT / "launcher" / "image_glyphs_sites.h"
SOURCE = ROOT / "launcher" / "image_glyphs.c"
BUILDS = ("1.00", "1.01")
UI, CORE, GAME = "UnityEngine.UI.dll", "UnityEngine.CoreModule.dll", "Assembly-CSharp.dll"
HOOKS = (
    ("UnityEngine.UI.Image", "OnEnable", UI, 0, "554889e55350"),
    ("UnityEngine.UI.Image", "OnDisable", UI, 0, "554889e54156"),
    ("UnityEngine.UI.Image", "set_sprite", UI, 1, "554889e54157"),
    ("InputOverseer", "GetTextSpriteAsset", GAME, 0, "554889e54156"),
)
SERVICES = (
    ("UnityEngine.Object", "GetInstanceID", CORE, 0),
    ("UnityEngine.Object", "get_name", CORE, 0),
    ("UnityEngine.Behaviour", "get_isActiveAndEnabled", CORE, 0),
    ("UnityEngine.UI.Image", "set_overrideSprite", UI, 1),
)
SLOTS = ("buttonSouth buttonNorth buttonEast buttonWest startButton selectButton leftTrigger rightTrigger leftShoulder rightShoulder dpad dpadUp "
         "dpadDown dpadLeft dpadRight leftStick rightStick leftStickPress rightStickPress touchPad").split()
XBOX_SHEET = "ABXY_Controller_Sheet(384,256)_"
XBOX_NUMBERS = {"buttonSouth": 0, "buttonNorth": 3, "buttonEast": 1, "buttonWest": 2, "startButton": 5, "selectButton": 4, "leftTrigger": 12,
                "rightTrigger": 13, "leftShoulder": 14, "rightShoulder": 16, "leftStickPress": 11, "rightStickPress": 15, "touchPad": 4}
# What each sprite number of the outline sheet draws (row order of the sheet).
OUTLINE = {0: "buttonWest", 1: "buttonEast", 2: "buttonSouth", 3: "buttonNorth", 4: "startButton", 5: "selectButton", 11: "leftStickPress",
           12: "leftTrigger", 13: "rightTrigger", 14: "leftShoulder", 15: "rightShoulder", 18: "rightStickPress", 19: "touchPad"}
FILLED = {0: "buttonWest", 1: "buttonEast", 2: "buttonSouth", 3: "buttonNorth"}
C_NAMES = {"SOUTH": "buttonSouth", "NORTH": "buttonNorth", "EAST": "buttonEast", "WEST": "buttonWest", "START": "startButton", "SELECT": "selectButton",
           "LEFT_TRIGGER": "leftTrigger", "RIGHT_TRIGGER": "rightTrigger", "LEFT_SHOULDER": "leftShoulder", "RIGHT_SHOULDER": "rightShoulder",
           "LEFT_STICK_PRESS": "leftStickPress", "RIGHT_STICK_PRESS": "rightStickPress", "TOUCHPAD": "touchPad"}
# Glyph Images that nothing follows: (end of the path, sprite) that every build must still hold, with the Xbox input they must become.
SCREENS = (
    ("Pause Menu Canvas/RootPanel/PauseTabPanel/PauseTabPanel/LeftTabPrompt", "KS_ButtonPrompt_12", "leftTrigger"),
    ("Pause Menu Canvas/RootPanel/PauseTabPanel/PauseTabPanel/RightTabPrompt", "KS_ButtonPrompt_13", "rightTrigger"),
    ("Vendor Variant/MainGear/PauseTabPanel_1/PauseTabPanel/LeftTabPrompt", "KS_ButtonPrompt_12", "leftTrigger"),
    ("Vendor Variant/MainGear/PauseTabPanel_1/PauseTabPanel/RightTabPrompt", "KS_ButtonPrompt_13", "rightTrigger"),
    ("Canvas Coop-PauseMenu/BG/PauseTabPanel/PauseTabPanel/LeftTabPrompt", "KS_ButtonPrompt_12", "leftTrigger"),
    ("Canvas Coop-PauseMenu/BG/PauseTabPanel/PauseTabPanel/RightTabPrompt", "KS_ButtonPrompt_13", "rightTrigger"),
    ("Player 2 Gear/PauseTabPanel/PauseTabPanel/LeftTabPrompt", "KS_ButtonPrompt_12", "leftTrigger"),
    ("Player 2 Gear/PauseTabPanel/PauseTabPanel/RightTabPrompt", "KS_ButtonPrompt_13", "rightTrigger"),
    ("Player 1 Gear/List/Selected Parent/Selected/Selected Panel/Resource Requirements/Level Resource Panel/L2", "KS_ButtonPrompt_12", "leftTrigger"),
    ("Player 2 Gear/List/Selected Parent/Selected/Selected Panel/Resource Requirements/Level Resource Panel/R2", "KS_ButtonPrompt_13", "rightTrigger"),
    ("Death Screen/GameObject/Tip Button Prompt/Button Prompt/Button", "KS_ButtonPrompt_14", "leftShoulder"),
    ("Death Screen/GameObject/Tip Button Prompt/Button Prompt_1/Button", "KS_ButtonPrompt_15", "rightShoulder"),
    ("Death Screen/GameObject/Continue/prompt", "ButtonPromptSheetTemp(256,64)_2", "buttonSouth"),
    ("Continue Prompt Object/Button Prompt/Button_B", "KS_ButtonPrompt_2", "buttonSouth"),
    ("Dialogue Panel/DialogueVertGroup/Continue Button", "KS_ButtonPrompt_2", "buttonSouth"),
    ("Upgrade_Tab/Upgrade_Level/Button_L2/Upgrade_Icon_4", "PS_ButtonPrompts_11", "leftTrigger"),
    ("Upgrade_Tab/Border_Upgrade/Upgrade Text /Button", "PS_ButtonPrompts_3", "buttonWest"),
    ("Objective_Status_Notification/Panel/Data/Prompt_Panel/Prompt_Icon", "PS_ButtonPrompts_19", "touchPad"),
)
SCENE_FILES = ("sharedassets4.assets", "sharedassets219.assets")
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def read(index, address, size):
    return bytes(index.boot.read(address, size))


def discover(index):
    slots = Slots(index)
    hooks = []
    for type_name, method, image, parameters, _ in HOOKS:
        start, end = index.method_bounds(engine_method(index, type_name, method, image, parameters))
        data = read(index, start, end - start)
        hooks.append((start, len(data), fnv64(data), data[:6]))
    overseer_key = "GameRuntime::GetSubsystem class<None> method<['InputOverseer']>"
    table = slot_table(index, slots, {overseer_key, "TypeInfo GameRuntime"})
    overseer_slot, = table[overseer_key]
    runtime_slot, = table["TypeInfo GameRuntime"]
    get_overseer = subsystem_body(index, slots)
    services = [engine_method(index, *service) for service in SERVICES] + [get_overseer]
    ranges = [index.method_bounds(address) for address in services[:-1]] + [(get_overseer, function_end(index, get_overseer))]
    digest = fnv64(b"".join(read(index, begin, end - begin) for begin, end in ranges))
    return {"hooks": hooks, "services": services, "ranges": ranges, "digest": digest, "overseer_slot": overseer_slot, "runtime_slot": runtime_slot,
            "sprite": image_field(index, "UnityEngine.UI.Image", UI, "m_Sprite"),
            "override": image_field(index, "UnityEngine.UI.Image", UI, "m_OverrideSprite"),
            "icon_data": index.field_offset("InputOverseer", "gamepadIcons"),
            "input_device": index.field_offset("InputOverseer", "m_currentInputDevice"),
            "xbox": index.field_offset("InputIconData", "xboxSeries")}


def format_build(build, v):
    hooks = ",\n     ".join(f"{{0x{m:x}u, 0x{n:x}u, 0x{h:016x}ull, {hexbytes(e, 6)}}}" for m, n, h, e in v["hooks"])
    services = ", ".join(f"0x{s:x}u" for s in v["services"])
    ranges = ", ".join(f"{{0x{b:x}u, 0x{e:x}u}}" for b, e in v["ranges"])
    return (f"/* CUSA57767-{build} */\n"
            f"static const SosImageGlyphSite kImageGlyphs_{build.replace('.', '_')} = {{\n"
            f"    {{{hooks}}},\n"
            f"    {services},\n"
            f"    {{{ranges}}}, 0x{v['digest']:016x}ull,\n"
            f"    0x{v['overseer_slot']:x}u, 0x{v['runtime_slot']:x}u,\n"
            f"    0x{v['sprite']:x}u, 0x{v['override']:x}u, 0x{v['icon_data']:x}u, 0x{v['input_device']:x}u, 0x{v['xbox']:x}u}};")


def header_text(blocks):
    return ("/* Generated by scripts/image_glyphs.py: per build, Image.OnEnable, Image.OnDisable, Image.set_sprite and\n"
            " * InputOverseer.GetTextSpriteAsset (offset, length, FNV-1a, first bytes), the functions the handlers call and the FNV-1a of\n"
            " * their code, the metadata slots and the object layout. */\n" + "\n\n".join(blocks) + "\n")


def source_sheets(text):
    """{sheet prefix: {sprite number: GamepadIcons field}} of launcher/image_glyphs.c."""
    arrays = {}
    for name, body in re.findall(r"static const int8_t (k\w+Sheet)\[\] = \{(.*?)\};", text, re.S):
        values = [value.strip() for value in body.split(",") if value.strip()]
        arrays[name] = {i: C_NAMES[value.replace("SOS_GLYPH_", "")] for i, value in enumerate(values) if value != "NONE"}
    return {prefix: arrays[array] for prefix, array in re.findall(r'SHEET\("([^"]+)", (k\w+Sheet)\)', text)}


def validate_sheets(sheets, ps4_map, ps5_map, xbox_map):
    """Errors in the sprite tables, against the game's own icon maps (slot -> sprite name)."""
    errors = []
    for prefix, icon_map in (("PS_ButtonPrompts_", ps5_map), ("PS4_ButtonPrompts_", ps4_map)):
        expected = {int(sprite[len(prefix):]): slot for slot, sprite in icon_map.items()
                    if sprite and re.fullmatch(re.escape(prefix) + r"\d+", sprite) and slot in XBOX_NUMBERS}
        if sheets.get(prefix) != expected:
            errors.append(f"{prefix} differs from the game's icon map")
    if sheets.get("KS_ButtonPrompt_") != OUTLINE:
        errors.append("KS_ButtonPrompt_ differs from the outline sheet")
    if sheets.get("ButtonPromptSheetTemp(256,64)_") != FILLED:
        errors.append("ButtonPromptSheetTemp differs from the filled sheet")
    for slot, number in XBOX_NUMBERS.items():
        if xbox_map.get(slot) != f"{XBOX_SHEET}{number}":
            errors.append(f"the game's Xbox icon map shows {xbox_map.get(slot)} for {slot}")
    return errors


def game_objects(data, objects):
    found = {}
    for path_id, (offset, _size, class_id) in objects.items():
        if class_id == 1:
            count = struct.unpack_from("<i", data, offset)[0]
            at = offset + 8 + 12 * count
            length = struct.unpack_from("<i", data, at)[0]
            found[path_id] = (bytes(data[at + 4:at + 4 + length]).decode("utf-8", "replace"),
                              [struct.unpack_from("<iq", data, offset + 4 + 12 * k)[1] for k in range(count)])
    return found


def path_of(data, objects, names, game_object):
    parts = []
    while game_object in names and len(parts) < 20:
        name, components = names[game_object]
        parts.append(name)
        transform = next((c for c in components if c in objects and objects[c][2] in (4, 224)), None)
        if transform is None:
            break
        offset = objects[transform][0] + 52
        count = struct.unpack_from("<i", data, offset)[0]
        parent = struct.unpack_from("<iq", data, offset + 4 + 12 * count)[1]
        if parent not in objects:
            break
        game_object = struct.unpack_from("<iq", data, objects[parent][0])[1]
    return "/".join(reversed(parts))


def externals(path):
    """The serialized file's external references, in order (file id 1 is the first)."""
    data = path.read_bytes()
    position = 0x14 + 28
    end = data.index(b"\0", position)
    position = end + 1 + 4
    has_tree = data[position]
    position += 1
    count, = struct.unpack_from("<i", data, position)
    position += 4
    for _ in range(count):
        class_id, = struct.unpack_from("<i", data, position)
        position += 7 + (16 if class_id == 114 else 0) + 16
        if has_tree:
            raise ValueError("type trees are not expected here")
    count, = struct.unpack_from("<i", data, position)
    position += 4
    for _ in range(count):
        position = ((position + 3) & ~3) + 24
    count, = struct.unpack_from("<i", data, position)
    position += 4
    for _ in range(count):
        position = ((position + 4 + 3) & ~3) + 8
    count, = struct.unpack_from("<i", data, position)
    position += 4
    names = []
    for _ in range(count):
        position = data.index(b"\0", position) + 1 + 16 + 4
        end = data.index(b"\0", position)
        names.append(data[position:end].decode())
        position = end + 1
    return names


def static_glyphs(media, scripts, sheets):
    """{path: sprite name} of the Images of SCENE_FILES that show a mapped sprite and have no AutoButtonSprite."""
    sprites = {}
    for file in ("resources.assets", "sharedassets4.assets"):
        data, objects = pac.parse(media / file)
        for path_id, (offset, size, class_id) in objects.items():
            if class_id == pac.CLASS_SPRITE:
                name = pac.object_name(data, offset, size, class_id) or ""
                prefix = next((p for p in sheets if name.startswith(p)), None)
                if prefix and name[len(prefix):].isdigit() and int(name[len(prefix):]) in sheets[prefix]:
                    sprites[(file, path_id)] = name
    images, followers = set(scripts["Image"]), set(scripts["AutoButtonSprite"])
    found = {}
    for file in SCENE_FILES:
        data, objects = pac.parse(media / file)
        names = game_objects(data, objects)
        files = [file] + externals(media / file)
        for path_id, (offset, size, class_id) in objects.items():
            if class_id != pac.CLASS_MONOBEHAVIOUR or pac.script_of(data, objects, path_id) not in images:
                continue
            owner = struct.unpack_from("<iq", data, offset)[1]
            for at in range(offset + 0x1C, offset + size - 11, 4):
                file_id, target = struct.unpack_from("<iq", data, at)
                sprite = sprites.get((files[file_id], target)) if 0 <= file_id < len(files) else None
                if sprite and owner in names:
                    if not any(pac.script_of(data, objects, c) in followers for c in names[owner][1] if c in objects):
                        found[path_of(data, objects, names, owner)] = sprite
                    break
    return found


def validate_screens(found, sheets):
    errors = []
    for ending, sprite, slot in SCREENS:
        matches = {path: name for path, name in found.items() if path.endswith(ending)}
        if not matches:
            errors.append(f"{ending} is not a static glyph any more")
        elif set(matches.values()) != {sprite}:
            errors.append(f"{ending} shows {sorted(set(matches.values()))}, not {sprite}")
        prefix = next((p for p in sheets if sprite.startswith(p)), None)
        if prefix is None or sheets[prefix].get(int(sprite[len(prefix):])) != slot:
            errors.append(f"{sprite} does not become {slot}")
    return errors


def check_code(index, build, values, stored):
    check(format_build(build, values) in stored, "launcher/image_glyphs_sites.h holds the hooks of this build")
    names = index.all_method_names()
    code = read(index, index.module_base, index.module_size)
    starts = sorted(names)
    for (type_name, method, _image, _parameters, entry), (start, length, _digest, first) in zip(HOOKS, values["hooks"]):
        full = f"{type_name}::{method}"
        check(names.get(start) == full and start + length in starts, f"{full}: the hook is at the start of the method, which ends at the next one")
        check(first == bytes.fromhex(entry), f"{full}: the first 6 bytes are three or four whole instructions, none relative")
        inside = [m.start() for m in re.finditer(rb"[\xe8\xe9]", code) if m.start() + 5 <= len(code) and
                  start < index.module_base + m.start() + 5 + struct.unpack_from("<i", code, m.start() + 1)[0] < start + 6]
        check(not inside, f"{full}: no direct call or jump lands inside the stolen bytes")
    enable = read(index, values["hooks"][0][0], values["hooks"][0][1])
    track = engine_method(index, "UnityEngine.UI.Image", "TrackSprite", UI, 0)
    check(enable.find(b"\xe9") >= 0 and track in [values["hooks"][0][0] + m.start() + 5 + struct.unpack_from("<i", enable, m.start() + 1)[0]
                                                 for m in re.finditer(rb"\xe9", enable) if m.start() + 5 <= len(enable)],
          "Image.OnEnable ends in TrackSprite")
    check(values["sprite"] == 0xD8 and values["override"] == 0xE0, "m_Sprite is at +0xd8 and m_OverrideSprite at +0xe0")
    icons = [index.field_offset("GamepadIcons", slot) - 0x10 for slot in SLOTS]
    check(icons == [8 * i for i in range(len(SLOTS))], "the 20 GamepadIcons sprites are consecutive, in the order of SosGlyph")
    derived = [index.type_name(i) for i in range(len(index.types)) if image_parent(index, i) == "UnityEngine.UI.Image"]
    overriding = [name for name in derived if any(f"{name}::{m}" in names.values() for m in ("OnDisable", "OnEnable"))]
    check(not overriding, f"no Image subclass replaces OnEnable or OnDisable (subclasses {derived})")
    text = index.method_address("InputOverseer", "GetTextSpriteAsset")
    with_icons = index.method_address("InputOverseer", "SetTextWithIcons")
    start, end = index.method_bounds(with_icons)
    body = read(index, start, end - start)
    resolve = index.method_address("InputOverseer", "ResolveIconsForCurrentInput")
    calls = [start + m.start() + 5 + struct.unpack_from("<i", body, m.start() + 1)[0] for m in re.finditer(rb"\xe8", body) if m.start() + 5 <= len(body)]
    check(text in calls and resolve not in calls, "SetTextWithIcons takes the text atlas before any icon lookup, so the atlas lookup needs its own hook")
    change = index.method_address("InputOverseer", "OnInputActionChange")
    start, end = index.method_bounds(change)
    check(re.search(rb"\x41\xc7\x01\x03\x00\x00\x00", read(index, start, end - start)) is not None,
          "using the other gamepad stores the PS4 type again, which the atlas hook has to correct")


def image_parent(index, type_index):
    parent = index.types[type_index][4]
    if parent < 0:
        return None
    types = index.boot.pointer(index.metadata_registration + 0x38)
    pointer = index.boot.pointer(types + 8 * parent)
    kind = index.boot.u32(pointer + 8) >> 16 & 0xFF
    return index.type_name(index.boot.u64(pointer)) if kind in (0x11, 0x12) else None


def game_assets(media):
    data, objects = pac.parse(media / "resources.assets")
    icon_data = next(p for p, (o, s, c) in objects.items() if c == pac.CLASS_MONOBEHAVIOUR and s > 1000 and
                     (pac.object_name(data, o, s, c) or "").endswith("InputIconData"))
    maps = pac.icon_maps(data, objects, icon_data)
    ps5, ps4, xbox = (dict(zip(SLOTS, slots)) for slots, _ in maps)
    return ps4, ps5, xbox


def check_assets(media, sheets):
    if not (media / "resources.assets").is_file():
        return False
    ps4, ps5, xbox = game_assets(media)
    errors = validate_sheets(sheets, ps4, ps5, xbox)
    check(not errors, f"every mapped sprite is the same input in the game's own icon maps, and its Xbox sprite is the expected one {errors or ''}")
    scripts = pac.named_scripts(media)
    if scripts is None or not all((media / file).is_file() for file in SCENE_FILES):
        return False
    found = static_glyphs(media, scripts, sheets)
    errors = validate_screens(found, sheets)
    check(not errors, f"{len(SCREENS)} known glyph Images without a follower are still there and become the matching Xbox input "
                      f"({len(found)} such Images in all) {errors or ''}")
    return True


def check_build(build):
    print(f"CUSA57767-{build}")
    index = open_reported(build, "the glyph hooks")
    if index is None:
        return
    index.all_method_names()
    check_code(index, build, discover(index), HEADER.read_text() if HEADER.is_file() else "")
    complete = check_assets(evidence.media(build), source_sheets(SOURCE.read_text()))
    evidence.report_content(build, "the sprites and the scenes", complete)


def self_test():
    sheets = source_sheets(SOURCE.read_text())
    ps5 = {slot: f"PS_ButtonPrompts_{n}" for n, slot in sheets["PS_ButtonPrompts_"].items()}
    ps4 = {slot: f"PS4_ButtonPrompts_{n}" for n, slot in sheets["PS4_ButtonPrompts_"].items()}
    xbox = {slot: f"{XBOX_SHEET}{n}" for slot, n in XBOX_NUMBERS.items()}
    found = {"Root/" + ending: sprite for ending, sprite, _ in SCREENS}
    check(not validate_sheets(sheets, ps4, ps5, xbox) and not validate_screens(found, sheets), "the real tables pass")

    def changed(prefix, number, slot):
        copy = {name: dict(table) for name, table in sheets.items()}
        copy[prefix][number] = slot
        return copy

    check(bool(validate_sheets(changed("KS_ButtonPrompt_", 12, "leftShoulder"), ps4, ps5, xbox)), "L2 shown as LB is rejected")
    check(bool(validate_sheets(changed("KS_ButtonPrompt_", 13, "rightShoulder"), ps4, ps5, xbox)), "R2 shown as RB is rejected")
    check(bool(validate_sheets(changed("PS4_ButtonPrompts_", 11, "leftShoulder"), ps4, ps5, xbox)), "a trigger and a bumper swapped in the icon sheet are rejected")
    check(bool(validate_sheets(changed("PS_ButtonPrompts_", 1, "buttonEast"), ps4, ps5, xbox)), "Cross shown as B is rejected")
    check(bool(validate_sheets(sheets, ps4, ps5, dict(xbox, rightTrigger=XBOX_SHEET + "16"))), "an Xbox icon map with RB for the right trigger is rejected")
    check(bool(validate_screens(dict(found, **{"Root/" + SCREENS[0][0]: "KS_ButtonPrompt_14"}), sheets)), "a tab prompt with another sprite is rejected")
    check(bool(validate_screens({p: s for p, s in found.items() if not p.endswith(SCREENS[4][0])}, sheets)), "a vanished Pit tab prompt is rejected")


def main(argv):
    if "generate" in argv:
        blocks = []
        for build in BUILDS:
            index = open_build(build)
            if index:
                index.all_method_names()
                blocks.append(format_build(build, discover(index)))
        sys.stdout.write(header_text(blocks))
        return 0
    if "--self-test" in argv:
        self_test()
    else:
        for build in BUILDS:
            check_build(build)
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
