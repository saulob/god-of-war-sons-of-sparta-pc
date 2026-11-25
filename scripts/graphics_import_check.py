"""Graphics import audit of every supported CUSA57767 build against the host video core and the committed inventory.

    python scripts/graphics_import_check.py             check the prepared builds against scripts/graphics_imports.json
    python scripts/graphics_import_check.py --write     rewrite the inventory from the prepared builds (review the diff)
    python scripts/graphics_import_check.py --self-test prove that the gate fails on each kind of omission

The libraries are the graphics ones the game links (GnmDriver, VideoOut, Razor). Each imported symbol of the eboot and of
every module the runtime loads is classified from the host function it binds to:

    implemented        real host code
    forwarded          only calls another host import
    intentional-no-op  retail firmware has no such function: the host returns what retail returns
    stubbed-success    logs "(STUBBED)" and returns success without writing state, outputs or commands
    stubbed-failure    logs "(STUBBED)" and aborts or fails
    unresolved         no host function is registered for the import

A stub that returns success while the guest expects state, an output structure or recorded commands is the pattern that
leaves a draw with invalid state and no error, so no stub may have a guest caller in any loaded module of any build.
The inventory records class and caller presence per build: an import that appears, disappears or gains a caller in one
build and not in the committed inventory fails until reviewed.
"""
import copy
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import build_evidence as evidence  # noqa: E402
import ps4_module as pm  # noqa: E402
from game_profiles import PROFILES  # noqa: E402

TITLE = "CUSA57767"
INVENTORY = ROOT / "scripts" / "graphics_imports.json"
GRAPHICS_LIBRARY = re.compile(r"^(libSceGnm|libSceVideoOut|libSceRazor|libSceGpu|libSceAgc|libSceGfx)")
HOST_SOURCES = (ROOT / "gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp", ROOT / "gpu/shadps4/core/libraries/videoout/video_out.cpp")
RUNTIME_TABLE = ROOT / "src/runtime_sos.c"
STUB_CLASSES = ("stubbed-success", "stubbed-failure")
CLASSES = ("implemented", "forwarded", "intentional-no-op") + STUB_CLASSES + ("unresolved",)

REGISTRATION = re.compile(r'LIB_FUNCTION\(\s*"([^"]+)"\s*,\s*"(\w+)"\s*,\s*1\s*,\s*"\w+"\s*,\s*(\w+)\s*\)')
FUNCTION = re.compile(r"^[A-Za-z][^\n;{}]*?\b(\w+)\(([^)]*)\)\s*\{(.*?)^\}", re.M | re.S)
FORWARD = re.compile(r"(?:LOG_\w+\([^;]*\);\s*)?(?:return\s+)?(?:sceGnm|sceVideoOut)\w+\([^;]*\);")
NAMES = re.compile(r'\{"([^"]+)#(\w+)","([^"]+)"\}')
RUNTIME_ENTRY = re.compile(r'\{\s*"(\w+)"\s*,\s*(razor_cpu_ok)\s*\}')


def inventory_builds():
    return {version: f"{TITLE}-{version}" for version in PROFILES[TITLE]["versions"]}


def classify(body):
    """Class of a host function from its body."""
    flat = " ".join(re.sub(r"//[^\n]*", "", body).split())
    if "UNREACHABLE" in body or ("(STUBBED)" in body and re.search(r"return\s+(-?\d+|ORBIS_\w*ERROR\w*)\s*;", flat) and "ORBIS_OK" not in flat):
        return "stubbed-failure"
    if "(STUBBED)" in body:
        return "stubbed-success"
    if "Not available in retail firmware" in body:
        return "intentional-no-op"
    if FORWARD.fullmatch(flat):
        return "forwarded"
    return "implemented"


def host_classes():
    """{(nid, library): class} of everything the host registers, read from the sources."""
    result = {}
    for path in HOST_SOURCES:
        text = path.read_text(errors="replace")
        bodies = {m.group(1): m.group(3) for m in FUNCTION.finditer(text)}
        for m in REGISTRATION.finditer(text):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if text[line_start:m.start()].lstrip().startswith("//"):
                continue
            nid, library, function = m.groups()
            if function in bodies:
                result[(nid, library)] = classify(bodies[function])
    runtime = RUNTIME_TABLE.read_text(errors="replace")
    for m in RUNTIME_ENTRY.finditer(runtime):
        result[(pm.nid(m.group(1)), "libSceRazorCpu")] = "intentional-no-op"
    return result


def symbol_names():
    names = {}
    for line in (ROOT / "src" / "import_names.inc").read_text(errors="replace").splitlines():
        m = NAMES.match(line.strip())
        if m:
            names[(m.group(1), m.group(2))] = m.group(3)
    for m in RUNTIME_ENTRY.finditer(RUNTIME_TABLE.read_text(errors="replace")):
        names[(pm.nid(m.group(1)), "libSceRazorCpu")] = m.group(1)
    return names


