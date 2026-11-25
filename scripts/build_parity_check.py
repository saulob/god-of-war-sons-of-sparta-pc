"""Build parity gate: every build-specific table covers every supported build, and the builds agree where they must.

    python scripts/build_parity_check.py             run the gate against the sources and the prepared builds
    python scripts/build_parity_check.py --builds    print the supported build keys, one per line
    python scripts/build_parity_check.py --self-test prove that the gate fails on each kind of omission and mismatch

The supported builds are the versions of scripts/game_profiles.py. A feature with build-specific data (addresses, sites,
fingerprints, tables, instruction bytes) is registered in FEATURES with the function that reads its per-build data; a source
that names a build in a string literal but is registered nowhere fails the gate. Shared host code is listed in SHARED_HOST
and needs one test, not one per build. Nothing is executed; the prepared eboots are read and decoded with llvm-mc.
"""
import ast
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from game_profiles import PROFILES  # noqa: E402

TITLE = "CUSA57767"
UNSUPPORTED_PROBES = {"9.99", "1.02"}
INIT_EX_BUFFERS, INIT_EX_AUTO_START, INIT_EX_FILE_CALLBACKS = 164, 168, (48, 56, 64, 72, 80)
GAME_VIDEO_BUFFERS, SYNC_MODE_NONE = 6, 1


def inventory():
    """{version: {'key', 'sha256', 'folder'}} of the supported builds."""
    return {version: {"key": f"{TITLE}-{version}", "sha256": build["eboot_sha256"], "folder": build["folder"]}
            for version, build in PROFILES[TITLE]["versions"].items()}


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


class Gate:
    def __init__(self):
        self.failures = []
        self.counts = {}
        self.feature = ""

    def start(self, feature):
        self.feature = feature
        self.counts.setdefault(feature, 0)

    def check(self, ok, text):
        self.counts[self.feature] += 1
        if not ok:
            self.failures.append(f"{self.feature}: {text}")
            print(f"  [fail] {self.feature}: {text}")
        return ok

    def same_builds(self, found, expected, what):
        """The table names exactly the supported builds: none missing, none unknown."""
        missing, unknown = sorted(expected - set(found)), sorted(set(found) - expected)
        self.check(not missing, f"{what} has no entry for {', '.join(missing)}")
        self.check(not unknown, f"{what} has entries for unsupported builds {', '.join(unknown)}")


def ast_assignment(path, name):
    for node in ast.parse(read(path)).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value)
    return None


# --- tables read from the sources: {build key or version: data} ----------------------------------------------------

def game_build_header(text):
    return {f"{t}-{v}": sha for t, v, sha in re.findall(r'\{"(CUSA\d+)", "([\d.]+)",\s*"[^"]*",\s*"([0-9a-f]{64})"\}', text)}


def game_profile_rows(text):
    rows = {}
    for block in text.split(".title_id = ")[1:]:
        version = re.search(r'\.version = "([\d.]+)"', block)
        folder = re.search(r'\.folder = "([^"]+)"', block)
        sha = re.search(r'\.eboot_sha256 = "([0-9a-f]{64})"', block)
        skip = re.search(r"\.skip_intro = (true|false)", block)
        sites = re.search(r"\.resolution_sites = (\w+)", block)
        if version and folder and sha:
            rows[f"{TITLE}-{version.group(1)}"] = {"folder": folder.group(1), "sha256": sha.group(1),
                                                    "skip_intro": skip and skip.group(1), "resolution": sites and sites.group(1)}
    return rows


def skip_intro_rows(text):
    return {key: len(fields.split(",")) - 1
            for key, _size, fields in re.findall(r'\{"(CUSA\d+-[\d.]+)", (\d+)u((?:, 0x[0-9a-f]+){8})\}', text)}


def unlock_rows(text):
    return {key: len(fields.split(",")) - 1
            for key, _size, fields in re.findall(r'\{"(CUSA\d+-[\d.]+)", (\d+)u((?:, 0x[0-9a-f]+){2})\}', text)}


def image_glyph_sites(text):
    """{version: semantics}: the hooks' lengths and entries, the services' lengths and the object layout."""
    found = {}
    for match in re.finditer(r"kImageGlyphs_(\d+)_(\d+) = \{(.*?)\};", text, re.S):
        body = match.group(3)
        hooks = re.findall(r"\{0x[0-9a-f]+u, (0x[0-9a-f]+)u, 0x[0-9a-f]+ull, \{([^}]*)\}\}", body)
        ranges = re.findall(r"\{0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}", body)
        layout = re.search(r"((?:0x[0-9a-f]+u, ){4}0x[0-9a-f]+u)$", body)
        if len(hooks) == 4 and len(ranges) == 5 and layout:
            found[f"{match.group(1)}.{match.group(2)}"] = (tuple(hooks), tuple(int(b, 16) - int(a, 16) for a, b in ranges), byte_list(layout.group(1)))
    return found


def split_versions(text, prefix):
    """{version: text of its block} of a generated header whose tables are named <prefix>_<major>_<minor>."""
    found = {}
    for match in re.finditer(r"\b" + prefix + r"_(\d+)_(\d+)\b", text):
        found.setdefault(f"{match.group(1)}.{match.group(2)}", None)
    return found


