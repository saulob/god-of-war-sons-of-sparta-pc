"""Settings confirmation dialog glyphs of the built boot images: generates launcher/prompt_dialog_sites.h and proves, from the
game's metadata and code, what the dialog hook relies on.

    python scripts/prompt_dialog.py            check the installed builds against the header and the game code
    python scripts/prompt_dialog.py generate   print the header for the installed builds

Nothing is executed; a build without a boot image or metadata is skipped. Exit status 0 when every check passes.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from il2cpp_index import ROOT, open_build  # noqa: E402
from prompt_sites import fnv64, hexbytes, lookup_slot_and_target  # noqa: E402

HEADER = ROOT / "launcher" / "prompt_dialog_sites.h"
BUILDS = ("1.00", "1.01")
UPDATE = ("PromptTextSwitch", "UpdatePromptDescriptions")
SWITCH = ("MenuSystem.SettingsMenu", "TrySwitchPromptText")
ENTRY = bytes([0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x80, 0x3D])
ENTRY_STOLEN = 18
ENTRY_KEPT = 11
DIALOG_KEYS = {
    "exit": ("MenuSystem.SettingsMenu", "ExitConfirmationPopup", "Quit Game"),
    "default": ("MenuSystem.SettingsMenu", "ToggleDefaultAll", "Reset to Default"),
    "changedsettings": ("MenuSystem.SettingsMenu", "ConfirmationPopup", "Unsaved Changes"),
    "restartrun": ("MenuSystem.SettingsMenu", "RestartRunConfirmationPopup", "Restart Run"),
    "restartPitRun": ("MenuSystem.SettingsMenu", "PitExitConfirmationPopup", "Return to Pit Entrance"),
    "keybindings": ("MenuSystem.SettingsMenu", "TryPromptResetKeybindings", "Reset Keybindings"),
}
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def read(index, address, size):
    return bytes(index.boot.read(address, size))


def function_end(index, start):
    """The end of a function: after the last ud2 that is followed by padding."""
    data = read(index, start, 0x600)
    at = 0
    while True:
        found = data.find(b"\x0f\x0b", at)
        if found < 0:
            raise LookupError(f"no end found for the function at 0x{start:x}")
        if data[found + 2:found + 4] == b"\x00\x00" or data[found + 2:found + 4] == b"\xcc\xcc":
            return start + found + 2
        at = found + 2


def slot_name(slots, address):
    return slots.describe(address)


def slot_table(index, slots, wanted):
    """Static slots (tagged metadata usages) whose description is in `wanted`: {description: [addresses]}."""
    low, high = 0x9000000, 0xA100000
    data = read(index, low, high - low)
    found = {}
    for offset in range(0, len(data) - 8, 8):
        value = struct.unpack_from("<Q", data, offset)[0]
        if not value & 1 or value >> 32:
            continue
        kind = (value >> 29) & 7
        if kind not in (1, 6) or (kind == 6 and ((value & 0x1FFFFFFF) >> 1) >= slots.nspecs):
            continue
        try:
            description = slots.describe(low + offset)
        except (TypeError, IndexError, struct.error, LookupError):
            continue
        if description in wanted:
            found.setdefault(description, []).append(low + offset)
    return found


class Slots:
    def __init__(self, index):
        self.index = index
        boot = index.boot
        registration = index.metadata_registration
        self.insts = boot.pointer(registration + 0x18)
        self.types = boot.pointer(registration + 0x38)
        self.specs = boot.pointer(registration + 0x48)
        self.nspecs = boot.u32(registration + 0x40)
        self.method_owner = {}
        for type_index, t in enumerate(index.types):
            for method in range(t[9], t[9] + t[16]):
                self.method_owner[method] = type_index

    def type_name(self, pointer):
        boot = self.index.boot
        kind = boot.u32(pointer + 8) >> 16 & 0xFF
        return self.index.type_name(boot.u64(pointer)) if kind in (0x11, 0x12) else f"kind{kind:#x}"

    def instance(self, number):
        boot = self.index.boot
        inst = boot.pointer(self.insts + 8 * number)
        argv = boot.pointer(inst + 8)
        return [self.type_name(boot.pointer(argv + 8 * i)) for i in range(boot.u32(inst))]

    def describe(self, address):
        value = self.index.boot.u64(address)
        kind, number = (value >> 29) & 7, (value & 0x1FFFFFFF) >> 1
        if kind == 1:
            return "TypeInfo " + self.type_name(self.index.boot.pointer(self.types + 8 * number))
        if kind == 6:
            definition, class_inst, method_inst = struct.unpack_from("<3i", read(self.index, self.specs + 12 * number, 12))
            owner = self.method_owner.get(definition)
            name = self.index.meta.string(self.index.methods[definition][0])
            owner_name = self.index.type_name(owner) if owner is not None else "?"
            arguments = self.instance(method_inst) if method_inst >= 0 else None
            holder = self.instance(class_inst) if class_inst >= 0 else None
            return f"{owner_name}::{name} class<{holder}> method<{arguments}>"
        return f"kind{kind}"


def image_field(index, type_name, image, field):
    type_index = index.type_indices(type_name, image)[0]
    t = index.types[type_index]
    table = index.boot.pointer(index.metadata_registration + 0x58)
    base = index.boot.pointer(table + 8 * type_index)
    for k in range(t[18]):
        if index.meta.string(index.fields[t[8] + k][0]) == field:
            return index.boot.u32(base + 4 * k)
    raise LookupError(field)


def struct_member(index, type_name, member):
    """Offset inside the struct of a field of a value type (its fields are boxed: the object header is 0x10)."""
    return image_field(index, type_name, "Assembly-CSharp.dll", member) - 0x10


def engine_method(index, type_name, method_name, image, parameters):
    found = []
    for type_index in index.type_indices(type_name, image):
        for method_index, row in index.type_methods(type_index):
            if index.meta.string(row[0]) == method_name and row[10] == parameters:
                address = index._address(method_index)
                if address:
                    found.append(address)
    if len(found) != 1:
        raise LookupError(f"{type_name}::{method_name}: {len(found)} matches")
    return found[0]


def shared_target(index, slot, register_prefixes=(b"\x48\x8b\x35",)):
    """The call that follows a load of `slot` into rsi: the shared generic body every call site of that method info uses."""
    code = read(index, index.module_base, index.module_size)
    counts = {}
    for prefix in register_prefixes:
        for match in re.finditer(re.escape(prefix), code):
            at = match.start()
            if index.module_base + at + 7 + struct.unpack_from("<i", code, at + 3)[0] != slot:
                continue
            window = code[at + 7:at + 40]
            for call in re.finditer(rb"\xe8", window):
                if call.start() + 5 > len(window):
                    continue
                target = index.module_base + at + 7 + call.start() + 5 + struct.unpack_from("<i", window, call.start() + 1)[0]
                if target != 0x29C2E90 and not (0x29C0000 <= target < 0x29D0000):
                    counts[target] = counts.get(target, 0) + 1
                    break
    return counts


def subsystem_body(index, slots):
    """The shared body of GameRuntime.GetSubsystem<T>: the one function that opens with a method info check, loads the
    GameRuntime statics and tail-jumps to the subsystem lookup."""
    _, lookup = lookup_slot_and_target(index)
    class_slot = slot_table(index, slots, {"TypeInfo GameRuntime"}).get("TypeInfo GameRuntime", [])
    code = read(index, index.module_base, index.module_size)
    found = []
    for match in re.finditer(rb"\x5d\xe9", code):
        at = match.start() + 1
        if at + 5 > len(code) or index.module_base + at + 5 + struct.unpack_from("<i", code, at + 1)[0] != lookup:
            continue
        start = code.rfind(b"\x55\x48\x89\xe5", max(0, at - 0x90), at)
        if start < 0:
            continue
        body = code[start:at + 5]
        loads = [index.module_base + start + m.start() + 7 + struct.unpack_from("<i", body, m.start() + 3)[0]
                 for m in re.finditer(rb"\x48[\x8b\x8d]\x05|\x48\x8d\x3d", body) if m.start() + 7 <= len(body)]
        if body[4:12] == b"\x53\x50\x48\x83\x7f\x38\x00\x48" and set(class_slot) & set(loads):
            found.append(index.module_base + start)
    if len(found) != 1:
        raise LookupError(f"GameRuntime.GetSubsystem body: {len(found)} candidates")
    return found[0]


def discover(index):
    slots = Slots(index)
    update = index.method_address(*UPDATE)
    entry = read(index, update, ENTRY_STOLEN)
    image_key = "UnityEngine.Component::GetComponent class<None> method<['UnityEngine.UI.Image']>"
    overseer_key = "GameRuntime::GetSubsystem class<None> method<['InputOverseer']>"
    table = slot_table(index, slots, {image_key, overseer_key})
    image_slot, = table[image_key]
    overseer_slot, = table[overseer_key]
    components = shared_target(index, image_slot)
    if len(components) != 1:
        raise LookupError(f"GetComponent<Image> targets: {components}")
    get_component = next(iter(components))
    get_overseer = subsystem_body(index, slots)
    values = {
        "update": update,
        "entry": entry,
        "image_slot": image_slot,
        "overseer_slot": overseer_slot,
        "get_component": get_component,
        "get_overseer": get_overseer,
        "set_override": engine_method(index, "UnityEngine.UI.Image", "set_overrideSprite", "UnityEngine.UI.dll", 1),
        "transform_of": engine_method(index, "UnityEngine.Component", "get_transform", "UnityEngine.CoreModule.dll", 0),
        "parent_of": engine_method(index, "UnityEngine.Transform", "get_parent", "UnityEngine.CoreModule.dll", 0),
        "child_count": engine_method(index, "UnityEngine.Transform", "get_childCount", "UnityEngine.CoreModule.dll", 0),
        "child_at": engine_method(index, "UnityEngine.Transform", "GetChild", "UnityEngine.CoreModule.dll", 1),
    }
    order = ("update", "get_component", "get_overseer", "set_override", "transform_of", "parent_of", "child_count", "child_at")
    shared = ("get_component", "get_overseer")
    values["ranges"] = [(values[name], function_end(index, values[name]) if name in shared else index.method_bounds(values[name])[1])
                        for name in order]
    digest = 0xCBF29CE484222325
    for begin, end in values["ranges"]:
        for byte in read(index, begin, end - begin):
            digest = ((digest ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    values["hash"] = digest
    values["text_list"] = index.field_offset("PromptTextSwitch", "textList")
    values["icon_data"] = index.field_offset("InputOverseer", "gamepadIcons")
    values["input_device"] = index.field_offset("InputOverseer", "m_currentInputDevice")
    xbox = index.field_offset("InputIconData", "xboxSeries")
    values["xbox_confirm"] = xbox + struct_member(index, "GamepadIcons", "buttonSouth")
    values["xbox_back"] = xbox + struct_member(index, "GamepadIcons", "buttonEast")
    values["override_sprite"] = image_field(index, "UnityEngine.UI.Image", "UnityEngine.UI.dll", "m_OverrideSprite")
    return values


def format_build(build, v):
    ranges = ", ".join(f"{{0x{b:x}u, 0x{e:x}u}}" for b, e in v["ranges"])
    return (f"/* CUSA57767-{build}: PromptTextSwitch::UpdatePromptDescriptions */\n"
            f"static const SosPromptDialogSite kDialog_{build.replace('.', '_')} = {{\n"
            f"    0x{v['update']:x}u, {hexbytes(v['entry'], ENTRY_STOLEN)},\n"
            f"    0x{v['image_slot']:x}u, 0x{v['overseer_slot']:x}u,\n"
            f"    0x{v['get_component']:x}u, 0x{v['get_overseer']:x}u, 0x{v['set_override']:x}u,\n"
            f"    0x{v['transform_of']:x}u, 0x{v['parent_of']:x}u, 0x{v['child_count']:x}u, 0x{v['child_at']:x}u,\n"
            f"    {{{ranges}}}, 0x{v['hash']:016x}ull,\n"
            f"    0x{v['text_list']:x}, 0x{v['icon_data']:x}, 0x{v['input_device']:x}, 0x{v['xbox_confirm']:x}, "
            f"0x{v['xbox_back']:x}, 0x{v['override_sprite']:x}}};")


def header_text(blocks):
    return ("/* Generated by scripts/prompt_dialog.py: per build, PromptTextSwitch.UpdatePromptDescriptions (its first 18 bytes), the\n"
            " * method infos it needs, the game functions the dialog hook calls, the FNV-1a of their code and the object layout. */\n" +
            "\n\n".join(blocks) + "\n")


def literal_table(index):
    meta = index.meta
    records = meta.records("stringLiteral", "<2I")
    base = meta.section["stringLiteralData"][0]
    return lambda n: meta.data[base + records[n][1]:base + records[n][1] + records[n][0]].decode("utf-8", "replace")


def check_coverage(index, slots, update):
    code = read(index, index.module_base, index.module_size)
    names = index.all_method_names()
    index.method_bounds(update)

    def call_sites(target):
        found = []
        for match in re.finditer(rb"[\xe8\xe9]", code):
            at = match.start()
            if at + 5 <= len(code) and index.module_base + at + 5 + struct.unpack_from("<i", code, at + 1)[0] == target:
                found.append(index.module_base + at)
        return found

    sites = call_sites(update)
    owners = sorted({index.method_at(a) for a in sites})
    check(owners == ["MenuSystem.SettingsMenu::TrySwitchPromptText", "PromptTextSwitch::UpdateLocale"],
          f"UpdatePromptDescriptions is entered only from TrySwitchPromptText and UpdateLocale (found {owners})")
    locale = index.method_address("PromptTextSwitch", "UpdateLocale")
    check(read(index, locale, 4) == b"\x48\x8b\x77\x30" and any(a == locale + 4 and code[a - index.module_base] == 0xE9 for a in sites),
          "UpdateLocale loads this and the stored key and tail-jumps to the method entry")
    pointers = [m.start() for m in re.finditer(rb"[\x48\x4c][\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", code)
                if m.start() + 7 <= len(code) and index.module_base + m.start() + 7 + struct.unpack_from("<i", code, m.start() + 3)[0] == update]
    check(not pointers, f"nothing takes the address of the method ({len(pointers)} references)")

    switch = index.method_address(*SWITCH)
    literal = literal_table(index)
    callers = {}
    for site in call_sites(switch):
        callers.setdefault(index.method_at(site), []).append(site)
    boot = index.boot
    for key, (type_name, method, label) in DIALOG_KEYS.items():
        full = f"{type_name}::{method}"
        if not check(full in callers, f"{label}: {full} calls TrySwitchPromptText"):
            continue
        low, high = index.method_bounds(index.method_address(type_name, method))
        body = code[low - index.module_base:high - index.module_base]
        used = set()
        for match in re.finditer(rb"[\x48\x4c][\x8b\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", body):
            if match.start() + 7 > len(body):
                continue
            slot = low + match.start() + 7 + struct.unpack_from("<i", body, match.start() + 3)[0]
            try:
                value = boot.u64(slot)
            except Exception:
                continue
            if value & 1 and (value >> 29) & 7 == 5 and ((value & 0x1FFFFFFF) >> 1) < len(index.meta.records("stringLiteral", "<2I")):
                used.add(literal((value & 0x1FFFFFFF) >> 1))
        check(key in used, f"{label}: {full} passes the key '{key}'")


def check_code(index, build, values, stored):
    update = values["update"]
    check(format_build(build, values) in stored, "launcher/prompt_dialog_sites.h holds the dialog hook of this build")
    names = index.all_method_names()
    check(names.get(update) == "PromptTextSwitch::UpdatePromptDescriptions", "the hook address is the start of the method")
    entry = values["entry"]
    check(entry[:13] == ENTRY and entry[17] == 0x00, "the entry is push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; cmp byte [rip+x],0")
    check(len(ENTRY[:ENTRY_KEPT]) == ENTRY_KEPT, "the first 11 bytes are six whole push/mov instructions")
    after = read(index, update + ENTRY_STOLEN, 7)
    check(after == b"\x49\x89\xf6\x48\x89\xfb\x75", "after the entry: mov r14,rsi; mov rbx,rdi; jne (no flag change before the branch, rax unused)")
    method = index.methods[next(m for t in index.type_indices(UPDATE[0]) for m, row in index.type_methods(t)
                                if index.meta.string(row[0]) == UPDATE[1])]
    check(method[10] == 1, f"the method takes one argument besides this (parameter count {method[10]})")
    code = read(index, index.module_base, index.module_size)
    entries = [index.module_base + m.start() for m in re.finditer(rb"[\xe8\xe9]", code)
               if m.start() + 5 <= len(code) and update < index.module_base + m.start() + 5 + struct.unpack_from("<i", code, m.start() + 1)[0] < update + ENTRY_STOLEN]
    check(not entries, f"no direct call or jump lands inside the stolen bytes ({len(entries)} found)")
    flag = update + ENTRY_STOLEN + struct.unpack_from("<i", entry, ENTRY_KEPT + 2)[0]
    check(index.module_base <= flag < index.module_base + index.module_size, "the class init flag read by the entry is inside the image")
    check(values["text_list"] == 0x20 and values["icon_data"] == 0x20 and values["input_device"] == 0x30 and values["override_sprite"] == 0xE0,
          f"layout: textList +0x20, gamepadIcons +0x20, input device +0x30, overrideSprite +0xe0 "
          f"(metadata says {values['text_list']:#x}, {values['icon_data']:#x}, {values['input_device']:#x}, {values['override_sprite']:#x})")
    check(values["xbox_back"] - values["xbox_confirm"] == 0x10, f"the Xbox South and East sprites are 0x10 apart "
          f"(+0x{values['xbox_confirm']:x}, +0x{values['xbox_back']:x})")
    expected = {"PromptTextSwitch::UpdatePromptDescriptions", "UnityEngine.UI.Image::set_overrideSprite", "UnityEngine.Component::get_transform",
                "UnityEngine.Transform::get_parent", "UnityEngine.Transform::get_childCount", "UnityEngine.Transform::GetChild"}
    inside = {name for address, name in names.items() if any(begin <= address < end for begin, end in values["ranges"])}
    check(inside == expected, f"the hashed code holds only the dialog method and the Unity transform and sprite calls (found {sorted(inside - expected)} extra)")
    footer = [name for name in names.values() if name.startswith("AutoButtonSprite::")]
    check(footer and not any(begin <= address < end for address, name in names.items() if name in footer for begin, end in values["ranges"]),
          f"no AutoButtonSprite method (the footer glyphs, {len(footer)} methods) lies in the hashed code")
    gamepad = index.enum_values("GamepadType")
    devices = index.enum_values("ControlDevice")
    check(gamepad.get("XboxSeries") == 2 and devices.get("Gamepad") == 2, "XboxSeries is 2 and ControlDevice.Gamepad is 2")


def check_build(build):
    index = open_build(build)
    if index is None:
        print(f"  [skip] CUSA57767-{build}: no boot image or metadata installed")
        return
    print(f"CUSA57767-{build}")
    try:
        values = discover(index)
    except LookupError as error:
        check(False, f"the dialog hook sites are found: {error}")
        return
    stored = HEADER.read_text() if HEADER.is_file() else ""
    check_code(index, build, values, stored)
    check_coverage(index, Slots(index), values["update"])


def main(argv):
    if len(argv) > 1 and argv[1] == "generate":
        blocks = []
        for build in BUILDS:
            index = open_build(build)
            if index:
                blocks.append(format_build(build, discover(index)))
        print(header_text(blocks), end="")
        return 0
    for build in BUILDS:
        check_build(build)
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
