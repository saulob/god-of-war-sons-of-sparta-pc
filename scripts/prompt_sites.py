"""Controller prompt patch sites of the built boot images: generates launcher/controller_prompts_sites.h and proves, from the
game's metadata and code, that the sites are every place that writes or reads InputOverseer.m_currentControllerUsed.

    python scripts/prompt_sites.py            check the installed builds against the header and the game code
    python scripts/prompt_sites.py generate   print the header for the installed builds

Nothing is executed; a build without a boot image or metadata is skipped. Exit status 0 when every check passes.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from il2cpp_index import ROOT, open_build  # noqa: E402

HEADER = ROOT / "launcher" / "controller_prompts_sites.h"
HOOK_METHOD = ("InputOverseer", "ResolveIconsForCurrentInput")
HOOK_PROLOGUE = bytes([0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56])
HOOK_STOLEN = 6
FNV_OFFSET, FNV_PRIME = 0xCBF29CE484222325, 0x100000001B3
BUILDS = ("1.00", "1.01")
CONTEXT = 8
OVERSEER = "InputOverseer"

WRITERS = [
    (OVERSEER, ".ctor", rb"\xc7[\x40-\x47]\x34[\x03\x04]\x00\x00\x00", 4),
    (OVERSEER, "Initialize", rb"\xc7[\x40-\x47]\x34[\x03\x04]\x00\x00\x00", 4),
    (OVERSEER, "DetermineController", rb"\xc7[\x40-\x47]\x34[\x03\x04]\x00\x00\x00", 4),
    (OVERSEER, "OnInputActionChange", rb"\x41\xc7[\x00-\x07][\x03\x04]\x00\x00\x00", 4),
]
READERS = [
    (OVERSEER, "GetTextSpriteAsset"),
    (OVERSEER, "ResolveIconsForCurrentInput"),
    (OVERSEER, "SetTextWithIcons"),
    ("UIControlTypeListener", "OnEnable"),
    ("UIControlTypeListener", "OnDisable"),
    ("UIControlTypeListener", "IsDualSenseController"),
    ("UIControlTypeListener", "OnControllerDeviceChanged"),
    ("UIControlTypeListener", "GetControllerType"),
    ("UiScripts.Widgets.WidgetKeybindElement", "GetYOffsetForController"),
    ("UiScripts.Widgets.WidgetKeybindElement", "GetXOffsetForController"),
]
READ_PATTERN = re.compile(rb"[\x40-\x4f]?\x8b[\x40-\x7f]\x34")
READ_ANY = re.compile(rb"(?:[\x40-\x4f]?(?:\x8b|\x3b|\x39)[\x40-\x7f]|\x83[\x78-\x7f])\x34")
STORE_ANY = re.compile(rb"(?:\xc7[\x40-\x47]\x34[\x00-\x07]\x00\x00\x00|\x89[\x40-\x7f]\x34)")
READS_NOT_PATCHED = {OVERSEER + "::OnInputActionChange", OVERSEER + "::get_CurrentController"}
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def read(index, address, size):
    return bytes(index.boot.read(address, size))


def body(index, type_name, method_name):
    address = index.method_address(type_name, method_name)
    start, end = index.method_bounds(address)
    return start, read(index, start, end - start)


def pop_into(register, length):
    push = b"\x6a\x02" + (b"\x41" if register >= 8 else b"") + bytes([0x58 + (register & 7)])
    return push + b"\x90" * (length - len(push))


def reader_replacement(original):
    rex = original[0] if original[0] & 0xF0 == 0x40 else 0
    modrm = original[len(original) - 2]
    register = ((modrm >> 3) & 7) | (8 if rex & 4 else 0)
    return pop_into(register, len(original))


def site_table(index):
    sites = []
    for type_name, method, pattern, _ in WRITERS:
        start, data = body(index, type_name, method)
        found = list(re.finditer(pattern, data))
        if len(found) != 1:
            raise LookupError(f"{type_name}::{method}: {len(found)} controller stores")
        at = found[0].end() - 4
        sites.append((f"{type_name}::{method}", start + at, data[at:at + 1], b"\x02", 1))
    for type_name, method in READERS:
        start, data = body(index, type_name, method)
        found = list(READ_PATTERN.finditer(data))
        if not found:
            raise LookupError(f"{type_name}::{method}: no controller read")
        original = found[0].group(0)
        sites.append((f"{type_name}::{method}", start + found[0].start(), original, reader_replacement(original), 0))
    table = []
    for name, offset, original, xbox, immediate in sites:
        before = read(index, offset - CONTEXT, CONTEXT)
        after = read(index, offset + len(original), CONTEXT)
        table.append((name, offset, original, xbox, before, after, immediate))
    return table


def fnv64(data):
    value = FNV_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def hook_record(index):
    """The prompt hook's method: start, length up to the next method, FNV-1a of those bytes and the first 8 bytes."""
    start, data = body(index, *HOOK_METHOD)
    return start, len(data), fnv64(data), data[:8]


