"""Cheat hook sites of CUSA57767 1.00 and 1.01, each found in that build's own boot image and IL2CPP metadata.

    python scripts/cheat_sites.py generate      # writes src/sos_cheat_sites.h from the local boot images
    python scripts/cheat_sites.py check         # the header against the boot images, or its contract without them
    python scripts/cheat_sites.py --self-test   # mutated copies of the data that must be refused

Nothing is shared between the builds: every method is looked up by type, name and signature, every patched instruction
by a byte pattern that must match exactly once in that method, and every field offset and item id is read again.
"""
import argparse
import bisect
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import build_evidence as evidence  # noqa: E402

HEADER = ROOT / "src" / "sos_cheat_sites.h"
BUILDS = ("1.00", "1.01")
ATOMIC_BLOCK = 16
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
ANY = None

STRING_NEW_NID = "sQcvFCJYqD8"  # il2cpp_string_new
GAME_MODULE = "Il2cppUserAssemblies.prx"

# (key, type, method, signature): the game functions the handlers call.
FUNCTIONS = [
    ("get_game_object", "UnityEngine.Component", "get_gameObject", "UnityEngine.GameObject ()", "UnityEngine.CoreModule.dll"),
    ("is_player", "Utilities.PawnUtils", "IsPlayer", "static bool (UnityEngine.GameObject target)", "Assembly-CSharp.dll"),
    ("get_pawn", "Controller", "GetPawnObject", "UnityEngine.GameObject ()", "Assembly-CSharp.dll"),
    ("player_index", "PlayerController", "get_PlayerIndex", "int ()", "Assembly-CSharp.dll"),
    ("try_get_wallet", "RuntimeData.PlayerRuntimeDataUtils", "TryGetWallet",
     "static PersistentData.PlayerWallet (UnityEngine.GameObject playerObject)", "Assembly-CSharp.dll"),
    ("item_by_id", "PersistentData.PlayerInventory", "GetItemDefinitionByItemId",
     "static ItemScripts.ItemDefinition (string itemId)", "Assembly-CSharp.dll"),
    ("wallet_amount", "PersistentData.PlayerWallet", "GetCurrentAmount", "long (ItemScripts.ItemDefinition target)",
     "Assembly-CSharp.dll"),
    ("wallet_add", "PersistentData.PlayerWallet", "AddToWallet",
     "void (ItemScripts.ItemDefinition item, long count, generic overrideDontShowNotification)", "Assembly-CSharp.dll"),
    ("slope_aligned", "CharacterMovement", "GetSlopeAlignedVersionOfMovementDelta",
     "UnityEngine.Vector2 (UnityEngine.Vector2 moveDelta)", "Assembly-CSharp.dll"),
    ("jump_angle", "MoveStateJump", "GetJumpAngleAsForwardDirectionVector",
     "UnityEngine.Vector2 (float customForwardAdjustmentFactor)", "Assembly-CSharp.dll"),
]

