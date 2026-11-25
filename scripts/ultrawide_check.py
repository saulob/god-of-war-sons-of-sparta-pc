"""True Ultrawide static gate for CUSA57767 1.00 and 1.01, and the generator of launcher/sos_resolution_sites.h.

    python scripts/ultrawide_check.py              verify the generated sites and the game facts Hor+ relies on
    python scripts/ultrawide_check.py generate     write launcher/sos_resolution_sites.h from the built boot images
    python scripts/ultrawide_check.py --self-test  prove each check fails on a mutated copy of the facts

Nothing is executed and the game files are opened read-only. The scenes and UI assets are the base game's: 1.01 ships
none of them.
"""
import mmap
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from il2cpp_index import ROOT, open_build  # noqa: E402
import build_evidence as evidence  # noqa: E402
from prompt_sites import fnv64  # noqa: E402

HEADER = ROOT / "launcher" / "sos_resolution_sites.h"
MEDIA = evidence.media("1.00")
BUILDS = ("1.00", "1.01")

# Every Screen.SetResolution request of the game's code: mov edi, 1920; mov esi, 1080; mov edx, 1; call rax.
SET_RESOLUTION = bytes.fromhex("bf80070000be38040000ba01000000ffd0")
SET_RESOLUTION_ICALL = b"UnityEngine.Screen::SetResolution_Injected("
SET_RESOLUTION_METHODS = (
    ("UiScripts.Settings.GraphicsSettingsMultiChoiceOptionSlot", "SetResolution"),
    ("Subsystems.GeneralSettingsSubsystem", "InvokeGraphicsInt"),
    ("Subsystems.GraphicsSubsystem", "InvokeIntEvent"),
    ("Subsystems.GraphicsSubsystem", "UpdateGraphicIntEvents"),
    ("Subsystems.GraphicsSubsystem", "OnAspectRatioChanged"),
    ("Subsystems.GraphicsSubsystem", "OnResolutionScreenValueChanged"),
    ("<Coroutine_ApplyInitialSettings>d__10", "MoveNext"),
)
# Camera calls that would stop the projection or the CPU culling from following the screen's shape.
CAMERA_SHAPE_ICALLS = (b"UnityEngine.Camera::set_aspect(", b"UnityEngine.Camera::ResetAspect(",
                       b"UnityEngine.Camera::set_rect_Injected(", b"UnityEngine.Camera::set_pixelRect_Injected(",
                       b"UnityEngine.Camera::set_projectionMatrix(", b"UnityEngine.Camera::set_projectionMatrix_Injected(",
                       b"UnityEngine.Camera::ResetProjectionMatrix(",
                       b"UnityEngine.Camera::set_cullingMatrix(", b"UnityEngine.Camera::set_cullingMatrix_Injected(",
                       b"UnityEngine.Camera::ResetCullingMatrix(")
SHAPE_CALLERS_ALLOWED = re.compile(r"^(UnityEngine\.Camera::|UnityEngine\.Experimental\.Rendering\.Universal\.PixelPerfectCamera::|"
                                   r"UnityEngine\.InputSystem\.PlayerInputManager::)")

SPLASH_FILE, SPLASH_OBJECT = "level1", "SplashScreen"
SPLASH_ORIGINAL = struct.pack("<8f", 0, 0, 1, 1, 0, 0, 0, 0)
SPLASH_CENTRED = struct.pack("<8f", 0.5, 0.5, 0.5, 0.5, 0, 0, 1920, 1080)
HUD_CANVASES = ("PlayerHUD", "Pause Menu Canvas", "Tutorial Canvas", "Canvas_Caption", "SkipCutsceneCanvas",
                "Death Screen Canvas", "Canvas_SettingsMenu", "LaconiaMainMenuCanvas", "Save Manual Menu Canvas")
FULL_SCREEN_FADES = ("Fade", "FadeIn", "FadeImage", "BlackBG", "Vignette", "TransparentBlackPanel", "GradientFade_Image")
C_GAMEOBJECT, C_RECT, C_CAMERA, C_BEHAVIOUR = 1, 224, 20, 114
ASPECTS = (("16:9", 16 / 9), ("21:9", 21 / 9), ("32:9", 32 / 9))

failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


# --- boot images ---------------------------------------------------------------------------------------------------

def icall_references(index, code, low, image, prefixes):
    """{method name: count} of the RIP-relative LEAs in the code that load an icall name starting with one of `prefixes`."""
    names = {m.start() for prefix in prefixes for m in re.finditer(re.escape(prefix), image)}
    found = {}
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", code):
        target = low + m.start() + 7 + struct.unpack_from("<i", code, m.start() + 3)[0]
        if target in names:
            name = index.method_at(low + m.start())
            found[name] = found.get(name, 0) + 1
    return found


def build_facts(build):
    """The SetResolution sites and the camera-shape callers of one build, or None when it is not installed."""
    index = open_build(build)
    if index is None:
        return None
    names = index.all_method_names()
    low, high = min(names), max(names) + 0x4000
    code = bytes(index.boot.read(low, high - low))
    sites, problems = [], []
    for type_name, method in SET_RESOLUTION_METHODS:
        try:
            start = index.method_address(type_name, method)
        except LookupError as error:
            problems.append(str(error))
            continue
        begin, end = index.method_bounds(start)
        body = code[begin - low:end - low]
        hits = [m.start() for m in re.finditer(re.escape(SET_RESOLUTION), body)]
        if len(hits) != 1:
            problems.append(f"{type_name}::{method}: {len(hits)} requests")
            continue
        sites.append((begin + hits[0], f"{type_name}::{method}"))
    everywhere = sorted(low + m.start() for m in re.finditer(re.escape(SET_RESOLUTION), code))
    image = bytes(index.boot.read(0, index.boot.size))
    callers = icall_references(index, code, low, image, (SET_RESOLUTION_ICALL,))
    shape = icall_references(index, code, low, image, CAMERA_SHAPE_ICALLS)
    return {"sites": sites, "problems": problems, "everywhere": everywhere,
            "callers": {n for n in callers if not n.startswith("UnityEngine.Screen::")}, "shape_callers": set(shape)}


def synthetic_facts(build, sites):
    offsets = sites.get(build) or []
    names = [f"{t}::{m}" for t, m in SET_RESOLUTION_METHODS]
    return {"sites": list(zip(offsets, names)), "problems": [], "everywhere": list(offsets), "callers": set(names), "shape_callers": set()}


def synthetic_scenes(text):
    scalers = {canvas: {(1, (1920.0, 1080.0), 1, 0.0)} for canvas in HUD_CANVASES}
    splash = header_splash(text)
    return {"cameras": [("level9", 1, (0.0, 0.0, 1.0, 1.0), 0)], "pixel_perfect": 0, "scalers": scalers,
            "fades": [("level4", "Fade", (0.0, 0.0, 1.0, 1.0, 0, 0, 1920, 1080))], "videos": [(SPLASH_FILE, "VideoPlayer")],
            "splash": (splash[0], splash[1], SPLASH_ORIGINAL) if splash else None}


def check_build(build, facts, header_sites):
    print(f"CUSA57767 {build}")
    check(not facts["problems"], f"{build}: every SetResolution request found once in its method " + "; ".join(facts["problems"]))
    offsets = [offset for offset, _ in facts["sites"]]
    check(len(offsets) == len(SET_RESOLUTION_METHODS) and len(set(offsets)) == len(offsets),
          f"{build}: {len(SET_RESOLUTION_METHODS)} distinct 1920 x 1080 SetResolution requests")
    check(facts["everywhere"] == sorted(offsets), f"{build}: no other request of that shape exists in the game code")
    expected = {f"{t}::{m}" for t, m in SET_RESOLUTION_METHODS}
    check(facts["callers"] == expected, f"{build}: only those methods call Screen.SetResolution "
          f"(extra: {sorted(facts['callers'] - expected)}, missing: {sorted(expected - facts['callers'])})")
    check(header_sites.get(build) == sorted(offsets), f"{build}: launcher/sos_resolution_sites.h holds these sites")
    stray = sorted(n for n in facts["shape_callers"] if not SHAPE_CALLERS_ALLOWED.search(n))
    check(not stray, f"{build}: no game code sets a camera's aspect, rect, projection or culling matrix {stray}")