def format_hook(build, record, index):
    start, length, digest, prologue = record
    device = index.field_offset(OVERSEER, "m_currentInputDevice")
    used = index.field_offset(OVERSEER, "m_currentControllerUsed")
    return (f"/* CUSA57767-{build}: {OVERSEER}::{HOOK_METHOD[1]} */\n"
            f"static const SosPromptHook kHook_{build.replace('.', '_')} = {{0x{start:x}u, 0x{length:x}u, 0x{digest:016x}ull, "
            f"{hexbytes(prologue, 8)}, 0x{device:x}, 0x{used:x}}};")


def hexbytes(data, width):
    return "{" + ", ".join(f"0x{b:02x}" for b in data) + ", 0" * (width - len(data)) + "}"


def format_block(build, table):
    lines = [f"/* CUSA57767-{build} */", f"static const SosPromptSite kSites_{build.replace('.', '_')}[] = {{"]
    for name, offset, original, xbox, before, after, immediate in table:
        lines.append(f"    {{0x{offset:x}u, {len(original)}, {immediate}, {hexbytes(before, CONTEXT)}, "
                     f"{hexbytes(original, 4)}, {hexbytes(xbox, 4)}, {hexbytes(after, CONTEXT)}}}, /* {name} */")
    lines.append("};")
    return "\n".join(lines)


def header_text(blocks):
    return ("/* Generated by scripts/prompt_sites.py: the places of each build that write or read the game's controller type\n"
            " * (InputOverseer.m_currentControllerUsed): offset, length, whether the bytes are a plain immediate, the bytes\n"
            " * before, the original bytes, the Xbox bytes and the bytes after; and the method the Automatic prompts hook:\n"
            " * offset, length, FNV-1a of its bytes, its first 8 bytes and the InputOverseer field offsets. */\n" +
            "\n\n".join(blocks) + "\n")


def lookup_slot_and_target(index):
    """The static slot holding typeof(InputOverseer) and the shared subsystem lookup, as UIControlTypeListener uses them."""
    start, data = body(index, "UIControlTypeListener", "GetControllerType")
    slot = None
    for match in re.finditer(rb"[\x48\x4c][\x8b\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", data):
        disp = struct.unpack_from("<i", data, match.start() + 3)[0]
        if re.search(rb"\x83[\x78-\x7f]\x38\x00", data[match.start() + 7:match.start() + 31]):
            slot = start + match.start() + 7 + disp
            break
    target = None
    for match in re.finditer(rb"\x48\x8b\x30\xe8", data):
        target = start + match.end() + 4 + struct.unpack_from("<i", data, match.end())[0]
    return slot, target


def fetching_methods(index, slot, target):
    names = index.all_method_names()
    start, code = index.game_code()
    found = {}
    for address in sorted(names):
        if not start <= address < start + len(code):
            continue
        low, high = index.method_bounds(address)
        data = code[low - start:high - start]
        calls = (m for m in re.finditer(rb"\xe8", data) if m.start() + 5 <= len(data))
        if not any(low + m.start() + 5 + struct.unpack_from("<i", data, m.start() + 1)[0] == target for m in calls):
            continue
        refs = (m for m in re.finditer(rb"[\x48\x4c][\x8b\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", data)
                if m.start() + 7 <= len(data))
        if any(low + m.start() + 7 + struct.unpack_from("<i", data, m.start() + 3)[0] == slot for m in refs):
            found[address] = (low, data)
    return found


def holder_types(index):
    """Types with a field that holds an InputOverseer."""
    holders = set()
    for type_index, t in enumerate(index.types):
        for field in range(t[8], t[8] + t[18]):
            if "nputOverseer" in index.meta.string(index.fields[field][0]):
                holders.add(index.type_name(type_index))
    return holders