# (key, type, field, offset, image): the layout the handlers read; a build with another offset is refused.
FIELDS = [
    ("native_object", "UnityEngine.Object", "m_CachedPtr", 0x10, "UnityEngine.CoreModule.dll"),
    ("vitals_max_hp", "EntityComponents.Vitals", "maxHp", 0x38, "Assembly-CSharp.dll"),
    ("vitals_hp", "EntityComponents.Vitals", "currentHp", 0x40, "Assembly-CSharp.dll"),
    ("vitals_grey_hp", "EntityComponents.Vitals", "currentGreyHp", 0x44, "Assembly-CSharp.dll"),
    ("spirit_current", "EntityComponents.SpartanSpirit", "currentSpirit", 0x20, "Assembly-CSharp.dll"),
    ("spirit_max", "EntityComponents.SpartanSpirit", "maxSpirit", 0x24, "Assembly-CSharp.dll"),
    ("mana_current", "EntityComponents.ManaState", "currentMana", 0x28, "Assembly-CSharp.dll"),
    ("mana_max", "EntityComponents.ManaState", "currentMaxMana", 0x2C, "Assembly-CSharp.dll"),
    ("hit_source", "EntityComponents.HitEventData", "source", 0x10, "Assembly-CSharp.dll"),
    ("hit_target", "EntityComponents.HitEventData", "hitEntity", 0x20, "Assembly-CSharp.dll"),
    ("hit_config", "EntityComponents.HitEventData", "hitConfig", 0x28, "Assembly-CSharp.dll"),
    ("hit_args", "EntityComponents.HitEventData", "eventModuleArgs", 0x30, "Assembly-CSharp.dll"),
    ("damage_module", "EventModules.HitConfig", "damageModule", 0x10, "Assembly-CSharp.dll"),
    ("damage_config", "EventModules.EventModule_Damage", "damageConfig", 0x10, "Assembly-CSharp.dll"),
    ("damage_value", "DamageConfig", "damage", 0x10, "Assembly-CSharp.dll"),
    ("damage_unmodifiable", "DamageConfig", "unmodifiableDamage", 0x35, "Assembly-CSharp.dll"),
    ("movement_input_delta", "CharacterMovement", "deltaFromMovementInputsThisFrame", 0xE0, "Assembly-CSharp.dll"),
    ("jump_forward_adjustment", "MoveStateJump", "forwardAdjustmentFactor", 0x9C, "Assembly-CSharp.dll"),
    ("jump_context", "MoveStateAerialBase", "context", 0x58, "Assembly-CSharp.dll"),
    ("wallet_count", "PersistentData.PlayerWallet", "walletCount", 0x20, "Assembly-CSharp.dll"),
]

# Item ids: Add 1000 Blood Orbs and Add 10 Upgrade Materials. Boss trophies (loot_boss_*) only their boss gives.
BLOOD_ORB = "loot_orb_red"
UPGRADE_MATERIALS = ["loot_common_shield", "loot_rare_shield", "loot_epic_shield", "loot_common_spear", "loot_rare_spear",
                     "loot_epic_spear_grip", "loot_epic_spear_tail", "loot_epic_spear_tip", "loot_epic_spear_tip_status"]
ITEM_FILES = ["Media/resources.assets", "Media/sharedassets3.assets", "Media/sharedassets4.assets"]


def hexpattern(text):
    return [ANY if b == "??" else int(b, 16) for b in text.split()]


# (key, cheat, type, method, signature, pattern, at, length, executed, call target key or None, post-call site)
# `at`/`length`: the instruction replaced by the detour; `executed`: its bytes the stub runs (a ret before padding is 1).
SITES = [
    ("god_hp", "God Mode", "EntityComponents.Vitals", "OffsetHp", "void (float amount)",
     hexpattern("c7 43 44 00 00 00 00 c5 fa 58 53 40 85 c8"), 7, 5, 5, None, False),
    ("god_grey", "God Mode", "EntityComponents.Vitals", "OffsetGreyHp", "void (float amount)",
     hexpattern("c5 fa 11 45 ?? c5 fa 58 43 44 c5 f2 5d c8"), 5, 5, 5, None, False),
    ("spirit", "Infinite Spartan Spirit", "EntityComponents.SpartanSpirit", "OffsetSpartanSpirit",
     "void (float amount, bool fromRegeneration)",
     hexpattern("c5 f8 28 c8 c5 f2 58 43 20 c5 fa 10 4b 24"), 4, 5, 5, None, False),
    ("magic", "Infinite Magic", "EntityComponents.ManaState", "OffsetMana", "void (float amount, bool fromRegeneration)",
     hexpattern("c5 f8 55 c1 c5 fa 11 43 28 48 8b 7b 38"), 4, 5, 5, None, False),
    ("damage", "Damage Multiplier", "EntityComponents.Hittable", "ExecuteHitEvent", None,
     hexpattern("48 8b 43 28 48 85 c0 0f 84 ?? ?? ?? ?? 48 8b 78 10 48 85 ff 74 ?? 48 8b 07 48 8b 73 30 "
                "48 8b 90 80 01 00 00 ff 90 78 01 00 00"), 36, 6, 6, None, False),
    ("movement", "Movement Speed", "PlayerMovement", "TickPseudoPhysics", None,
     hexpattern("c5 fb 10 83 e0 00 00 00 48 89 df 31 f6 e8 ?? ?? ?? ??"), 13, 5, 5, "slope_aligned", False),
    ("jump", "Jump Height", "<DelayStartJumping>d__27", "MoveNext", None,
     hexpattern("c5 fa 10 83 9c 00 00 00 4c 8b 73 58 48 89 df e8 ?? ?? ?? ?? 80 3d ?? ?? ?? ?? 00"), 20, 7, 7,
     "jump_angle", True),
    ("wallet_tick", "Item actions", "PlayerController", "Update", "void ()",
     hexpattern("c3 00 00 00 00"), 0, 5, 1, None, False),
]

