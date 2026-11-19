#!/usr/bin/env python3
"""Report which PS4 imports of a Sons of Sparta boot image the runtime resolves.

Reads build/image/<build>/image.json and boot.bin, the generated import names and the
runtime/GPU resolver sources.  Without evidence the classification is conservative.  With
--runtime-log it also consumes the STOP snapshots and system-module loads of a real run:
the observed API becomes HIGH, and imports called immediately around the observed call
become HIGH_STARTUP_PROBE (or HIGH outside the optional-hardware libraries).
"""
import argparse
import json
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REFERENCE_ROOTS = [
    Path(r"S:\VisualStudio\repos\referencias-ps4\shadPS4\src"),
    Path(r"S:\VisualStudio\repos\alienation-pc\src"),
    Path(r"S:\VisualStudio\repos\13-sentinels-aegis-rim-pc\src"),
    Path(r"S:\VisualStudio\repos\odin-sphere-leifthrasir-pc\src"),
    Path(r"S:\VisualStudio\repos\dragons-crown-pro-pc\src"),
]
OPTIONAL_LIBS = {
    "libSceAjm", "libSceAvPlayer", "libSceCamera", "libSceHmd", "libSceHmdSetupDialog",
    "libSceHttp", "libSceIme", "libSceImeDialog", "libSceMove", "libSceNpCommon",
    "libSceNpManager", "libSceNpWebApi", "libSceRemoteplay", "libSceSocialScreen",
    "libSceSocialScreenDialog", "libSceSsl", "libSceWebBrowserDialog", "libSceMouse",
    "libSceVrTracker",
}
HIGH_LIBS = {"libSceSystemService", "libSceVideoOut", "libScePad"}
MODULE_IDS = {0x8f: "libSceMove", 0x95: "libSceIme", 0xa9: "libSceMouse", 0xb4: "AppContent"}
PROBE_WINDOW = 0x200


def read_text(path):
    try:
        return path.read_text(errors="ignore")
    except OSError:
        return ""


def source_text():
    text = []
    for pattern in ("*.c", "*.h", "*.cpp"):
        for path in (ROOT / "src").rglob(pattern):
            text.append(read_text(path))
    return "\n".join(text)


def reference_text():
    text = []
    for root in REFERENCE_ROOTS:
        if not root.exists():
            continue
        for pattern in ("*.c", "*.cpp", "*.h"):
            for path in root.rglob(pattern):
                text.append(read_text(path))
    return "\n".join(text)


def import_names():
    names = {}
    for line in read_text(ROOT / "src" / "import_names.inc").splitlines():
        match = re.search(r'\{"([^"]+)","([^"]+)"\}', line)
        if match:
            names[match.group(1)] = match.group(2)
    return names


def parse_boot(path):
    data = path.read_bytes()
    off = 8
    size, entry, segment_count, reloc_count, import_count, capabilities = struct.unpack_from("<6Q", data, off)
    off += 48
    procparam = struct.unpack_from("<Q", data, off)[0]
    off += 8
    tls = struct.unpack_from("<4Q", data, off)
    off += 32
    module_count = struct.unpack_from("<Q", data, off)[0]
    off += 8
    modules = []
    for _ in range(module_count):
        base, length, init, tls_address, tls_memsz, tls_filesz, tls_module = struct.unpack_from("<7Q", data, off)
        off += 56
        modules.append({"base": base, "size": length})
    bindings = struct.unpack_from("<Q", data, off)[0]
    off += 8
    binding_indices = set()
    for _ in range(bindings):
        index = struct.unpack_from("<Q", data, off)[0]
        binding_indices.add(index)
        off += 24
    segments = []
    for _ in range(segment_count):
        address, length, flags = struct.unpack_from("<3Q", data, off)
        off += 24
        segments.append((address, length, flags))
    names = []
    for _ in range(import_count):
        names.append(data[off:off + 128].split(b"\0")[0].decode("ascii", "replace"))
        off += 128
    relocs = []
    for _ in range(reloc_count):
        target, kind, value, addend = struct.unpack_from("<QQqq", data, off)
        off += 32
        relocs.append((target, kind, value, addend))
    return {
        "data": data, "image_offset": off, "size": size, "entry": entry,
        "segments": segments, "modules": modules, "names": names, "relocs": relocs,
        "capabilities": capabilities, "tls": tls, "procparam": procparam,
        "binding_indices": binding_indices,
    }