def header_sites(text):
    sites = {}
    for build in BUILDS:
        block = re.search(r"#define SOS_SET_RESOLUTION_SITES_" + build.replace(".", "_") + r"((?:.*\\\n)*.*)", text)
        sites[build] = sorted(int(v, 16) for v in re.findall(r"SOS_SET_RESOLUTION_SITE\((0x[0-9a-f]+)u\)", block.group(1))) if block else None
    return sites


# --- scenes and UI -------------------------------------------------------------------------------------------------

class Serialized:
    """A Unity 2022.3 serialized file without type trees: objects, their classes and the external files."""

    def __init__(self, path):
        with open(path, "rb") as file:
            d = self.data = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        version, = struct.unpack_from(">I", d, 8)
        pos = 20
        data_offset = struct.unpack_from(">I", d, 12)[0]
        if version >= 22:
            data_offset = struct.unpack_from(">q", d, 32)[0]
            pos = 48
        pos = d.find(b"\0", pos) + 1 + 4
        if d[pos]:
            raise ValueError(f"{path.name}: type trees are not supported")
        pos += 1
        types = []
        count = self.i32(pos)
        pos += 4
        for _ in range(count):
            class_id = self.i32(pos)
            pos += 7 + (16 if class_id == C_BEHAVIOUR else 0) + 16
            types.append(class_id)
        count = self.i32(pos)
        pos += 4
        self.objects = {}
        for _ in range(count):
            pos = (pos + 3) & ~3
            path_id, start = struct.unpack_from("<qq", d, pos)
            size, kind = struct.unpack_from("<Ii", d, pos + 16)
            self.objects[path_id] = (data_offset + start, size, types[kind])
            pos += 24
        count = self.i32(pos)
        pos += 4
        for _ in range(count):
            pos = ((pos + 3) & ~3) + 12
        count = self.i32(pos)
        pos += 4
        self.externals = []
        for _ in range(count):
            pos = d.find(b"\0", pos) + 1 + 20
            end = d.find(b"\0", pos)
            self.externals.append(Path(d[pos:end].decode()).name)
            pos = end + 1

    def i32(self, at):
        return struct.unpack_from("<i", self.data, at)[0]

    def body(self, path_id):
        offset, size, _ = self.objects[path_id]
        return bytes(self.data[offset:offset + size])

    def of_class(self, class_id):
        return [p for p, (_, _, c) in self.objects.items() if c == class_id]


def text_at(body, at):
    length, = struct.unpack_from("<I", body, at)
    return body[at + 4:at + 4 + length].decode("utf-8", "replace"), (at + 4 + length + 3) & ~3


class Media:
    def __init__(self, folder):
        self.folder, self.files, self.scripts = folder, {}, {}

    def file(self, name):
        if name not in self.files:
            path = self.folder / name
            self.files[name] = Serialized(path) if path.is_file() else None
        return self.files[name]

    def scenes(self):
        names = [p.name for p in self.folder.glob("level*") if re.fullmatch(r"level\d+", p.name)]
        return sorted(names, key=lambda n: int(n[5:]))

    def script(self, sf, body):
        file_id, path_id = struct.unpack_from("<iq", body, 16)
        owner = sf if file_id == 0 else self.file(sf.externals[file_id - 1])
        key = (id(owner), path_id)
        if key not in self.scripts:
            name = "?"
            if owner and path_id in owner.objects:
                b = owner.body(path_id)
                _, at = text_at(b, 0)
                cls, at = text_at(b, at + 20)
                namespace, _ = text_at(b, at)
                name = (namespace + "." if namespace else "") + cls
            self.scripts[key] = name
        return self.scripts[key]


def object_name(sf, body):
    game_object = struct.unpack_from("<q", body, 4)[0]
    if game_object not in sf.objects:
        return "?"
    g = sf.body(game_object)
    count = struct.unpack_from("<i", g, 0)[0]
    return text_at(g, 4 + 12 * count + 4)[0]