PRIMITIVES = {1: "void", 2: "bool", 3: "char", 4: "sbyte", 5: "byte", 6: "short", 7: "ushort", 8: "int", 9: "uint",
              10: "long", 11: "ulong", 12: "float", 13: "double", 14: "string", 0x18: "IntPtr", 0x1C: "object"}


def fnv1a(data, value=FNV_OFFSET):
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


class Build:
    """One build's code and metadata, through scripts/il2cpp_index.py."""

    def __init__(self, version):
        import il2cpp_index
        self.version = version
        self.index = il2cpp_index.open_build(version)
        if self.index is None:
            raise LookupError("boot image or metadata not installed")
        self.boot = self.index.boot
        self.index.all_method_names()
        self.methods = {}
        for type_index in range(len(self.index.types)):
            type_name = self.index.type_name(type_index)
            image = self.index.image_names[self.index.image_of_type[type_index]]
            for method_index, method in self.index.type_methods(type_index):
                name = self.index.meta.string(method[0])
                self.methods.setdefault((image, type_name, name), []).append(method_index)

    def type_name_of(self, type_index):
        index, boot = self.index, self.boot
        types = boot.pointer(index.metadata_registration + 0x38)
        count = boot.u64(index.metadata_registration + 0x30)
        if not 0 <= type_index < count:
            return "?"
        record = boot.pointer(types + 8 * type_index)
        data, bits = boot.u64(record), boot.u32(record + 8)
        kind = (bits >> 16) & 0xFF
        if kind in PRIMITIVES:
            return PRIMITIVES[kind]
        if kind in (0x11, 0x12):
            return index.type_name(data & 0xFFFFFFFF)
        return "generic" if kind == 0x15 else f"kind{kind:#x}"

    def signature(self, method_index):
        method = self.index.methods[method_index]
        parameters = self.index.meta.records("parameters", "<3i")
        arguments = [f"{self.type_name_of(parameters[method[4] + k][2])} {self.index.meta.string(parameters[method[4] + k][0])}"
                     for k in range(method[10])]
        static = "static " if method[7] & 0x10 else ""
        return f"{static}{self.type_name_of(method[2])} ({', '.join(arguments)})"

    def method(self, type_name, name, signature, image="Assembly-CSharp.dll"):
        found = []
        for method_index in self.methods.get((image, type_name, name), []):
            if signature is None or self.signature(method_index) == signature:
                address = self.index._address(method_index)
                if address:
                    found.append(address)
        if len(found) != 1:
            raise LookupError(f"{type_name}::{name} {signature or ''}: {len(found)} matches")
        return found[0]

    def bounds(self, address):
        return self.index.method_bounds(address)

    def read(self, address, size):
        return bytes(self.boot.read(address, size))

    def field(self, type_name, field, image):
        index = self.index
        matches = [t for t in index.type_indices(type_name, image)]
        if len(matches) != 1:
            raise LookupError(f"{type_name}: {len(matches)} types")
        t = index.types[matches[0]]
        table = self.boot.pointer(index.metadata_registration + 0x58)
        base = self.boot.pointer(table + 8 * matches[0])
        for k in range(t[18]):
            if index.meta.string(index.fields[t[8] + k][0]) == field:
                return self.boot.u32(base + 4 * k)
        raise LookupError(f"{type_name}.{field}")

    def relocated(self, begin, end):
        relocs = self.boot.relocs
        return [a for a in relocs if begin - 8 < a < end]

    def module_base(self):
        return max(self.boot.modules, key=lambda m: m[1])[0]

    def image_size(self):
        return self.boot.size