def module_paths(version):
    media = evidence.media(version)
    return [evidence.eboot(version)] + media_modules(media / "Modules") + media_modules(media / "Plugins")


def media_modules(folder):
    return sorted(folder.glob("*.prx"))


def scan_module(path):
    """{(nid, library): call site count} of the graphics imports of a module; thunks do not count as calls."""
    module = pm.read_module(path)
    symbols = module["symbols"]
    slots = {}
    for target, _kind, index, _addend in module["relocs"]:
        if index and index < len(symbols) and symbols[index]["section"] == 0 and symbols[index]["identity"]:
            nid, library, _module = symbols[index]["identity"]
            if GRAPHICS_LIBRARY.match(library[0]):
                slots[target] = (nid, library[0])
    found = {key: 0 for key in slots.values()}
    if not slots:
        return found
    loads = [p for p in module["ph"] if p["type"] == 1]
    image = bytearray(max(p["vaddr"] + p["memsz"] for p in loads))
    for p in loads:
        image[p["vaddr"]:p["vaddr"] + p["filesz"]] = module["elf"][p["offset"]:p["offset"] + p["filesz"]]
    image = bytes(image)
    thunks = {}
    for m in re.finditer(b"\xff[\x25\x15]", image):
        at = m.start()
        if at + 6 > len(image):
            continue
        slot = at + 6 + struct.unpack_from("<i", image, at + 2)[0]
        if slot in slots:
            if image[at + 1] == 0x15:
                found[slots[slot]] += 1
            else:
                thunks[at] = slots[slot]
    for m in re.finditer(b"[\xe8\xe9]", image):
        at = m.start()
        if at + 5 <= len(image) and at + 5 + struct.unpack_from("<i", image, at + 1)[0] in thunks:
            found[thunks[at + 5 + struct.unpack_from("<i", image, at + 1)[0]]] += 1
    return found


def scan_build(version):
    """{(nid, library): called} over the eboot and the modules the runtime loads, or None when the build's files are not on this machine."""
    if evidence.eboot(version) is None:
        return None
    called = {}
    for path in module_paths(version):
        for key, sites in scan_module(path).items():
            called[key] = called.get(key, False) or sites > 0
    return called


def build_live(builds):
    """{version: {(nid, library): called}} for the installed builds."""
    live = {}
    for version in builds:
        scanned = scan_build(version)
        if scanned is not None:
            live[version] = scanned
    return live


def inventory_from(live, hosts, names):
    rows = {}
    for version, imports in sorted(live.items()):
        for (nid, library), called in imports.items():
            row = rows.setdefault(f"{nid}#{library}", {"symbol": names.get((nid, library), "?"), "class": hosts.get((nid, library), "unresolved"), "called": {}})
            row["called"][version] = called
    return {key: rows[key] for key in sorted(rows, key=lambda k: (k.split("#")[1], rows[k]["symbol"], k))}


def evaluate(builds, live, hosts, names, committed):
    """(list of failure texts, list of report lines); the gate and its self-test share it."""
    failures, report = [], []

    def check(ok, text):
        if not ok:
            failures.append(text)
        return ok

    for version in sorted(builds):
        key = builds[version].replace("-", " ", 1)
        if not check(version in live, f"{key}: not installed, so its graphics imports were not read"):
            continue
        imports = live[version]
        unresolved = sorted(names.get(k, k[0]) for k in imports if k not in hosts)
        unnamed = sorted(k[0] for k in imports if k not in names or pm.nid(names[k]) != k[0])
        stubs = sorted(names.get(k, k[0]) for k, called in imports.items() if called and hosts.get(k) in STUB_CLASSES)
        counts = {c: sum(1 for k in imports if hosts.get(k, "unresolved") == c) for c in CLASSES}
        called = sum(1 for v in imports.values() if v)
        report.append(f"{key}: {len(imports)} graphics imports, {called} called by the guest, "
                      + ", ".join(f"{n} {c}" for c, n in counts.items() if n))
        check(not unresolved, f"{key}: graphics imports with no host function: {unresolved}")
        check(not unnamed, f"{key}: graphics imports without a symbolic name whose hash is the NID: {unnamed}")
        check(not stubs, f"{key}: stubbed graphics imports that the guest calls (success or failure without state): {stubs}")
        listed = {k for k, row in committed.items() if version in row["called"]}
        present = {f"{nid}#{library}" for nid, library in imports}
        check(present <= listed, f"{key}: graphics imports missing from scripts/graphics_imports.json: {sorted(names.get(tuple(k.split('#')), k) for k in present - listed)}")
        check(listed <= present, f"{key}: inventory lists graphics imports this build no longer has: {sorted(committed[k]['symbol'] for k in listed - present)}")
        for (nid, library), was_called in sorted(imports.items(), key=lambda item: item[0]):
            row = committed.get(f"{nid}#{library}")
            if row is None or version not in row["called"]:
                continue
            symbol = names.get((nid, library), nid)
            check(row["class"] == hosts.get((nid, library), "unresolved"), f"{key}: {symbol} is {hosts.get((nid, library), 'unresolved')} in the host but {row['class']} in the inventory")
            check(row["called"][version] == was_called, f"{key}: {symbol} {'gained' if was_called else 'lost'} a guest caller against the inventory")
    unknown = sorted({v for row in committed.values() for v in row["called"]} - set(builds))
    check(not unknown, f"the inventory names builds that are not supported: {unknown}")
    return failures, report