def image_facts(boot):
    image = boot["data"][boot["image_offset"]:]
    slots = {}
    for target, kind, value, _addend in boot["relocs"]:
        if kind in (1, 2) and value < len(boot["names"]):
            slots[target] = value
    stubs = {}
    direct_calls = defaultdict(list)
    for address, length, flags in boot["segments"]:
        if not flags & 1:
            continue
        end = min(address + length, len(image))
        chunk = image[address:end]
        for pattern, is_call in ((b"\xff\x15", True), (b"\xff\x25", False)):
            pos = chunk.find(pattern)
            while pos >= 0:
                absolute = address + pos
                if pos + 6 <= len(chunk):
                    disp = struct.unpack_from("<i", chunk, pos + 2)[0]
                    target = absolute + 6 + disp
                    if target in slots:
                        stubs[absolute] = slots[target]
                        if is_call:
                            direct_calls[slots[target]].append(absolute)
                pos = chunk.find(pattern, pos + 1)
    callers = defaultdict(list)
    for index, positions in direct_calls.items():
        callers[index].extend(positions)
    stub_set = set(stubs)
    for address, length, flags in boot["segments"]:
        if not flags & 1:
            continue
        end = min(address + length, len(image))
        chunk = image[address:end]
        for pattern in (b"\xe8", b"\xe9"):
            pos = chunk.find(pattern)
            while pos >= 0:
                absolute = address + pos
                if pos + 5 <= len(chunk):
                    disp = struct.unpack_from("<i", chunk, pos + 1)[0]
                    target = absolute + 5 + disp
                    if target in stub_set:
                        callers[stubs[target]].append(absolute)
                pos = chunk.find(pattern, pos + 1)

    def module_of(address):
        for index, module in enumerate(boot["modules"]):
            if module["base"] <= address < module["base"] + module["size"]:
                return index, address - module["base"]
        return None, address

    result = {}
    for index, name in enumerate(boot["names"]):
        found = sorted(set(callers.get(index, [])))
        result[name] = {"callers": [(address, *module_of(address)) for address in found]}
    return result


def implementation_data():
    text = source_text()
    references = reference_text()
    exports = set(re.findall(r'\{"([A-Za-z_][A-Za-z0-9_]*)",\s*[A-Za-z_]', text))
    nids = set(re.findall(r'"([A-Za-z0-9+\-_]{11}#lib[A-Za-z0-9_]+)"', text))
    gpu_text = "\n".join(read_text(path) for path in (ROOT / "gpu" / "shadps4").rglob("*") if path.suffix in (".cpp", ".h"))
    nids.update(re.findall(r'LIB_FUNCTION\(\s*"([^"]+)"', gpu_text))
    return text, references, exports, nids, gpu_text


def function_start(image, address, limit=0x20000):
    for pos in range(address, max(0, address - limit), -1):
        if image[pos:pos + 4] in (b"\xf3\x0f\x1e\xfa", b"\x55\x48\x89\xe5"):
            return pos
    return None