def components(sf, body):
    g = sf.body(struct.unpack_from("<q", body, 4)[0])
    count = struct.unpack_from("<i", g, 0)[0]
    return [struct.unpack_from("<q", g, 8 + 12 * i)[0] for i in range(count)]


def rect_fields_at(sf, rect):
    """File offset of a RectTransform's anchor min, anchor max, anchored position and size delta (8 floats)."""
    body = sf.body(rect)
    children = struct.unpack_from("<i", body, 52)[0]
    return sf.objects[rect][0] + 56 + 12 * children + 12


def rect_of(sf, body):
    for c in components(sf, body):
        if c in sf.objects and sf.objects[c][2] == C_RECT:
            at = rect_fields_at(sf, c)
            return c, struct.unpack_from("<8f", sf.data, at)
    return None, None


def scene_facts(media):
    facts = {"cameras": [], "pixel_perfect": 0, "scalers": {}, "fades": [], "videos": [], "splash": None}
    files = media.scenes() + sorted(p.name for p in media.folder.glob("*.assets"))
    for name in files:
        sf = media.file(name)
        if sf is None:
            continue
        for cam in sf.of_class(C_CAMERA):
            b = sf.body(cam)
            if object_name(sf, b) == "Main Camera":
                rect = struct.unpack_from("<4f", b, 100)
                facts["cameras"].append((name, b[128], rect, struct.unpack_from("<q", b, 152)[0]))
        for mb in sf.of_class(C_BEHAVIOUR):
            b = sf.body(mb)
            script = media.script(sf, b)
            owner = object_name(sf, b)
            if script.endswith("PixelPerfectCamera"):
                facts["pixel_perfect"] += 1
            elif script == "UnityEngine.UI.CanvasScaler" and owner in HUD_CANVASES:
                _, at = text_at(b, 28)
                mode, = struct.unpack_from("<i", b, at)
                reference = struct.unpack_from("<2f", b, at + 12)
                match_mode, match = struct.unpack_from("<if", b, at + 20)
                facts["scalers"].setdefault(owner, set()).add((mode, reference, match_mode, round(match, 3)))
            elif script in ("UnityEngine.UI.Image", "UnityEngine.UI.RawImage") and owner in FULL_SCREEN_FADES + (SPLASH_OBJECT,):
                rect, values = rect_of(sf, b)
                if values is None:
                    continue
                if owner == SPLASH_OBJECT and script.endswith("RawImage") and name == SPLASH_FILE:
                    at = rect_fields_at(sf, rect)
                    facts["splash"] = (at, len(sf.data), bytes(sf.data[at:at + 32]))
                elif owner in FULL_SCREEN_FADES and (values[0] <= 0 and values[2] >= 1 or values[6] >= 1900):
                    facts["fades"].append((name, owner, values))
        for pid in sf.of_class(328):
            facts["videos"].append((name, object_name(sf, sf.body(pid))))
    return facts


def check_scenes(facts, header):
    print("Scenes and UI (base game files, shared by both builds)")
    cams = facts["cameras"]
    check(cams and all(ortho == 1 for _, ortho, _, _ in cams), f"all {len(cams)} gameplay cameras are orthographic: the vertical view is the orthographic size")
    check(all(rect == (0.0, 0.0, 1.0, 1.0) and not target for _, _, rect, target in cams),
          "every gameplay camera draws the whole screen to the screen (no viewport rect, no render texture)")
    check(facts["pixel_perfect"] == 0, "no scene uses a Pixel Perfect Camera, which would crop to a reference shape")
    for canvas in HUD_CANVASES:
        modes = facts["scalers"].get(canvas)
        ok = bool(modes) and all(mode == 1 and reference == (1920.0, 1080.0) and (match_mode == 1 or match == 1.0)
                                 for mode, reference, match_mode, match in modes)
        check(ok, f"{canvas}: scaled evenly from 1920 x 1080 by the screen height (Expand or match height), never stretched")
    bad = [f"{n}:{o}" for n, o, v in facts["fades"] if not (v[0] <= 0 and v[1] <= 0 and v[2] >= 1 and v[3] >= 1 or v[6] >= 3840 and v[7] >= 2160)]
    check(facts["fades"] and not bad, f"{len(facts['fades'])} full-screen fades cover any width up to 3840 {bad[:5]}")
    full_screen_videos = {(n, o) for n, o in facts["videos"] if n == SPLASH_FILE}
    check(full_screen_videos == {(SPLASH_FILE, "VideoPlayer")}, "the boot splash is the only full-screen video")
    splash = facts["splash"]
    if check(splash is not None, "the splash video's RawImage is found"):
        offset, length, original = splash
        check(original == SPLASH_ORIGINAL, "the splash video's RawImage stretches over the whole canvas")
        check(header and header.get("splash") == (offset, length), "launcher/sos_resolution_sites.h holds its offset and the file length")