def byte_list(text):
    return tuple(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", text))


def prompt_sites(text):
    """{version: {site name: (length, immediate, original bytes, Xbox bytes)}} and {version: hook semantics}."""
    sites, hooks = {}, {}
    for match in re.finditer(r"kSites_(\d+)_(\d+)\[\] = \{(.*?)\n\};", text, re.S):
        rows = {}
        for row in re.finditer(r"\{0x[0-9a-f]+u, (\d+), (\d+), \{[^}]*\}, \{([^}]*)\}, \{([^}]*)\}, \{[^}]*\}\}, /\* (.*?) \*/", match.group(3)):
            rows[row.group(5)] = (int(row.group(1)), int(row.group(2)), byte_list(row.group(3)), byte_list(row.group(4)))
        sites[f"{match.group(1)}.{match.group(2)}"] = rows
    for match in re.finditer(r"kHook_(\d+)_(\d+) = \{0x[0-9a-f]+u, (0x[0-9a-f]+)u, 0x[0-9a-f]+ull, \{([^}]*)\}, (0x[0-9a-f]+), (0x[0-9a-f]+)\}", text):
        hooks[f"{match.group(1)}.{match.group(2)}"] = (match.group(3), byte_list(match.group(4)), match.group(5), match.group(6))
    return sites, hooks


def prompt_dialog_sites(text):
    """{version: semantics}: the prologue without its relocated displacement, the ranges' lengths, the object layout and the menu hooks'
    lengths, entries and Image fields."""
    found = {}
    for match in re.finditer(r"kDialog_(\d+)_(\d+) = \{(.*?)\};", text, re.S):
        body = match.group(3)
        prologue = re.search(r"\{(0x[0-9a-f]+(?:, 0x[0-9a-f]+){17})\}", body)
        ranges = re.findall(r"\{0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}", body)
        layout = re.search(r"ull,\s*((?:0x[0-9a-f]+, ){5}0x[0-9a-f]+),", body)
        menus = re.findall(r"\{0x[0-9a-f]+u, (0x[0-9a-f]+)u, 0x[0-9a-f]+ull, \{([^}]*)\}, (\d+)u, \{([^}]*)\}\}", body)
        if prologue and layout and len(menus) == 2:
            bytes18 = byte_list(prologue.group(1))
            found[f"{match.group(1)}.{match.group(2)}"] = (bytes18[:13] + bytes18[17:], tuple(int(b, 16) - int(a, 16) for a, b in ranges),
                                                           byte_list(layout.group(1)), tuple((length, entry, count, offsets) for length, entry, count, offsets in menus))
    return found


def exit_game_sites(text):
    """{version: semantics}: prologues (without relocated displacement), 9 range lengths and 5 field offsets."""
    found = {}
    for match in re.finditer(r"kExitGame_(\d+)_(\d+) = \{(.*?)\};", text, re.S):
        body = match.group(3)
        byte_blocks = re.findall(r"\{(0x[0-9a-f]+(?:, 0x[0-9a-f]+)+)\}", body)
        ranges = re.findall(r"\{0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}", body)
        layout = re.search(r"ull,\s*((?:0x[0-9a-f]+u, ){4}0x[0-9a-f]+u),", body)
        if len(byte_blocks) == 4 and len(ranges) == 9 and layout:
            toggle, confirm, quit_b, text_b = (byte_list(b) for b in byte_blocks)
            found[f"{match.group(1)}.{match.group(2)}"] = (
                toggle, confirm, quit_b[:6] + quit_b[10:], text_b,
                tuple(int(b, 16) - int(a, 16) for a, b in ranges),
                byte_list(layout.group(1)),
            )
    return found


def dispatch_rows(text, prefix):
    """{build key: symbols} of a C table of `{"CUSA...-x.y", ...}` rows."""
    return {key: re.findall(r"\b" + prefix + r"\w*", rest) for key, rest in re.findall(r'\{"(CUSA\d+-[\d.]+)",([^\n]*)\}', text)}


# --- the features --------------------------------------------------------------------------------------------------

def check_identity(gate, builds, sources):
    gate.start("Build identity")
    keys = {b["key"] for b in builds.values()}
    shas = {b["key"]: b["sha256"] for b in builds.values()}
    header = game_build_header(sources["src/game_build.h"])
    gate.same_builds(header, keys, "src/game_build.h")
    gate.check(all(header.get(k) == shas[k] for k in header if k in shas), "src/game_build.h fingerprints differ from scripts/game_profiles.py")
    rows = game_profile_rows(sources["launcher/game_profile.cpp"])
    gate.same_builds(rows, keys, "launcher/game_profile.cpp")
    for version, build in builds.items():
        row = rows.get(build["key"], {})
        gate.check(row.get("sha256") == build["sha256"] and row.get("folder") == build["folder"],
                   f"launcher/game_profile.cpp differs from scripts/game_profiles.py for {build['key']}")
    for name in ("EBOOT_SHA256", "IMAGE_FOLDERS"):
        table = sources["il2cpp:" + name]
        gate.same_builds({f"{TITLE}-{v}" for v in table}, keys, f"scripts/il2cpp_index.py {name}")
    gate.check(all(sources["il2cpp:EBOOT_SHA256"].get(v) == b["sha256"] for v, b in builds.items()), "scripts/il2cpp_index.py fingerprints differ from scripts/game_profiles.py")
    for name in ("prompt_sites", "prompt_dialog", "exit_game_sites"):
        gate.same_builds({f"{TITLE}-{v}" for v in sources[name + ":BUILDS"]}, keys, f"scripts/{name}.py BUILDS")


def check_skip_intro(gate, builds, sources):
    gate.start("Skip Intro")
    keys = {b["key"] for b in builds.values()}
    rows = skip_intro_rows(sources["src/sos_skip_intro.c"])
    gate.same_builds(rows, keys, "src/sos_skip_intro.c site table")
    gate.check(len(set(rows.values())) <= 1, "src/sos_skip_intro.c site rows differ in shape between builds")
    profile = game_profile_rows(sources["launcher/game_profile.cpp"])
    gate.check(all(profile.get(k, {}).get("skip_intro") == "true" for k in keys), "launcher/game_profile.cpp does not enable Skip Intro for every supported build")


def check_hermes_unlock(gate, builds, sources):
    gate.start("Hermes Mode early unlock")
    keys = {b["key"] for b in builds.values()}
    rows = unlock_rows(sources["src/sos_hermes_unlock.c"])
    gate.same_builds(rows, keys, "src/sos_hermes_unlock.c site table")
    gate.check(len(set(rows.values())) <= 1, "src/sos_hermes_unlock.c site rows differ in shape between builds")


def check_pit_unlock(gate, builds, sources):
    gate.start("The Pit early unlock")
    keys = {b["key"] for b in builds.values()}
    rows = unlock_rows(sources["src/sos_pit_unlock.c"])
    gate.same_builds(rows, keys, "src/sos_pit_unlock.c site table")
    gate.check(len(set(rows.values())) <= 1, "src/sos_pit_unlock.c site rows differ in shape between builds")


def check_prompts(gate, builds, sources):
    gate.start("Xbox controller prompts")
    versions = set(builds)
    keys = {b["key"] for b in builds.values()}
    sites, hooks = prompt_sites(sources["launcher/controller_prompts_sites.h"])
    gate.same_builds({f"{TITLE}-{v}" for v in sites}, keys, "launcher/controller_prompts_sites.h sites")
    gate.same_builds({f"{TITLE}-{v}" for v in hooks}, keys, "launcher/controller_prompts_sites.h hook")
    dispatch = dispatch_rows(sources["launcher/controller_prompts.c"], "k(?:Sites|Hook)")
    gate.same_builds(dispatch, keys, "launcher/controller_prompts.c build table")
    for key, symbols in dispatch.items():
        suffix = "_" + key.rsplit("-", 1)[1].replace(".", "_")
        gate.check(symbols and all(s.endswith(suffix) for s in symbols), f"launcher/controller_prompts.c routes {key} to another build's tables")
    reference = min(versions)
    for version in sorted(versions & set(sites)):
        gate.check(sites[version] and sites[version] == sites[reference], f"CUSA57767 {version} prompt sites differ from {reference}: same sites, lengths, original and Xbox bytes expected")
    for version in sorted(versions & set(hooks)):
        gate.check(hooks[version] == hooks.get(reference), f"CUSA57767 {version} Automatic prompt hook differs from {reference} in length, prologue or field offsets")


def check_dialog(gate, builds, sources):
    gate.start("Settings dialog glyphs")
    keys = {b["key"] for b in builds.values()}
    sites = prompt_dialog_sites(sources["launcher/prompt_dialog_sites.h"])
    gate.same_builds({f"{TITLE}-{v}" for v in sites}, keys, "launcher/prompt_dialog_sites.h")
    dispatch = dispatch_rows(sources["launcher/prompt_dialog.c"], "kDialog")
    gate.same_builds(dispatch, keys, "launcher/prompt_dialog.c build table")
    for key, symbols in dispatch.items():
        suffix = "_" + key.rsplit("-", 1)[1].replace(".", "_")
        gate.check(symbols and all(s.endswith(suffix) for s in symbols), f"launcher/prompt_dialog.c routes {key} to another build's site")
    reference = min(builds)
    for version in sorted(set(builds) & set(sites)):
        gate.check(sites[version] == sites.get(reference), f"CUSA57767 {version} dialog site differs from {reference} in prologue, range lengths or object layout")


def check_image_glyphs(gate, builds, sources):
    gate.start("Menu glyph Images")
    keys = {b["key"] for b in builds.values()}
    sites = image_glyph_sites(sources["launcher/image_glyphs_sites.h"])
    gate.same_builds({f"{TITLE}-{v}" for v in sites}, keys, "launcher/image_glyphs_sites.h")
    dispatch = dispatch_rows(sources["launcher/image_glyphs.c"], "kImageGlyphs")
    gate.same_builds(dispatch, keys, "launcher/image_glyphs.c build table")
    for key, symbols in dispatch.items():
        suffix = "_" + key.rsplit("-", 1)[1].replace(".", "_")
        gate.check(symbols and all(s.endswith(suffix) for s in symbols), f"launcher/image_glyphs.c routes {key} to another build's site")
    reference = min(builds)
    for version in sorted(set(builds) & set(sites)):
        gate.check(sites[version] == sites.get(reference), f"CUSA57767 {version} glyph hooks differ from {reference} in entry, length or object layout")


def check_exit_game(gate, builds, sources):
    gate.start("Title menu Exit Game")
    keys = {b["key"] for b in builds.values()}
    sites = exit_game_sites(sources["src/sos_exit_game_sites.h"])
    gate.same_builds({f"{TITLE}-{v}" for v in sites}, keys, "src/sos_exit_game_sites.h")
    dispatch = dispatch_rows(sources["src/sos_exit_game.c"], "kExitGame")
    gate.same_builds(dispatch, keys, "src/sos_exit_game.c build table")
    for key, symbols in dispatch.items():
        suffix = "_" + key.rsplit("-", 1)[1].replace(".", "_")
        gate.check(symbols and all(s.endswith(suffix) for s in symbols), f"src/sos_exit_game.c routes {key} to another build's site")
    reference = min(builds)
    for version in sorted(set(builds) & set(sites)):
        gate.check(sites[version] == sites.get(reference), f"CUSA57767 {version} Exit Game site differs from {reference} in prologue, range lengths or object layout")


def resolution_requests(text):
    """{version: [offset]} of the generated SetResolution sites."""
    found = {}
    for version, block in re.findall(r"#define SOS_SET_RESOLUTION_SITES_(\d+_\d+)((?:.*\\\n)*.*)", text):
        found[version.replace("_", ".")] = re.findall(r"SOS_SET_RESOLUTION_SITE\((0x[0-9a-f]+)u\)", block)
    return found


def check_resolution(gate, builds, sources):
    gate.start("Render resolution and True Ultrawide")
    rows = game_profile_rows(sources["launcher/game_profile.cpp"])
    for version, build in sorted(builds.items()):
        table = rows[build["key"]]["resolution"] if build["key"] in rows else None
        gate.check(table == f"kResolutionSites_{version.replace('.', '_')}", f"launcher/game_profile.cpp: CUSA57767 {version} must use its own resolution site table")
    requests = resolution_requests(sources["launcher/sos_resolution_sites.h"])
    gate.same_builds(set(requests), set(builds), "launcher/sos_resolution_sites.h")
    for version, offsets in sorted(requests.items()):
        gate.check(len(offsets) == 7 and len(set(offsets)) == 7, f"launcher/sos_resolution_sites.h: CUSA57767 {version} needs its 7 distinct SetResolution sites")


FEATURES = [check_identity, check_skip_intro, check_hermes_unlock, check_pit_unlock, check_prompts, check_dialog, check_image_glyphs, check_exit_game, check_resolution]

# Sources that carry build-keyed data, with the feature that owns them.
OWNERS = {
    "src/game_build.h": "Build identity", "launcher/game_profile.cpp": "Build identity", "scripts/game_profiles.py": "Build identity",
    "scripts/il2cpp_index.py": "Build identity", "src/sos_skip_intro.c": "Skip Intro",
    "src/sos_hermes_unlock.c": "Hermes Mode early unlock",
    "src/sos_pit_unlock.c": "The Pit early unlock",
    "launcher/controller_prompts.c": "Xbox controller prompts", "launcher/controller_prompts_sites.h": "Xbox controller prompts",
    "launcher/prompt_dialog.c": "Settings dialog glyphs", "launcher/prompt_dialog_sites.h": "Settings dialog glyphs",
    "launcher/image_glyphs.c": "Menu glyph Images", "launcher/image_glyphs_sites.h": "Menu glyph Images",
    "src/sos_exit_game.c": "Title menu Exit Game", "src/sos_exit_game_sites.h": "Title menu Exit Game",
    "launcher/sos_resolution_sites.h": "Render resolution and True Ultrawide",
}
# Host code with no build-specific data: one test covers it, not one per build.
SHARED_HOST = {"src/loader.c": "passes the title id, not a build, to the save and redzone layers"}

# Tests and checks that must name every supported build, or iterate the inventory.
COVERAGE = [
    ("tests/skip_intro_tests.cpp", "names"), ("tests/hermes_unlock_tests.cpp", "names"),
    ("tests/pit_unlock_tests.cpp", "names"), ("scripts/pit_unlock_check.py", "names"),
    ("tests/title_patch_matrix_tests.cpp", "names"), ("tests/controller_prompts_tests.cpp", "names"),
    ("tests/prompt_dialog_tests.cpp", "names"), ("tests/image_glyphs_tests.cpp", "names"),
    ("scripts/image_glyphs.py", "names"), ("tests/exit_game_tests.cpp", "names"), ("tests/render_resolution_tests.cpp", "inventory"),
    ("scripts/skip_intro_check.py", "names"), ("scripts/hermes_unlock_check.py", "names"),
    ("scripts/title_input_check.py", "names"), ("scripts/map_input_check.py", "names"),
    ("scripts/prompt_sites.py", "names"), ("scripts/prompt_dialog.py", "names"), ("scripts/exit_game_sites.py", "names"),
    ("tests/controller_icons_tests.cpp", "names"),
    ("tests/ultrawide_tests.cpp", "inventory"), ("scripts/ultrawide_check.py", "names"),
    ("scripts/graphics_import_check.py", "profiles"), ("scripts/graphics_state_check.py", "profiles"), ("tools/test_graphics_state.ps1", "profiles"),
]


def check_registration(gate, builds, sources):
    gate.start("Registered build-specific sources")
    literal = re.compile(r'"CUSA\d{5}-\d+\.\d+"')
    for folder in ("src", "launcher"):
        for path in sorted((ROOT / folder).rglob("*")):
            relative = path.relative_to(ROOT).as_posix()
            if path.suffix not in (".c", ".cpp", ".h", ".inc") or "third_party" in relative or relative in sources.get("_skip", ()):
                continue
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8", errors="replace"), flags=re.S)
            if literal.search(code):
                gate.check(relative in OWNERS or relative in SHARED_HOST or path.suffix == ".h" and relative.replace(".h", ".c") in OWNERS,
                           f"{relative} names a build in a string but is registered in neither OWNERS nor SHARED_HOST of scripts/build_parity_check.py")


def check_coverage(gate, builds, sources):
    gate.start("Tests name every build")
    for path, mode in COVERAGE:
        text = sources.get("cov:" + path)
        if text is None:
            gate.check(False, f"{path} is missing")
        elif mode == "inventory":
            gate.check("SupportedBuilds()" in text, f"{path} no longer iterates the supported builds")
        elif mode == "profiles":
            gate.check("PROFILES" in text or "build_parity_check.py" in text, f"{path} no longer iterates the supported builds")
        else:
            missing = [v for v in builds if not re.search(r"(?<![\d.])" + re.escape(v) + r"(?![\d.])", text)]
            gate.check(not missing, f"{path} never names CUSA57767 {', '.join(missing)}")
            unknown = {v for v in re.findall(r"CUSA57767-(\d+\.\d+)", text)} - set(builds) - UNSUPPORTED_PROBES
            gate.check(not unknown, f"{path} names builds that are not supported: {', '.join(sorted(unknown))}")


# --- real builds: fingerprints, boot images and the AvPlayer caller -------------------------------------------------

def llvm_mc():
    return shutil.which("llvm-mc") or "C:/msys64/clang64/bin/llvm-mc.exe"


def load_image(path):
    import ps4_module as pm
    module = pm.read_module(path)
    loads = [p for p in module["ph"] if p["type"] == 1]
    image = bytearray(max(p["vaddr"] + p["memsz"] for p in loads))
    for p in loads:
        image[p["vaddr"]:p["vaddr"] + p["filesz"]] = module["elf"][p["offset"]:p["offset"] + p["filesz"]]
    return bytes(image)


def decode(blob, base):
    listing = " ".join("0x%02x" % b for b in blob)
    out = subprocess.run([llvm_mc(), "--disassemble", "-triple=x86_64", "--show-encoding", "-output-asm-variant=1"], input=listing,
                         capture_output=True, text=True, check=True).stdout
    insns, at = [], 0
    for line in out.splitlines():
        encoding = re.search(r"encoding: \[(.*)\]", line)
        if encoding:
            size = len(encoding.group(1).split(","))
            insns.append((base + at, line.split("#")[0].strip()))
            at += size
    return insns


def normalise(text):
    text = re.sub(r"\[rip [+-] \d+\]", "[rip + X]", text)
    return re.sub(r"^(call|jmp)\s+-?\d+$", r"\1 X", re.sub(r"\s+", " ", text))


def avplayer_facts(insns, init_site, sync_site):
    """What the intro's player setup does, as {fact name: bool}, from the decoded window ending at the sync mode call."""
    text = [(at, normalise(t)) for at, t in insns]
    index = next((i for i, (at, _t) in enumerate(text) if at == init_site), None)
    facts = {"InitEx call is in the window": index is not None}
    if index is None:
        return facts
    before = text[max(0, index - 64):index]
    base = next((int(m.group(1)) for _a, t in reversed(before) for m in [re.match(r"lea rdi, \[rbp - (\d+)\]", t)] if m), None)
    facts["InitEx receives a stack parameter block"] = base is not None
    stores = {}
    for i, (_a, t) in enumerate(before):
        m = re.match(r"mov (?:dword|byte|qword|word) ptr \[rbp - (\d+)\], (\w+)$", t)
        if m and base is not None:
            stores[base - int(m.group(1))] = (m.group(2), i)
    facts["video buffer count is 6"] = stores.get(INIT_EX_BUFFERS, (None,))[0] == str(GAME_VIDEO_BUFFERS)
    facts["auto start is off"] = stores.get(INIT_EX_AUTO_START, (None,))[0] == "0"

    def loaded_from_code(register, position):
        for _a, t in reversed(before[:position]):
            m = re.match(r"(?:lea|mov|xor)\w* (\w+),", t)
            if m and m.group(1) == register:
                return t.startswith("lea") and "[rip + X]" in t
        return False
    facts["custom file callbacks are set"] = all(o in stores for o in INIT_EX_FILE_CALLBACKS) and all(
        loaded_from_code(*stores[o]) for o in INIT_EX_FILE_CALLBACKS[1:])
    sync = next((i for i, (at, _t) in enumerate(text) if at == sync_site), None)
    window = [t for _a, t in text[max(0, (sync or 0) - 4):sync]] if sync else []
    facts["sync mode is None"] = "mov esi, %d" % SYNC_MODE_NONE in window and not any(t.startswith("xor esi") or re.match(r"mov esi, (?!%d$)" % SYNC_MODE_NONE, t) for t in window)
    return facts


def check_real_builds(gate, builds, load):
    gate.start("Prepared builds")
    for version, build in sorted(builds.items()):
        eboot = ROOT / "game" / "prepared" / build["folder"] / "eboot.bin"
        gate.check(eboot.is_file(), f"{build['key']}: {eboot.relative_to(ROOT)} is not installed")
        if eboot.is_file():
            gate.check(hashlib.sha256(eboot.read_bytes()).hexdigest() == build["sha256"], f"{build['key']}: eboot.bin is not the supported build")
        from il2cpp_index import boot_image_path
        gate.check(boot_image_path(version) is not None, f"{build['key']}: no built boot image, so the patch sites were not read from its game code")
    gate.start("AvPlayer caller")
    import avplayer_check as av
    names = {name: nid for nid, name in av.symbol_names().items()}
    windows = {}
    for version, build in sorted(builds.items()):
        path = ROOT / "game" / "prepared" / build["folder"] / "eboot.bin"
        if not path.is_file():
            continue
        sites = av.scan(path)
        found = {n: [at for at, _k in sites.get(names[n], [])] for n in ("sceAvPlayerInitEx", "sceAvPlayerSetAvSyncMode", "sceAvPlayerSetLogCallback")}
        if not gate.check(all(len(v) == 1 for v in found.values()), f"{build['key']}: InitEx, SetAvSyncMode and SetLogCallback must each be called once: {found}"):
            continue
        init, sync, log = (found[n][0] for n in ("sceAvPlayerInitEx", "sceAvPlayerSetAvSyncMode", "sceAvPlayerSetLogCallback"))
        image = load(path)
        insns = decode(image[log + 5:sync + 5], log + 5)
        for fact, ok in avplayer_facts(insns, init, sync).items():
            gate.check(ok, f"{build['key']}: AvPlayer caller: {fact}")
        windows[version] = [normalise(t) for _a, t in insns]
    reference = min(windows) if windows else None
    for version, window in sorted(windows.items()):
        gate.check(window == windows[reference], f"CUSA57767 {version}: the AvPlayer setup instructions differ from {reference} beyond relocated addresses")
    check_crash_path(gate, builds, load)


def check_crash_path(gate, builds, load=load_image):
    gate.start("1.01 thread-pointer load")
    thread_load = bytes.fromhex("64488b042510000000")
    tcb_load = bytes.fromhex("64488b042500000000")
    expected_sites = [0x1a2e604, 0x1a2e674]
    for version, build in sorted(builds.items()):
        path = ROOT / "game" / "prepared" / build["folder"] / "eboot.bin"
        if not gate.check(path.is_file(), f"{build['key']}: {path.relative_to(ROOT)} is not installed"):
            continue
        import ps4_module as pm
        module = pm.read_module(path)
        sites, tcb = [], 0
        for segment in module["ph"]:
            if segment["type"] != 1 or not segment["flags"] & 1:
                continue
            blob = module["elf"][segment["offset"]:segment["offset"] + segment["filesz"]]
            at = blob.find(thread_load)
            while at >= 0:
                sites.append(segment["vaddr"] + at)
                at = blob.find(thread_load, at + 1)
            tcb += blob.count(tcb_load)
        if version == "1.00":
            gate.check(not sites, f"{build['key']}: no FS thread-pointer load was introduced by the base game")
            gate.check(tcb == 387, f"{build['key']}: expected 387 FS TCB loads, found {tcb}")
        else:
            gate.check(sites == expected_sites, f"{build['key']}: FS thread-pointer loads must be {[hex(a) for a in expected_sites]}, found {[hex(a) for a in sites]}")
            gate.check(tcb == 387, f"{build['key']}: expected 387 FS TCB loads, found {tcb}")
            image = load(path)
            for at in sites:
                gate.check(image[at - 4:at] == b"\x55\x48\x89\xe5", f"{build['key']}: image+{hex(at)} is not a function entry")
                gate.check(image[at:at + 9] == thread_load, f"{build['key']}: image+{hex(at)} is not the FS thread-pointer load")
                gate.check(image[at + 9:at + 15] == bytes.fromhex("c780d8000000"), f"{build['key']}: image+{hex(at + 9)} does not set the thread's +0xd8 field")
    builder = read("launcher/boot_image.cpp")
    python_builder = read("scripts/prepare_image.py")
    gate.check("kFsThreadLoad" in builder and "0x10, 0x00, 0x00, 0x00" in builder, "launcher/boot_image.cpp does not rewrite the FS thread-pointer load")
    gate.check("64488b042510000000" in python_builder, "scripts/prepare_image.py does not rewrite the FS thread-pointer load")
    loader = read("src/loader.c")
    gate.check("thread_load" in loader and "runtime_thread_thread_offset" in loader, "src/loader.c does not redirect the thread-pointer load to the TEB slot")
    threads = read("src/runtime_thread.c")
    gate.check("runtime_thread_thread_offset" in threads and "set_thread_slot(t)" in threads, "src/runtime_thread.c does not keep the guest thread handle in the TEB slot")
    gate.check("guest_cancel_enable" in threads and "offsetof(GuestThread, guest_cancel_enable) == 0xd0" in threads,
               "src/runtime_thread.c does not reserve the 1.01 guest cancellation fields at +0xd0")


def read_sources():
    sources = {path: read(path) for path in (
        "src/game_build.h", "launcher/game_profile.cpp", "src/sos_skip_intro.c", "src/sos_hermes_unlock.c", "src/sos_pit_unlock.c",
        "launcher/controller_prompts.c", "launcher/controller_prompts_sites.h", "launcher/prompt_dialog.c", "launcher/prompt_dialog_sites.h",
        "launcher/image_glyphs.c", "launcher/image_glyphs_sites.h",
        "src/sos_exit_game.c", "src/sos_exit_game_sites.h", "launcher/sos_resolution_sites.h")}
    for name in ("EBOOT_SHA256", "IMAGE_FOLDERS"):
        sources["il2cpp:" + name] = ast_assignment("scripts/il2cpp_index.py", name) or {}
    for name in ("prompt_sites", "prompt_dialog", "exit_game_sites"):
        sources[name + ":BUILDS"] = ast_assignment(f"scripts/{name}.py", "BUILDS") or ()
    for path, _mode in COVERAGE:
        sources["cov:" + path] = read(path) if (ROOT / path).is_file() else None
    return sources


def run_gate(builds, sources, real=True, load=load_image):
    gate = Gate()
    for feature in FEATURES + [check_registration, check_coverage]:
        feature(gate, builds, sources)
    if real:
        check_real_builds(gate, builds, load)
    return gate


def self_test():
    builds, sources = inventory(), read_sources()
    base = run_gate(builds, sources, real=False)
    results = []

    def expect_failure(name, mutate_builds=None, mutate=None):
        b = {k: dict(v) for k, v in builds.items()}
        s = dict(sources)
        if mutate_builds:
            mutate_builds(b)
        if mutate:
            mutate(s)
        before = sys.stdout
        sys.stdout = open(os.devnull, "w")
        try:
            gate = run_gate(b, s, real=False)
        finally:
            sys.stdout.close()
            sys.stdout = before
        results.append((name, bool(gate.failures)))

    results.append(("the real sources pass", not base.failures))
    expect_failure("a build added to the inventory but missing from every table", lambda b: b.update({"1.02": {"key": f"{TITLE}-1.02", "sha256": "0" * 64, "folder": f"{TITLE}-1.02"}}))
    expect_failure("1.01 dropped from the Skip Intro table", mutate=lambda s: s.update({"src/sos_skip_intro.c": re.sub(r'\{"CUSA57767-1\.01".*\n', "", s["src/sos_skip_intro.c"])}))
    expect_failure("1.01 dropped from the Hermes Mode table", mutate=lambda s: s.update({"src/sos_hermes_unlock.c": re.sub(r'\{"CUSA57767-1\.01".*\n', "", s["src/sos_hermes_unlock.c"])}))
    expect_failure("1.01 dropped from The Pit table", mutate=lambda s: s.update({"src/sos_pit_unlock.c": re.sub(r'\{"CUSA57767-1\.01".*\n', "", s["src/sos_pit_unlock.c"])}))
    expect_failure("1.01 dropped from the prompt dispatch table", mutate=lambda s: s.update({"launcher/controller_prompts.c": re.sub(r'.*"CUSA57767-1\.01".*\n', "", s["launcher/controller_prompts.c"])}))
    expect_failure("1.01 dropped from the dialog dispatch table", mutate=lambda s: s.update({"launcher/prompt_dialog.c": re.sub(r'.*"CUSA57767-1\.01".*\n', "", s["launcher/prompt_dialog.c"])}))
    expect_failure("1.01 dropped from the menu glyph dispatch table", mutate=lambda s: s.update({"launcher/image_glyphs.c": re.sub(r'.*"CUSA57767-1\.01".*\n', "", s["launcher/image_glyphs.c"])}))
    expect_failure("a menu glyph layout offset changed in 1.01", mutate=lambda s: s.update({"launcher/image_glyphs_sites.h": re.sub(r"(kImageGlyphs_1_01.*?)0xe0u, 0x20u", r"\g<1>0xe8u, 0x20u", s["launcher/image_glyphs_sites.h"], count=1, flags=re.S)}))
    expect_failure("1.01 dropped from the Exit Game dispatch table", mutate=lambda s: s.update({"src/sos_exit_game.c": re.sub(r'.*"CUSA57767-1\.01".*\n', "", s["src/sos_exit_game.c"])}))
    expect_failure("an Exit Game layout offset changed in 1.01", mutate=lambda s: s.update({"src/sos_exit_game_sites.h": re.sub(r"(kExitGame_1_01.*?0x10u, 0xb8u, )0x40u", r"\g<1>0x44u", s["src/sos_exit_game_sites.h"], count=1, flags=re.S)}))
    expect_failure("an Xbox patch byte changed in 1.01", mutate=lambda s: s.update({"launcher/controller_prompts_sites.h": re.sub(
        r"(kSites_1_01.*?\{0x02, 0, 0, 0\})", lambda m: m.group(1).replace("{0x02, 0, 0, 0}", "{0x01, 0, 0, 0}"), s["launcher/controller_prompts_sites.h"], count=1, flags=re.S)}))
    expect_failure("a dialog layout offset changed in 1.01", mutate=lambda s: s.update({"launcher/prompt_dialog_sites.h": re.sub(r"(kDialog_1_01.*?0x20, 0x20, 0x30, )0x168", r"\g<1>0x169", s["launcher/prompt_dialog_sites.h"], count=1, flags=re.S)}))
    expect_failure("a menu hook Image field changed in 1.01", mutate=lambda s: s.update({"launcher/prompt_dialog_sites.h": re.sub(r"(kDialog_1_01.*?4u, \{0x198u, )0x1a0u", r"\g<1>0x1a4u", s["launcher/prompt_dialog_sites.h"], count=1, flags=re.S)}))
    expect_failure("a 1.01 fingerprint changed in the build header", mutate=lambda s: s.update({"src/game_build.h": s["src/game_build.h"].replace("aafe8a2a", "bbbb8a2a")}))
    expect_failure("1.01 missing from the image tables", mutate=lambda s: s.update({"il2cpp:IMAGE_FOLDERS": {k: v for k, v in s["il2cpp:IMAGE_FOLDERS"].items() if k != "1.01"}}))
    expect_failure("a graphics check that stops iterating the supported builds", mutate=lambda s: s.update({"cov:scripts/graphics_import_check.py": s["cov:scripts/graphics_import_check.py"].replace("PROFILES", "X")}))
    expect_failure("a test that names only 1.00", mutate=lambda s: s.update({"cov:tests/skip_intro_tests.cpp": s["cov:tests/skip_intro_tests.cpp"].replace("1.01", "x")}))
    expect_failure("1.01 loses a SetResolution site", mutate=lambda s: s.update({"launcher/sos_resolution_sites.h": s["launcher/sos_resolution_sites.h"].replace("SOS_SET_RESOLUTION_SITE(0x50f541cu)", "")}))
    expect_failure("1.01 routed to the 1.00 resolution sites", mutate=lambda s: s.update({"launcher/game_profile.cpp": s["launcher/game_profile.cpp"].replace(".resolution_sites = kResolutionSites_1_01", ".resolution_sites = kResolutionSites_1_00")}))
    expect_failure("1.01 missing from the generated resolution sites", mutate=lambda s: s.update({"launcher/sos_resolution_sites.h": s["launcher/sos_resolution_sites.h"].replace("SOS_SET_RESOLUTION_SITES_1_01", "SOS_SET_RESOLUTION_SITES_X")}))
    expect_failure("the resolution table of 1.01 differs", mutate=lambda s: s.update({"launcher/game_profile.cpp": re.sub(r'(\.version = "1\.01".*?\.resolution_sites = )kResolutionSites', r"\1kOtherSites", s["launcher/game_profile.cpp"], count=1, flags=re.S)}))

    good = _synthetic_avplayer()
    results.append(("the AvPlayer analysis accepts the intro's setup", all(avplayer_facts(good, 0x100, 0x200).values())))
    for name, old, new in (("the video buffer count is not 6", "mov dword ptr [rbp - 1116], 6", "mov dword ptr [rbp - 1116], 4"),
                           ("auto start is on", "mov byte ptr [rbp - 1112], 0", "mov byte ptr [rbp - 1112], 1"),
                           ("the sync mode is not None", "mov esi, 1", "mov esi, 0"),
                           ("a file callback is not set", "mov qword ptr [rbp - 1224], rax", "nop"),
                           ("a file callback is not code", "lea rcx, [rip + 11]", "mov rcx, rbx")):
        mutated = [(a, t.replace(old, new)) for a, t in good]
        results.append((f"AvPlayer: {name}", not all(avplayer_facts(mutated, 0x100, 0x200).values())))
    results.append(("AvPlayer: 1.01 relocated addresses stay equal", [normalise(t) for _a, t in good] == [normalise(t.replace("[rip + 11]", "[rip + 4096]")) for _a, t in good]))
    results.append(("AvPlayer: an extra instruction differs", [normalise(t) for _a, t in good] != [normalise(t) for _a, t in good + [(0x300, "nop")]]))
    bad = [name for name, ok in results if not ok]
    for name in bad:
        print(f"  [fail] self-test: the gate did not catch: {name}")
    if bad:
        return 1
    print(f"  [parity] gate self-test: {len(results)} checks")
    return 0


def _synthetic_avplayer():
    """The intro's setup as the game's code has it, reduced to the parts the analysis reads."""
    base = 1280
    insns = [(0x10, f"lea rdi, [rbp - {base}]")]
    for slot, register in zip(INIT_EX_FILE_CALLBACKS[1:], ("rax", "rcx", "rdx", "rsi")):
        insns.append((0x20 + slot, f"lea {register}, [rip + 11]"))
    insns.append((0x20, f"mov qword ptr [rbp - {base - INIT_EX_FILE_CALLBACKS[0]}], rbx"))
    for slot, register in zip(INIT_EX_FILE_CALLBACKS[1:], ("rax", "rcx", "rdx", "rsi")):
        insns.append((0x60 + slot, f"mov qword ptr [rbp - {base - slot}], {register}"))
    insns.append((0x80, f"mov dword ptr [rbp - {base - INIT_EX_BUFFERS}], 6"))
    insns.append((0x90, f"mov byte ptr [rbp - {base - INIT_EX_AUTO_START}], 0"))
    insns.append((0x100, "call 4338248"))
    insns += [(0x180, "mov rdi, qword ptr [r15]"), (0x190, "mov esi, 1"), (0x200, "call 4337650")]
    return insns


def main(argv):
    builds = inventory()
    if "--builds" in argv:
        print("\n".join(b["key"] for b in builds.values()))
        return 0
    if "--self-test" in argv:
        return self_test()
    gate = run_gate(builds, read_sources())
    total = sum(gate.counts.values())
    for feature, count in gate.counts.items():
        print(f"  [parity] {feature}: {count} checks")
    labels = " and ".join(f"CUSA57767 {v}" for v in sorted(builds))
    if gate.failures:
        print(f"Build parity FAILED for {labels}: {len(gate.failures)} of {total} failed")
        return 1
    print(f"Build parity passed: {labels} validated")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