def runtime_evidence(path, boot):
    if not path:
        return {"stops": [], "windows": [], "apis": set(), "modules": []}
    text = read_text(path)
    stops = []
    pattern = re.compile(
        r"STOP: first unsupported PS4 import: ([^#\s]+)#([^\s]+) \(index (\d+)\)\s*\n"
        r"API: ([^\n]+)\s*\n.*?"
        r"Caller (?:at image\+0x([0-9a-fA-F]+)|in linked module (\d+): \+0x([0-9a-fA-F]+))",
        re.S,
    )
    for match in pattern.finditer(text):
        nid, library, index, api, image_offset, module_index, module_offset = match.groups()
        if image_offset is not None:
            address = int(image_offset, 16)
        else:
            module = boot["modules"][int(module_index)]
            address = module["base"] + int(module_offset, 16)
        stops.append({
            "nid": nid, "library": library, "index": int(index),
            "api": api.strip(), "address": address,
        })
    windows = [(stop["address"] - PROBE_WINDOW, stop["address"] + PROBE_WINDOW) for stop in stops]
    modules = []
    for match in re.finditer(r"Runtime: (?:internal )?system module 0x([0-9a-fA-F]+) loaded", text):
        module_id = int(match.group(1), 16)
        if module_id not in [entry[0] for entry in modules]:
            modules.append((module_id, MODULE_IDS.get(module_id, "unknown")))
    return {"stops": stops, "windows": windows, "apis": {stop["api"] for stop in stops},
            "modules": modules}


def classify(name, callers, exports, nids, text, bound, evidence):
    nid, _, library = name.partition("#")
    key = name.replace("#libScePosix", "#libkernel")
    symbol = NAMES.get(key if key in NAMES else name)
    if not symbol:
        return "UNKNOWN", False, False
    resolved = bound or nid in nids or key in nids or name in nids or bool(symbol and re.search(r'\{"%s"' % re.escape(symbol), text))
    exists = bool(re.search(r'\b%s\s*\(' % re.escape(symbol), text))
    if symbol in evidence["apis"]:
        return "HIGH", resolved, exists
    for call in callers:
        if any(lo <= call[0] <= hi for lo, hi in evidence["windows"]):
            return ("HIGH_STARTUP_PROBE" if library in OPTIONAL_LIBS else "HIGH"), resolved, exists
    if "EyeToEyeDistance" in symbol:
        return "OPTIONAL_RUNTIME", resolved, exists
    if library in OPTIONAL_LIBS:
        return "OPTIONAL_RUNTIME", resolved, exists
    if library in HIGH_LIBS and callers:
        return "HIGH", resolved, exists
    if callers:
        return "MEDIUM", resolved, exists
    return "LOW", resolved, exists