def header_splash(text):
    offset = re.search(r"#define SOS_SPLASH_RECT_OFFSET (0x[0-9a-f]+)u", text)
    length = re.search(r"#define SOS_SPLASH_FILE_LENGTH (\d+)u", text)
    return (int(offset.group(1), 16), int(length.group(1))) if offset and length else None


# --- generation ----------------------------------------------------------------------------------------------------

def render_header(per_build, splash):
    def byte_list(data):
        return ", ".join(f"0x{b:02x}" for b in data)
    lines = ["/* Generated by scripts/ultrawide_check.py generate: do not edit. */",
             "#ifndef SOS_RESOLUTION_SITES_H", "#define SOS_RESOLUTION_SITES_H", "",
             "#define SOS_SET_RESOLUTION_SITE(offset) \\",
             "    {offset, " + str(len(SET_RESOLUTION)) + ", SOS_SITE_SET_RESOLUTION, {" + byte_list(SET_RESOLUTION) + "}}", ""]
    for build in BUILDS:
        entries = [f"    SOS_SET_RESOLUTION_SITE(0x{offset:x}u) /* {name} */" for offset, name in per_build[build]]
        lines.append(f"#define SOS_SET_RESOLUTION_SITES_{build.replace('.', '_')} \\")
        lines.append(", \\\n".join(entries))
        lines.append("")
    offset, length, _ = splash
    lines += [f'#define SOS_SPLASH_GUEST_PATH "/app0/Media/{SPLASH_FILE}"', f"#define SOS_SPLASH_FILE_LENGTH {length}u",
              f"#define SOS_SPLASH_RECT_OFFSET 0x{offset:x}u", f"#define SOS_SPLASH_RECT_ORIGINAL {{{byte_list(SPLASH_ORIGINAL)}}}",
              f"#define SOS_SPLASH_RECT_CENTRED {{{byte_list(SPLASH_CENTRED)}}}", "", "#endif", ""]
    return "\n".join(lines)


def generate():
    per_build = {}
    for build in BUILDS:
        facts = build_facts(build)
        if facts is None or facts["problems"] or len(facts["sites"]) != len(SET_RESOLUTION_METHODS):
            print(f"CUSA57767 {build}: sites not found, nothing written")
            return 1
        per_build[build] = sorted(facts["sites"])
    splash = scene_facts(Media(MEDIA))["splash"]
    if not splash or splash[2] != SPLASH_ORIGINAL:
        print("splash RawImage not found, nothing written")
        return 1
    HEADER.write_text(render_header(per_build, splash), encoding="utf-8", newline="\n")
    print(f"wrote {HEADER.relative_to(ROOT)}")
    return 0


# --- self-test -----------------------------------------------------------------------------------------------------