def find_all(code, pattern):
    hits = []
    first = pattern[0]
    start = 0
    while True:
        at = code.find(bytes([first]), start) if first is not None else start
        if at < 0 or at + len(pattern) > len(code):
            return hits
        if all(p is None or code[at + i] == p for i, p in enumerate(pattern)):
            hits.append(at)
        start = at + 1


_PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}
_MODRM_1 = set(range(0x00, 0x04)) | set(range(0x08, 0x0C)) | set(range(0x10, 0x14)) | set(range(0x18, 0x1C)) | \
    set(range(0x20, 0x24)) | set(range(0x28, 0x2C)) | set(range(0x30, 0x34)) | set(range(0x38, 0x3C)) | \
    {0x63, 0x69, 0x6B, 0x80, 0x81, 0x83} | set(range(0x84, 0x90)) | {0xC0, 0xC1, 0xC6, 0xC7} | \
    set(range(0xD0, 0xD4)) | set(range(0xD8, 0xE0)) | {0xF6, 0xF7, 0xFE, 0xFF}
_IMM8_1 = {0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x6A, 0x6B, 0x80, 0x83, 0xA8, 0xC0, 0xC1, 0xC6, 0xCD,
           0xE4, 0xE5, 0xE6, 0xE7} | set(range(0xB0, 0xB8)) | set(range(0x70, 0x80)) | {0xEB} | set(range(0xE0, 0xE4))
_IMMZ_1 = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0x68, 0x69, 0x81, 0xA9, 0xC7}
_NO_MODRM_0F = {0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x37, 0x77, 0xA0, 0xA1, 0xA2,
                0xA8, 0xA9, 0xAA} | set(range(0x80, 0x90)) | set(range(0xC8, 0xD0))
_IMM8_0F = {0x70, 0x71, 0x72, 0x73, 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6}


def _modrm_length(code, at, address_32=False):
    modrm = code[at]
    mod, rm = modrm >> 6, modrm & 7
    length = 1
    if mod != 3 and rm == 4:
        sib = code[at + 1]
        length += 1
        if mod == 0 and (sib & 7) == 5:
            length += 4
    if mod == 0 and rm == 5:
        length += 4
    elif mod == 1:
        length += 1
    elif mod == 2:
        length += 4
    return length