def caller_text(call):
    if not call:
        return "no caller found"
    if call[1] is not None:
        return f"module {call[1]} +0x{call[2]:x}"
    return f"image+0x{call[0]:x}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="CUSA57767-1.00")
    parser.add_argument("--image", type=Path)
    parser.add_argument("--runtime-log", type=Path)
    parser.add_argument("--report", type=Path, default=ROOT / "build" / "startup-import-report.txt")
    args = parser.parse_args()
    image_dir = args.image or (ROOT / "build" / "image" / args.build)
    report = json.loads(read_text(image_dir / "image.json"))
    boot = parse_boot(image_dir / "boot.bin")
    callers = image_facts(boot)
    text, references, exports, nids, _gpu = implementation_data()
    evidence = runtime_evidence(args.runtime_log, boot) if args.runtime_log else {
        "stops": [], "windows": [], "apis": set(), "modules": []}

    entries = []
    for index, name in enumerate(boot["names"]):
        calls = callers.get(name, {"callers": []})["callers"]
        confidence, resolved, exists = classify(name, calls, exports, nids, text,
                                                 index in boot["binding_indices"], evidence)
        nid, _, library = name.partition("#")
        symbol = NAMES.get(name.replace("#libScePosix", "#libkernel"), NAMES.get(name, "unknown"))
        reference = "shadPS4" if symbol in references else "-"
        entry = {
            "name": name, "nid": nid, "library": library, "symbol": symbol,
            "confidence": confidence, "resolved": resolved, "exists": exists,
            "callers": calls, "reference": reference,
        }
        entries.append(entry)

    counts = Counter(entry["confidence"] for entry in entries if not entry["resolved"])
    unresolved = [entry for entry in entries if not entry["resolved"]]
    unregistered = [entry for entry in unresolved if entry["exists"]]
    lines = [
        f"Startup import report: {report.get('title_id', '?')} {report.get('app_ver', '?')}",
        f"image: {image_dir}",
        f"entry: {report.get('entry')}  imports: {len(entries)}  resolved: {len(entries) - len(unresolved)}  unresolved: {len(unresolved)}",
        f"HIGH: {counts['HIGH']}  HIGH_STARTUP_PROBE: {counts['HIGH_STARTUP_PROBE']}  MEDIUM: {counts['MEDIUM']}  "
        f"LOW: {counts['LOW']}  OPTIONAL_RUNTIME: {counts['OPTIONAL_RUNTIME']}  UNKNOWN: {counts['UNKNOWN']}",
        f"implemented but unregistered: {len(unregistered)}",
        "",
        "By library:",
    ]
    by_library = defaultdict(lambda: Counter(total=0, resolved=0, unresolved=0, HIGH=0,
                                             HIGH_STARTUP_PROBE=0, MEDIUM=0, LOW=0,
                                             OPTIONAL_RUNTIME=0, UNKNOWN=0))
    for entry in entries:
        row = by_library[entry["library"]]
        row["total"] += 1
        if entry["resolved"]:
            row["resolved"] += 1
        else:
            row["unresolved"] += 1
            row[entry["confidence"]] += 1
    for library in sorted(by_library):
        row = by_library[library]
        lines.append(f"  {library:28} total={row['total']:3} resolved={row['resolved']:3} unresolved={row['unresolved']:3} "
                     f"HIGH={row['HIGH']} PROBE={row['HIGH_STARTUP_PROBE']} MEDIUM={row['MEDIUM']} "
                     f"LOW={row['LOW']} OPTIONAL_RUNTIME={row['OPTIONAL_RUNTIME']}")
    for heading, category in (("HIGH unresolved:", "HIGH"),
                              ("HIGH_STARTUP_PROBE unresolved:", "HIGH_STARTUP_PROBE")):
        lines += ["", heading]
        selected = [entry for entry in unresolved if entry["confidence"] == category]
        if not selected:
            lines.append("  none")
        for entry in selected:
            lines.append(f"  {entry['nid']}  {entry['symbol']:48} {entry['library']:24} "
                         f"{caller_text(entry['callers'][0] if entry['callers'] else None):24} {entry['reference']}")
    lines += ["", "Runtime-confirmed APIs:"]
    if not evidence["stops"]:
        lines.append("  none")
    image = boot["data"][boot["image_offset"]:]
    for stop in evidence["stops"]:
        start = function_start(image, stop["address"])
        where = f"function image+0x{start:x}" if start is not None else "function unknown"
        entry = next((item for item in entries if item["nid"] == stop["nid"]), None)
        symbol = entry["symbol"] if entry else stop["api"]
        resolved = "resolved" if entry and entry["resolved"] else "unresolved"
        lines.append(f"  {stop['api']:48} {stop['library']:24} caller image+0x{stop['address']:x} "
                     f"{where} {resolved}")
    lines += ["", "Probe-classified imports (resolved or not):"]
    probes = [entry for entry in entries if entry["confidence"] == "HIGH_STARTUP_PROBE"]
    if not probes:
        lines.append("  none")
    for entry in probes:
        state = "resolved" if entry["resolved"] else "unresolved"
        lines.append(f"  {entry['symbol']:48} {entry['library']:24} {caller_text(entry['callers'][0] if entry['callers'] else None):24} {state}")
    lines += ["", "Runtime module evidence:"]
    if not evidence["modules"]:
        lines.append("  none")
    for module_id, name in evidence["modules"]:
        lines.append(f"  0x{module_id:x} {name}")
    lines += ["", "Unresolved implementations that exist in the tree:"]
    if not unregistered:
        lines.append("  none")
    for entry in unregistered[:40]:
        lines.append(f"  {entry['nid']}  {entry['symbol']}  {entry['library']}")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines[:6]))
    print(f"report: {args.report}")


NAMES = import_names()

if __name__ == "__main__":
    main()