def self_test(builds, scenes, text):
    import copy
    results = []

    def expect(name, run):
        before = len(failures)
        saved = sys.stdout
        sys.stdout = open("nul" if sys.platform == "win32" else "/dev/null", "w")
        try:
            run()
        finally:
            sys.stdout.close()
            sys.stdout = saved
        caught = len(failures) > before
        del failures[before:]
        results.append((name, caught))

    def build_case(mutate, build="1.01"):
        def run():
            facts = copy.deepcopy(builds)
            if mutate(facts[build]) == "missing":
                facts[build] = None
            for b in BUILDS:
                check_build(b, facts[b], header_sites(text))
        return run

    def scene_case(mutate):
        def run():
            facts = copy.deepcopy(scenes)
            mutate(facts)
            check_scenes(facts, {"splash": header_splash(text)})
        return run

    def header_case(old, new):
        return lambda: [check_build(b, builds[b], header_sites(text.replace(old, new, 1))) for b in BUILDS]

    expect("1.01 loses a SetResolution site", build_case(lambda f: f["sites"].pop()))
    expect("1.00 loses a SetResolution site", build_case(lambda f: f["sites"].pop(), "1.00"))
    expect("a method has a second request", build_case(lambda f: f["problems"].append("x: 2 requests")))
    expect("another 1920 x 1080 request exists", build_case(lambda f: f["everywhere"].append(1)))
    expect("a new Screen.SetResolution caller", build_case(lambda f: f["callers"].add("Foo::Bar")))
    expect("game code sets a camera aspect", build_case(lambda f: f["shape_callers"].add("CameraController::Update")))
    first_101 = re.search(r"SOS_SET_RESOLUTION_SITES_1_01 \\\n    SOS_SET_RESOLUTION_SITE\((0x[0-9a-f]+)u\)", text)
    if first_101:
        expect("the header holds a wrong 1.01 offset", header_case(first_101.group(1) + "u", hex(int(first_101.group(1), 16) + 1) + "u"))
    expect("the header uses 1.00 sites for 1.01", header_case("SOS_SET_RESOLUTION_SITES_1_01", "SOS_SET_RESOLUTION_SITES_X"))
    expect("a perspective gameplay camera", scene_case(lambda s: s["cameras"].append(("level9", 0, (0.0, 0.0, 1.0, 1.0), 0))))
    expect("a gameplay camera with a 16:9 viewport rect", scene_case(lambda s: s["cameras"].append(("level9", 1, (0.12, 0.0, 0.76, 1.0), 0))))
    expect("a Pixel Perfect Camera", scene_case(lambda s: s.update(pixel_perfect=1)))
    expect("the HUD matches the screen width", scene_case(lambda s: s["scalers"].update(PlayerHUD={(1, (1920.0, 1080.0), 0, 0.0)})))
    expect("the HUD at a constant pixel size", scene_case(lambda s: s["scalers"].update(PlayerHUD={(0, (800.0, 600.0), 0, 0.0)})))
    expect("a fade fixed at 1920 x 1080", scene_case(lambda s: s["fades"].append(("level4", "Fade", (0.5, 0.5, 0.5, 0.5, 0, 0, 1920, 1080)))))
    expect("a second full-screen video", scene_case(lambda s: s["videos"].append((SPLASH_FILE, "Intro"))))
    expect("the splash bytes changed", scene_case(lambda s: s.update(splash=(s["splash"][0], s["splash"][1], b"\0" * 32))))
    expect("the splash moved in its file", scene_case(lambda s: s.update(splash=(s["splash"][0] + 4, s["splash"][1], s["splash"][2]))))
    bad = [name for name, caught in results if not caught]
    for name in bad:
        print(f"  [fail] self-test: not caught: {name}")
    print(f"  [ultrawide] gate self-test: {len(results)} mutations, {len(results) - len(bad)} detected")
    return 1 if bad else 0


def main(argv):
    if argv[1:2] == ["generate"]:
        return generate()
    text = HEADER.read_text(encoding="utf-8") if HEADER.is_file() else ""
    sites = header_sites(text)
    real = {build: build_facts(build) for build in BUILDS}
    builds = {build: real[build] or synthetic_facts(build, sites) for build in BUILDS}
    real_scenes = scene_facts(Media(MEDIA)) if (MEDIA / "level3").is_file() and (MEDIA / SPLASH_FILE).is_file() else None
    scenes = real_scenes or synthetic_scenes(text)
    if argv[1:2] == ["--self-test"]:
        return self_test(builds, scenes, text)
    for build in BUILDS:
        evidence.report_image(build, "the SetResolution sites")
        if not real[build]:
            evidence.report_contract(build, "the sites of launcher/sos_resolution_sites.h")
        check_build(build, builds[build], sites)
    check_scenes(scenes, {"splash": header_splash(text)})
    evidence.report_content_all("the scenes and UI the Hor+ view relies on")
    print(f"  [ultrawide] {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