def check_build(build):
    index = open_build(build)
    if index is None:
        print(f"  [skip] CUSA57767-{build}: no boot image or metadata installed")
        return None
    print(f"CUSA57767-{build}")
    values = index.enum_values("GamepadType")
    check(values.get("XboxOne") == 1 and values.get("XboxSeries") == 2 and values.get("PS4") == 3 and values.get("PS5") == 4,
          f"GamepadType: XboxOne=1, XboxSeries=2, PS4=3, PS5=4 (metadata says {values})")
    check(index.field_offset(OVERSEER, "m_currentControllerUsed") == 0x34 and
          index.field_offset(OVERSEER, "m_currentInputDevice") == 0x30, "controller type at +0x34, input device at +0x30")
    table = site_table(index)
    block = format_block(build, table)
    stored = HEADER.read_text() if HEADER.is_file() else ""
    check(block in stored, f"launcher/controller_prompts_sites.h holds the sites of this build ({len(table)})")
    check_hook(index, build, stored)

    slot, target = lookup_slot_and_target(index)
    if not check(slot is not None and target is not None, "the subsystem lookup of InputOverseer is found"):
        return table
    fetchers = fetching_methods(index, slot, target)
    names = index.all_method_names()
    readers = {f"{t}::{m}" for t, m in READERS}
    seen = set()
    stores = []
    for address, (low, data) in fetchers.items():
        for match in READ_ANY.finditer(data):
            seen.add(names[address])
        stores += [(names[address], low + m.start()) for m in STORE_ANY.finditer(data)]
    own = [a for a, n in names.items() if n.startswith(OVERSEER + "::")]
    holders = holder_types(index)
    own += [a for a, n in names.items() if n.rsplit("::", 1)[0] in holders]
    for address in own:
        low, high = index.method_bounds(address)
        data = read(index, low, high - low)
        if READ_ANY.search(data):
            seen.add(names[address])
        stores += [(names[address], low + m.start()) for m in STORE_ANY.finditer(data)]
    stores = [s for s in stores if s[0] != OVERSEER + "::set_CurrentController"]
    store_sites = {row[0] for row in table[:len(WRITERS)]}
    check(readers <= seen and seen - readers <= READS_NOT_PATCHED,
          f"every reader of the controller type is patched ({len(fetchers)} methods fetch InputOverseer, {len(holders)} types "
          f"hold one; extra: "
          f"{sorted(seen - readers - READS_NOT_PATCHED)})")
    check({n for n, _ in stores} <= store_sites, f"no other store to the controller type in InputOverseer or its users "
          f"(extra: {sorted({n for n, _ in stores} - store_sites)})")
    setter = index.method_address(OVERSEER, "set_CurrentController")
    module_code = read(index, index.module_base, index.module_size)
    direct = [m for m in re.finditer(rb"[\xe8\xe9]", module_code)
              if m.start() + 5 <= len(module_code) and
              index.module_base + m.start() + 5 + struct.unpack_from("<i", module_code, m.start() + 1)[0] == setter]
    check(not direct, f"set_CurrentController has no caller ({len(direct)} direct calls)")
    return table


def check_hook(index, build, stored):
    """The Automatic prompts hook: the method's prologue, boundaries and entry, against the header and the game code."""
    record = hook_record(index)
    start, length, _, prologue = record
    check(format_hook(build, record, index) in stored, "launcher/controller_prompts_sites.h holds the prompt hook of this build")
    check(prologue == HOOK_PROLOGUE, "the hook method starts with push rbp; mov rbp,rsp; push r15; push r14 (6 whole bytes stolen)")
    names = index.all_method_names()
    check(names.get(start) == f"{HOOK_METHOD[0]}::{HOOK_METHOD[1]}", "the hook address is the start of the method")
    next_start = min((a for a in names if a > start), default=None)
    check(next_start == start + length, "the hook method ends at the next method's start")
    method = index.methods[next(m for t in index.type_indices(HOOK_METHOD[0]) for m, row in index.type_methods(t)
                                if index.meta.string(row[0]) == HOOK_METHOD[1])]
    check(method[10] == 0, f"the hook method takes no argument besides this (parameter count {method[10]})")
    module_code = read(index, index.module_base, index.module_size)
    entries = [index.module_base + m.start() for m in re.finditer(rb"[\xe8\xe9]", module_code)
               if m.start() + 5 <= len(module_code)
               and start < index.module_base + m.start() + 5 + struct.unpack_from("<i", module_code, m.start() + 1)[0] < start + HOOK_STOLEN]
    check(not entries, f"no direct call or jump lands inside the stolen bytes ({len(entries)} found)")
    values = index.enum_values("ControlDevice")
    check(values.get("Gamepad") == 2, f"ControlDevice.Gamepad is 2 (metadata says {values})")


def main(argv):
    if len(argv) > 1 and argv[1] == "generate":
        blocks = []
        for build in BUILDS:
            index = open_build(build)
            if index:
                blocks.append(format_block(build, site_table(index)))
        for build in BUILDS:
            index = open_build(build)
            if index:
                blocks.append(format_hook(build, hook_record(index), index))
        print(header_text(blocks), end="")
        return 0
    for build in BUILDS:
        check_build(build)
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