def decode(code, at):
    """(length, branch target offset or None) of the instruction at `at`; the branch is a rel8/rel32 jump or call."""
    start = at
    operand_16 = False
    while code[at] in _PREFIXES:
        operand_16 |= code[at] == 0x66
        at += 1
    rex_w = False
    if 0x40 <= code[at] <= 0x4F:
        rex_w = bool(code[at] & 8)
        at += 1
    op = code[at]
    if op in (0xC4, 0xC5):
        if op == 0xC5:
            mmmmm, at = 1, at + 2
        else:
            mmmmm, at = code[at + 1] & 0x1F, at + 3
        opcode = code[at]
        at += 1
        at += _modrm_length(code, at)
        if mmmmm == 3 or (mmmmm == 1 and opcode in (0x70, 0x71, 0x72, 0x73, 0xC2, 0xC4, 0xC5, 0xC6)):
            at += 1
        return at - start, None
    if op == 0x0F:
        op2 = code[at + 1]
        at += 2
        if op2 == 0x38:
            at += 1
            at += _modrm_length(code, at)
            return at - start, None
        if op2 == 0x3A:
            at += 1
            at += _modrm_length(code, at) + 1
            return at - start, None
        if 0x80 <= op2 <= 0x8F:
            target = at + 4 + struct.unpack_from("<i", code, at)[0]
            return at + 4 - start, target
        if op2 not in _NO_MODRM_0F:
            at += _modrm_length(code, at)
        if op2 in _IMM8_0F:
            at += 1
        return at - start, None
    at += 1
    if op in (0xE8, 0xE9):
        target = at + 4 + struct.unpack_from("<i", code, at)[0]
        return at + 4 - start, target
    if 0x70 <= op <= 0x7F or op == 0xEB or 0xE0 <= op <= 0xE3:
        target = at + 1 + struct.unpack_from("<b", code, at)[0]
        return at + 1 - start, target
    reg = (code[at] >> 3) & 7 if op in _MODRM_1 else 0
    if op in _MODRM_1:
        at += _modrm_length(code, at)
    if op in _IMM8_1:
        at += 1
    if op in _IMMZ_1:
        at += 2 if operand_16 else 4
    if 0xB8 <= op <= 0xBF:
        at += 8 if rex_w else (2 if operand_16 else 4)
    if 0xA0 <= op <= 0xA3:
        at += 8
    if op == 0xF6 and reg in (0, 1):
        at += 1
    if op == 0xF7 and reg in (0, 1):
        at += 2 if operand_16 else 4
    if op in (0xC2, 0xCA):
        at += 2
    if op == 0xC8:
        at += 3
    return at - start, None


def sweep(code):
    """Instruction starts and branch targets (offsets into `code`) of a linear sweep of one method."""
    starts, targets = set(), set()
    at = 0
    while at < len(code):
        try:
            length, target = decode(code, at)
        except IndexError:
            break
        starts.add(at)
        if target is not None:
            targets.add(target)
        at += length
    return starts, targets


def rip_displacement(instruction):
    """Offset of the rip-relative disp32 in the instructions the sites use, -1 for none."""
    if instruction[:2] == b"\x80\x3d":
        return 2
    return -1


def string_new(version, build):
    import ps4_module
    prx = evidence.game_file(version, f"Media/Modules/{GAME_MODULE}")
    if not prx:
        raise LookupError(f"{GAME_MODULE} not installed")
    module = ps4_module.read_module(Path(prx))
    exported = [s for s in module["symbols"] if s.get("section") and s["name"].split("#")[0] == STRING_NEW_NID]
    if len(exported) != 1:
        raise LookupError(f"il2cpp_string_new: {len(exported)} exports")
    return build.module_base() + exported[0]["value"]


def item_ids(version):
    """Each item id the actions add, found in the asset files the build loads (an update without them keeps the base's)."""
    found = {}
    for relative in ITEM_FILES:
        path = evidence.game_file(version, relative)
        if not path:
            continue
        data = Path(path).read_bytes()
        for item in [BLOOD_ORB] + UPGRADE_MATERIALS:
            needle = struct.pack("<I", len(item)) + item.encode()
            if needle in data:
                found.setdefault(item, relative)
    return found


