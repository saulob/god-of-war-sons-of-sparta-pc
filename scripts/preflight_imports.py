#!/usr/bin/env python3
"""Offline import preflight for prepared PS4 builds.

Scans every prepared PRX's real ELF import table and its PLT/GOT call sites, then checks the
runtime's actual resolver inputs: import_names.inc NID mappings, name-keyed RuntimeExport tables,
NID-keyed resolver tables and generated PS4 NIDs. A called import with no resolver input is
reported as blocking, so a missing HLE export is visible before the game reaches it.

    python scripts/preflight_imports.py [--build CUSA57767-1.01] [--module Media/Plugins/SaveData.prx]
                                        [--all] [--src src] [--report out.txt] [--check]
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ps4_module import nid as ps4_nid, read_module  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NATIVE = {"libc", "libkernel", "libSceLibcInternal", "libSceFios2"}
CRITICAL_MODULES = {"Media/Plugins/SaveData.prx"}
REQUIRED_NIDS = {"VwadwBBBJ80#libSceSaveData": "sceSaveDataUmountWithBackup"}
OPTIONAL_MISSING = {
    ("Media/Plugins/SaveData.prx", "lU9YRFsgwSU"): "destructive backup restore UI, not on the title save path",
}


def read(path):
    try:
        return path.read_text(errors="ignore")
    except OSError:
        return ""


def load_names(src):
    names = {}
    for line in read(src / "import_names.inc").splitlines():
        match = re.search(r'\{"([^"]+)","([^"]+)"\}', line)
        if match:
            names[match.group(1)] = match.group(2)
    return names


def load_exports(src):
    names, nids = set(), set()
    paths = list((src).glob("*.c")) + list((src).glob("*.cpp"))
    for path in paths:
        text = read(path)
        for block in re.finditer(r'static const RuntimeExport\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};', text, re.S):
            for entry in re.finditer(r'\{\s*"([A-Za-z_]\w*)",\s*[A-Za-z_]\w*\s*\}', block.group(1)):
                names.add(entry.group(1))
        for entry in re.finditer(r'\{\s*"([A-Za-z0-9+\-_]{11}#lib[A-Za-z0-9_]+)",\s*[A-Za-z_]\w*\s*\}', text):
            nids.add(entry.group(1))
        for entry in re.finditer(r'strcmp\(name,\s*"([A-Za-z0-9+\-_]{11}#lib[A-Za-z0-9_]+)"\)', text):
            nids.add(entry.group(1))
    gpu = ROOT / "gpu"
    for path in gpu.rglob("*"):
        if path.suffix not in (".cpp", ".h", ".c"):
            continue
        text = read(path)
        for entry in re.finditer(r'LIB_FUNCTION\(\s*"([A-Za-z0-9+\-_]{11})"', text):
            nids.add(entry.group(1))
        for entry in re.finditer(r'\{\s*"([A-Za-z0-9+\-_]{11}#lib[A-Za-z0-9_]+)",\s*[A-Za-z_]\w*\s*\}', text):
            nids.add(entry.group(1))
    return names, nids


def loaded_image(module):
    size = max((p["vaddr"] + p["memsz"] for p in module["ph"]), default=0)
    image = bytearray(size)
    for ph in module["ph"]:
        if ph["type"] == 1 and ph["filesz"]:
            image[ph["vaddr"]:ph["vaddr"] + ph["filesz"]] = module["elf"][ph["offset"]:ph["offset"] + ph["filesz"]]
    return image


def function_ranges(module):
    image = loaded_image(module)
    header = next((p for p in module["ph"] if p["type"] == 0x6474E550), None)
    if header:
        hdr = header["vaddr"]
        if hdr + 12 <= len(image) and image[hdr:hdr + 4] == bytes([1, 0x1B, 3, 0x3B]):
            count = int.from_bytes(image[hdr + 8:hdr + 12], "little")
            starts = sorted(hdr + int.from_bytes(image[hdr + 12 + 8 * i:hdr + 16 + 8 * i], "little", signed=True)
                            for i in range(min(count, 1 << 20)))
            if starts:
                end = hdr + 4 + int.from_bytes(image[hdr + 4:hdr + 8], "little", signed=True)
                return [(starts[i], starts[i + 1] if i + 1 < len(starts) else end) for i in range(len(starts))]
    return [(p["vaddr"], p["vaddr"] + p["filesz"]) for p in module["ph"]
            if p["type"] == 1 and p["flags"] & 1 and p["filesz"]]


def call_evidence(module):
    image = loaded_image(module)
    functions = function_ranges(module)
    executable = [(p["vaddr"], p["vaddr"] + p["filesz"]) for p in module["ph"]
                  if p["type"] == 1 and p["flags"] & 1 and p["filesz"]]
    got = {}
    for target, kind, symbol, _addend in module["relocs"]:
        if kind == 7 and symbol < len(module["symbols"]):
            got[target] = module["symbols"][symbol]["name"]
    called, stubs = set(), {}
    for start, end in functions:
        for at in range(start, max(start, end - 6)):
            if image[at:at + 2] != b"\xff\x15":
                continue
            target = at + 6 + int.from_bytes(image[at + 2:at + 6], "little", signed=True)
            if target in got:
                called.add(got[target])
    for start, end in executable:
        for at in range(start, max(start, end - 6)):
            if image[at:at + 2] != b"\xff\x25" or image[at + 6] != 0xCC:
                continue
            target = at + 6 + int.from_bytes(image[at + 2:at + 6], "little", signed=True)
            if target in got:
                stubs[at] = got[target]
    for start, end in functions:
        for at in range(start, max(start, end - 5)):
            if image[at] != 0xE8:
                continue
            target = at + 5 + int.from_bytes(image[at + 1:at + 5], "little", signed=True)
            if target in stubs:
                called.add(stubs[target])
    return called


def canonical(name):
    return name.replace("#libScePosix", "#libkernel")


def resolution(import_name, names, exports, nids, generated):
    key = canonical(import_name)
    if key in names:
        return ("resolved" if names[key] in exports or key in nids else "missing"), names[key]
    if key in nids or key.split("#")[0] in nids:
        return "resolved", names.get(key, "")
    bare = key.split("#")[0]
    if bare in generated or key in generated:
        return "resolved", generated.get(bare, generated.get(key, ""))
    return "unknown", names.get(key, "")


def scan_module(path, root, names, exports, nids, generated, shared, guest_exports, cache):
    module = cache.get(path) or read_module(path)
    called = call_evidence(module)
    entries = []
    for symbol in module["symbols"]:
        if symbol["section"] != 0 or not symbol["identity"]:
            continue
        identity = symbol["identity"]
        nid, library = identity[0], identity[1][0]
        import_name = f"{nid}#{library}"
        state, mapped = resolution(import_name, names, exports, nids, generated)
        native = library in NATIVE
        guest = library in shared
        guest_ok = guest and nid in guest_exports.get(library, set())
        entries.append({
            "build": root.name,
            "module": str(path.relative_to(root)).replace("\\", "/"),
            "nid": nid, "library": library, "import": import_name,
            "mapped": mapped, "state": state, "native": native, "guest": guest, "guest_ok": guest_ok,
            "called": import_name in called or symbol["name"] in called,
        })
    return entries


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", action="append", help="CUSA57767-1.00 or 1.01; default both")
    parser.add_argument("--module", action="append", default=[], help="module path relative to the prepared build")
    parser.add_argument("--all", action="store_true", help="scan every PRX in the prepared builds")
    parser.add_argument("--src", type=Path, default=ROOT / "src")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--check", action="store_true", help="exit 1 when a called critical import has no resolver")
    args = parser.parse_args(argv)
    builds = args.build or ["CUSA57767-1.00", "CUSA57767-1.01"]
    names = load_names(args.src)
    exports, nids = load_exports(args.src)
    generated = {ps4_nid(name): name for name in exports}
    all_entries, failures, warnings = [], 0, 0
    for build in builds:
        root = ROOT / "game" / "prepared" / build
        if not root.is_dir():
            print(f"missing prepared build: {root}", file=sys.stderr)
            failures += 1
            continue
        modules = args.module or ([p for p in root.rglob("*.prx")] if args.all else list((root / "Media" / "Plugins").glob("*.prx")))
        if not args.all and not args.module:
            modules = [root / "Media" / "Plugins" / "SaveData.prx"]
        shared = {"libc", "libkernel", "libSceLibcInternal"}
        shared.update(p.stem for p in root.rglob("*.prx"))
        cache, guest_exports = {}, {}
        for path in modules:
            if not path.is_file():
                print(f"missing module: {path}", file=sys.stderr)
                failures += 1
                continue
            module = read_module(path)
            cache[path] = module
            for symbol in module["symbols"]:
                if symbol["section"] != 0 and symbol["identity"]:
                    guest_exports.setdefault(path.stem, set()).add(symbol["identity"][0])
        for path in modules:
            if path not in cache:
                continue
            entries = scan_module(path, root, names, exports, nids, generated, shared, guest_exports, cache)
            all_entries += entries
    lines = [f"Import preflight: {', '.join(builds)}", f"modules: {len(set(e['module'] for e in all_entries))} imports: {len(all_entries)}"]
    by_module = {}
    for entry in all_entries:
        row = by_module.setdefault((entry["build"], entry["module"]), Counter())
        row["total"] += 1
        row["called"] += entry["called"]
        row["native"] += entry["native"]
        row["guest"] += entry["guest"]
        row["resolved"] += entry["state"] == "resolved"
        row["missing"] += entry["state"] == "missing"
        row["unknown"] += entry["state"] == "unknown"
    for build, module in sorted(by_module):
        row = by_module[(build, module)]
        lines.append(f"  {build:16} {module:48} total={row['total']:3} called={row['called']:3} resolved={row['resolved']:3} "
                     f"missing={row['missing']:2} unknown={row['unknown']:2} native={row['native']:3} guest={row['guest']:3}")
    blocking = [e for e in all_entries if e["called"] and not e["native"] and e["state"] != "resolved" and
                not (e["guest"] and e["guest_ok"])]
    optional = [e for e in blocking if (e["module"], e["nid"]) in OPTIONAL_MISSING]
    blocking = [e for e in blocking if (e["module"], e["nid"]) not in OPTIONAL_MISSING]
    critical = [e for e in blocking if e["module"] in CRITICAL_MODULES]
    required_missing = [key for key in REQUIRED_NIDS if canonical(key) not in names]
    if optional:
        lines += ["", "KNOWN OPTIONAL GAPS (not implemented, not on startup/save path):"]
        for entry in optional:
            lines.append(f"  {entry['build']} {entry['module']} {entry['nid']} {entry['mapped'] or '-':48} "
                         f"{OPTIONAL_MISSING[(entry['module'], entry['nid'])]}")
    if blocking:
        lines += ["", "CALLED IMPORTS WITH NO RESOLVER:"]
        for entry in blocking:
            lines.append(f"  {entry['build']} {entry['module']} {entry['nid']} {entry['library']:24} "
                         f"{entry['mapped'] or '-':48} {entry['state']}")
    else:
        lines += ["", "CALLED IMPORTS WITH NO RESOLVER: none"]
    if required_missing:
        lines.append("")
        lines.append("REQUIRED NID MAPPINGS MISSING: " + ", ".join(required_missing))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    if args.check and (critical or required_missing):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