def load_inventory():
    return json.loads(INVENTORY.read_text())


def recorded_imports(committed, version):
    return {tuple(k.split("#")): row["called"][version] for k, row in committed.items() if version in row["called"]}


def self_test():
    builds = inventory_builds()
    hosts, names = host_classes(), symbol_names()
    committed = load_inventory()
    live = {v: recorded_imports(committed, v) for v in builds}
    results = [("the committed inventory passes against its own data", not evaluate(builds, live, hosts, names, committed)[0])]

    def expect_failure(name, mutate):
        b, l, h, n, c = dict(builds), copy.deepcopy(live), dict(hosts), dict(names), copy.deepcopy(committed)
        mutate(b, l, h, n, c)
        results.append((name, bool(evaluate(b, l, h, n, c)[0])))

    first = next(k for k, row in committed.items() if len(row["called"]) == len(builds))
    key = tuple(first.split("#"))
    uncalled_stub = next(k for k, row in committed.items() if row["class"] in STUB_CLASSES and not any(row["called"].values()))
    stub_key = tuple(uncalled_stub.split("#"))
    expect_failure("an import missing from the inventory", lambda b, l, h, n, c: c.pop(first))
    expect_failure("an import missing from one build of the inventory", lambda b, l, h, n, c: c[first]["called"].pop(max(builds)))
    expect_failure("an import the build no longer has", lambda b, l, h, n, c: l[max(builds)].pop(key))
    expect_failure("an import with no host function", lambda b, l, h, n, c: h.pop(key))
    expect_failure("an import changing class in the host", lambda b, l, h, n, c: h.__setitem__(key, "stubbed-success" if h[key] != "stubbed-success" else "implemented"))
    expect_failure("an import without a name", lambda b, l, h, n, c: n.pop(key))
    expect_failure("a guest caller appearing", lambda b, l, h, n, c: l[max(builds)].__setitem__(key, not c[first]["called"][max(builds)]))
    expect_failure("a stub called by the guest in one build", lambda b, l, h, n, c: (l[min(builds)].__setitem__(stub_key, True), c[uncalled_stub]["called"].__setitem__(min(builds), True)))
    expect_failure("a build added to the supported builds", lambda b, l, h, n, c: b.update({"9.99": f"{TITLE}-9.99"}))
    expect_failure("an inventory entry for an unsupported build", lambda b, l, h, n, c: c[first]["called"].__setitem__("9.99", True))
    ok = True
    for name, passed in results:
        print(f"  [{'ok  ' if passed else 'fail'}] {name}")
        ok = ok and passed
    print(f"Graphics import gate self-test {'passed' if ok else 'FAILED'}: {len(results)} cases")
    return 0 if ok else 1


def main(argv):
    if "--self-test" in argv:
        return self_test()
    builds = inventory_builds()
    hosts, names = host_classes(), symbol_names()
    scanned = build_live(builds)
    if "--write" in argv:
        if set(scanned) != set(builds):
            print("[fail] --write reads every supported build; the files of one are not on this machine")
            return 1
        INVENTORY.write_text(json.dumps(inventory_from(scanned, hosts, names), indent=1) + "\n")
        print(f"wrote {INVENTORY.relative_to(ROOT)}")
        return 0
    committed = load_inventory()
    live = {version: scanned.get(version) or recorded_imports(committed, version) for version in builds}
    for version in sorted(builds):
        evidence.report_content(version, "its graphics imports")
        if version not in scanned:
            evidence.report_contract(version, "the imports recorded in scripts/graphics_imports.json against the host functions")
    failures, report = evaluate(builds, live, hosts, names, committed)
    for line in report:
        print(f"  {line}")
    for text in failures:
        print(f"  [fail] {text}")
    labels = " and ".join(f"{TITLE} {v}" for v in sorted(builds))
    if failures:
        print(f"Graphics import audit FAILED for {labels}: {len(failures)} problems")
        return 1
    print(f"Graphics import audit passed: {labels} validated")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