def locate(version):
    build = Build(version)
    result = {"version": version, "image_size": build.image_size(), "sites": {}, "functions": {}, "fields": {}}
    for key, type_name, name, signature, image in FUNCTIONS:
        address = build.method(type_name, name, signature, image)
        begin, end = build.bounds(address)
        if build.relocated(begin, end):
            raise ValueError(f"{key}: relocations inside the method")
        result["functions"][key] = (address, end, fnv1a(build.read(begin, end - begin)))
    for key, type_name, field, expected, image in FIELDS:
        offset = build.field(type_name, field, image)
        if offset != expected:
            raise ValueError(f"{type_name}.{field} at {offset:#x}, the handlers read {expected:#x}")
        result["fields"][key] = offset
    for key, cheat, type_name, name, signature, pattern, at, length, executed, call, post in SITES:
        method = build.method(type_name, name, signature)
        begin, end = build.bounds(method)
        code = build.read(begin, end - begin)
        if build.relocated(begin, end):
            raise ValueError(f"{key}: relocations inside {type_name}::{name}")
        hits = find_all(code, pattern)
        if len(hits) != 1:
            raise ValueError(f"{key}: pattern found {len(hits)} times in {type_name}::{name}")
        site = begin + hits[0] + at
        if site % ATOMIC_BLOCK + length > ATOMIC_BLOCK:
            raise ValueError(f"{key}: {site:#x}+{length} crosses an aligned 16-byte block")
        if call:
            call_at = begin + hits[0] + (at if not post else at - 5)
            target = call_at + 5 + struct.unpack_from("<i", build.read(call_at + 1, 4))[0]
            if target != result["functions"][call][0]:
                raise ValueError(f"{key}: the call does not reach {call}")
        if executed < length and any(build.read(site + executed, length - executed)):
            raise ValueError(f"{key}: the bytes after the executed instruction are not padding")
        if executed < length and end - site < length:
            raise ValueError(f"{key}: the padding ends before the detour")
        starts, targets = sweep(code)
        offset = site - begin
        if offset not in starts or decode(code, offset)[0] != executed:
            raise ValueError(f"{key}: {site:#x} is not one whole {executed}-byte instruction of {type_name}::{name}")
        if any(offset < t < offset + length for t in targets) or any(offset < s < offset + executed for s in starts):
            raise ValueError(f"{key}: a branch lands inside the replaced instruction")
        if post and offset in targets:
            raise ValueError(f"{key}: the instruction after the call is also a branch target")
        original = build.read(site, length)
        result["sites"][key] = {
            "cheat": cheat, "rva": site, "length": length, "executed": executed, "original": original,
            "rip": rip_displacement(original[:executed]), "rel": 1 if original[0] in (0xE8, 0xE9) and executed == 5 else 0,
            "method": (begin, end), "hash": fnv1a(code),
        }
    result["string_new"] = string_new(version, build)
    if build.relocated(result["string_new"], result["string_new"] + 32):
        raise ValueError("il2cpp_string_new: relocations inside")
    result["string_new_hash"] = fnv1a(build.read(result["string_new"], 32))
    ids = item_ids(version)
    missing = [item for item in [BLOOD_ORB] + UPGRADE_MATERIALS if item not in ids]
    if missing:
        raise ValueError(f"item ids not in the build's assets: {', '.join(missing)}")
    result["items"] = ids
    return result


def suffix(version):
    return version.replace(".", "_")


def render(results):
    lines = ["/* Generated by scripts/cheat_sites.py from the CUSA57767 boot images. */",
             "#ifndef SOS_CHEAT_SITES_H", "#define SOS_CHEAT_SITES_H", "", '#include "sos_cheat_patch.h"', ""]
    for version in BUILDS:
        info = results[version]
        name = suffix(version)
        lines.append(f"static const SosCheatBuild kCheatBuild_{name} = {{")
        lines.append(f'    "CUSA57767-{version}", 0x{info["image_size"]:x}ull,')
        lines.append("    {")
        for key, *_ in SITES:
            s = info["sites"][key]
            original = ", ".join(f"0x{b:02x}" for b in s["original"])
            lines.append(f"        {{0x{s['rva']:x}u, {s['length']}, {s['executed']}, {s['rip']}, {s['rel']}, {{{original}}}, "
                         f"{{0x{s['method'][0]:x}u, 0x{s['method'][1]:x}u}}, 0x{s['hash']:016x}ull}}, /* {key} */")
        lines.append("    },")
        lines.append("    {")
        for key, *_ in FUNCTIONS:
            address, end, value = info["functions"][key]
            lines.append(f"        {{0x{address:x}u, 0x{end:x}u, 0x{value:016x}ull}}, /* {key} */")
        lines.append("    },")
        lines.append(f"    0x{info['string_new']:x}u, 0x{info['string_new_hash']:016x}ull,")
        lines.append("};")
        lines.append("")
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def parse_header(text):
    """The table of each build in the header: what the contract and the check compare."""
    builds = {}
    for match in re.finditer(r'kCheatBuild_(\d+)_(\d+) = \{\s*"CUSA57767-([\d.]+)", 0x([0-9a-f]+)ull,(.*?)\n\};', text, re.S):
        version = match.group(3)
        body = match.group(5)
        sites = re.findall(r"\{0x([0-9a-f]+)u, (\d+), (\d+), (-?\d+), (\d), \{([^}]*)\}, \{0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}, "
                           r"0x([0-9a-f]+)ull\}, /\* (\w+) \*/", body)
        functions = re.findall(r"\{0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)ull\}, /\* (\w+) \*/", body)
        string_new_match = re.search(r"\n    0x([0-9a-f]+)u, 0x([0-9a-f]+)ull,\s*$", body)
        builds[version] = {
            "image_size": int(match.group(4), 16),
            "sites": {s[9]: {"rva": int(s[0], 16), "length": int(s[1]), "executed": int(s[2]), "rip": int(s[3]),
                             "rel": int(s[4]), "original": bytes(int(b, 16) for b in s[5].split(",")),
                             "method": (int(s[6], 16), int(s[7], 16)), "hash": int(s[8], 16)} for s in sites},
            "functions": {f[3]: (int(f[0], 16), int(f[1], 16), int(f[2], 16)) for f in functions},
            "string_new": int(string_new_match.group(1), 16) if string_new_match else None,
        }
    return builds


def contract_problems(builds):
    """What the source table alone must satisfy, with no game data."""
    problems = []
    if sorted(builds) != sorted(BUILDS):
        problems.append(f"builds {sorted(builds)}, expected {list(BUILDS)}")
    for version, info in builds.items():
        if set(info["sites"]) != {s[0] for s in SITES}:
            problems.append(f"{version}: sites {sorted(info['sites'])}")
        if set(info["functions"]) != {f[0] for f in FUNCTIONS}:
            problems.append(f"{version}: functions {sorted(info['functions'])}")
        spans = []
        for key, s in info["sites"].items():
            if s["rva"] % ATOMIC_BLOCK + s["length"] > ATOMIC_BLOCK:
                problems.append(f"{version} {key}: not inside one aligned 16-byte block")
            if not 5 <= s["length"] <= 15 or not 1 <= s["executed"] <= s["length"] or len(s["original"]) != s["length"]:
                problems.append(f"{version} {key}: bad length")
            if not s["method"][0] <= s["rva"] < s["rva"] + s["length"] <= s["method"][1]:
                problems.append(f"{version} {key}: outside its method")
            if s["rva"] + s["length"] > info["image_size"]:
                problems.append(f"{version} {key}: outside the image")
            spans.append((s["rva"], s["rva"] + s["length"], key))
        spans.sort()
        for (b1, e1, k1), (b2, e2, k2) in zip(spans, spans[1:]):
            if b2 < e1:
                problems.append(f"{version}: {k1} and {k2} overlap")
    if len(builds) == 2:
        a, b = (builds[v] for v in BUILDS)
        same = [k for k in a["sites"] if k in b["sites"] and a["sites"][k]["rva"] == b["sites"][k]["rva"]]
        if same:
            problems.append(f"1.00 and 1.01 share site offsets: {same}")
        if a["image_size"] == b["image_size"]:
            problems.append("1.00 and 1.01 have the same image size")
    return problems


def compare(expected, header):
    problems = []
    if expected["image_size"] != header["image_size"]:
        problems.append("image size")
    for key, s in expected["sites"].items():
        h = header["sites"].get(key)
        if not h or any(h[f] != s[f] for f in ("rva", "length", "executed", "rip", "rel", "original", "method", "hash")):
            problems.append(f"site {key}")
    for key, value in expected["functions"].items():
        if header["functions"].get(key) != value:
            problems.append(f"function {key}")
    if header["string_new"] != expected["string_new"]:
        problems.append("il2cpp_string_new")
    return problems


def check():
    text = HEADER.read_text()
    builds = parse_header(text)
    problems = contract_problems(builds)
    for version in BUILDS:
        evidence.report_contract(version, f"cheat sites: {len(SITES)} sites, {len(FUNCTIONS)} functions, {len(FIELDS)} fields")
    failed = bool(problems)
    for p in problems:
        print(f"  [fail] contract: {p}")
    for version in BUILDS:
        try:
            expected = locate(version)
        except LookupError as e:
            evidence.report("image", version, evidence.NO_DATA, f"cheat sites not read from the real code: {e}")
            continue
        except ValueError as e:
            print(f"  [fail] {version}: {e}")
            failed = True
            continue
        evidence.report_image(version, "cheat sites, functions, fields and item ids")
        diff = compare(expected, builds.get(version, {"image_size": 0, "sites": {}, "functions": {}, "string_new": None}))
        if diff:
            print(f"  [fail] CUSA57767 {version}: header differs from the boot image: {', '.join(diff)}")
            failed = True
        else:
            print(f"  [ok]   CUSA57767 {version}: {len(SITES)} sites, {len(FUNCTIONS)} functions, {len(FIELDS)} fields and "
                  f"{1 + len(UPGRADE_MATERIALS)} item ids match the real boot image")
    print("  [ok]   cheat site contract" if not problems else "  [fail] cheat site contract")
    return 1 if failed else 0


def self_test():
    builds = parse_header(HEADER.read_text())
    failures = 0

    def refused(what, mutate):
        nonlocal failures
        import copy
        copied = copy.deepcopy(builds)
        mutate(copied)
        if contract_problems(copied):
            print(f"  [ok]   refused: {what}")
        else:
            print(f"  [fail] accepted: {what}")
            failures += 1

    refused("a build missing", lambda b: b.pop("1.01"))
    refused("an unknown build", lambda b: b.update({"1.02": b["1.00"]}))
    refused("a site missing", lambda b: b["1.00"]["sites"].pop("god_hp"))
    refused("a site crossing a 16-byte block", lambda b: b["1.00"]["sites"]["damage"].update(rva=b["1.00"]["sites"]["damage"]["rva"] | 0xC))
    refused("1.00 site offsets used for 1.01", lambda b: b["1.01"].update(sites=b["1.00"]["sites"]))
    refused("overlapping sites", lambda b: b["1.00"]["sites"]["god_grey"].update(rva=b["1.00"]["sites"]["god_hp"]["rva"] + 2))
    refused("a function missing", lambda b: b["1.01"]["functions"].pop("is_player"))
    refused("a site outside its method", lambda b: b["1.00"]["sites"]["magic"].update(method=(0, 1)))
    print(f"Cheat site self-test: {8 - failures} of 8 mutations refused")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", nargs="?", default="check", choices=["generate", "check"])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.mode == "generate":
        results = {version: locate(version) for version in BUILDS}
        HEADER.write_text(render(results), newline="\n")
        for version in BUILDS:
            print(f"CUSA57767 {version}: " + ", ".join(f"{k} {v['rva']:#x}" for k, v in results[version]["sites"].items()))
        return 0
    return check()


if __name__ == "__main__":
    sys.exit(main())
