"""DEBUG_SAULO: offline analysis of the automatic True Ultrawide captures (temporary, remove after the diagnosis).

    python scripts/analyze_ultrawide_capture.py [folder]   the newest build/logs/ultrawide-diagnostics run when omitted
    python scripts/analyze_ultrawide_capture.py --self-test
    python scripts/analyze_ultrawide_capture.py --check-sites   the camera and GameRuntime sites in the installed images

Reads only the uw_manual_*_frame.txt files the game writes and answers the same questions as the game's own
ultrawide_diagnostic_summary.txt, computed independently from the text as a cross-check.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COLUMNS = (100, 250, 400, 1680, 2960, 3110, 3260)
ROWS = (25, 50, 75)
NAMES = [f"x{c}y{r}" for r in ROWS for c in COLUMNS]
POINT = re.compile(r"(x\d+y\d+)=\(([^,()]+),([^,()]+),([^,()]+),([^,()]+)\)")


def number(text):
    try:
        return float(text)
    except ValueError:
        return float("nan")


def points(text):
    return {m[1]: tuple(number(m[i]) for i in range(2, 6)) for m in POINT.finditer(text)}


def is_extension(name, width, height):
    column = int(name[1:name.index("y")])
    x = min(round(column * width / 3360), width - 1)
    centre = height * 16 / 9
    if width <= centre + 1:
        return False
    side = (width - centre) / 2
    return x < side or x >= width - side


def aspect_class(width, height):
    if not width or not height:
        return "-"
    aspect = width / height
    for name, target in (("16:9", 16 / 9), ("21:9", 21 / 9), ("21:9", 64 / 27), ("32:9", 32 / 9)):
        if abs(aspect - target) / target < 0.02:
            return name
    return "other"


def wider(width, height):
    return height and width / height > 16 / 9 * 1.05


def dark(c):
    return max(c[:3]) < 0.06


def same(a, b):
    return all(abs(x - y) < 0.006 for x, y in zip(a[:3], b[:3]))


def parse(text):
    cap = {"name": "", "kind": "", "camera": None, "render": (0, 0), "display": (0, 0), "pixels": False,
           "passes": [], "ends": {}, "grid": [], "image": "none"}
    current = None
    for line in text.splitlines():
        m = re.match(r"DEBUG_SAULO True Ultrawide manual capture (\S+)", line)
        if m:
            cap["name"] = m[1]
            cap["kind"] = "manual"
            continue
        m = re.match(r"camera x=(\S+) y=(\S+) ortho=(\S+) updates", line)
        if m:
            cap["camera"] = tuple(number(m[i]) for i in (1, 2, 3))
            continue
        m = re.match(r"grid row \d+:(.*)", line)
        if m:
            cap["grid"].append([tuple(number(v) for v in cell.split(",")) for cell in m[1].split()])
            continue
        m = re.match(r"presented image file: (.*)", line)
        if m:
            cap["image"] = m[1].strip()
            continue
        m = re.match(r"guest render target (\d+)x(\d+)", line)
        if m:
            cap["render"] = (int(m[1]), int(m[2]))
            continue
        m = re.match(r"presented buffer \S+ (\d+)x(\d+)", line)
        if m:
            cap["display"] = (int(m[1]), int(m[2]))
            continue
        if line.startswith("pixel readback: ok"):
            cap["pixels"] = not re.search(r"\(0 samples", line)
            continue
        m = re.match(r"#\d+ PASS draws (\d+) .*final_candidate=(\d) role=([\w-]+)", line)
        if m:
            current = {"draws": int(m[1]), "final": m[2] == "1", "role": m[3], "address": None, "size": (0, 0),
                       "clear": None, "boxes": [], "sampled": [], "px": []}
            cap["passes"].append(current)
            continue
        if line.startswith("#"):
            current = None
            continue
        m = re.match(r"END (0x[0-9a-f]+) (\d+)x(\d+) display=(\d):(.*)", line)
        if m:
            cap["ends"][m[1]] = {"size": (int(m[2]), int(m[3])), "display": m[4] == "1", "points": points(m[5])}
            continue
        if current is None:
            continue
        m = re.match(r"  color0 (0x[0-9a-f]+) \S+ (\d+)x(\d+)", line)
        if m:
            current["address"], current["size"] = m[1], (int(m[2]), int(m[3]))
            continue
        m = re.match(r"  area \S+ reduced=\d clear0=1 rgba=\(([^)]*)\)", line)
        if m:
            current["clear"] = tuple(number(v) for v in m[1].split(","))
            continue
        m = re.match(r"  viewport (\S+) \S+ (\S+) \S+ depth \S+ \S+ scissor \S+ \S+ (\d+) \d+ draws", line)
        if m:
            current["boxes"].append((number(m[2]), int(m[3])))
            continue
        m = re.match(r"  samples (0x[0-9a-f]+) ", line)
        if m:
            current["sampled"].append(m[1])
            continue
        m = re.match(r"  px d#(\d+) (0x[0-9a-f]+)(.*)", line)
        if m and "unreadable" not in m[3]:
            current["px"].append((int(m[1]), m[2], points(m[3])))
    return cap


def dark_regions(grid, width, height):
    """4-connected groups (2+ cells) of dark cells of the 32 x 12 final grid, largest first."""
    if len(grid) != 12 or any(len(row) != 32 for row in grid) or not width or not height:
        return []
    seen, found = set(), []
    for r in range(12):
        for c in range(32):
            if (r, c) in seen or not dark(grid[r][c]):
                continue
            stack, cells = [(r, c)], []
            seen.add((r, c))
            while stack:
                y, x = stack.pop()
                cells.append((y, x))
                for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                    n = (y + dy, x + dx)
                    if 0 <= n[0] < 12 and 0 <= n[1] < 32 and n not in seen and dark(grid[n[0]][n[1]]):
                        seen.add(n)
                        stack.append(n)
            if len(cells) >= 2:
                cols, rows = [x for _, x in cells], [y for y, _ in cells]
                found.append((len(cells), min(cols) * width // 32, min(rows) * height // 12,
                              (max(cols) + 1) * width // 32, (max(rows) + 1) * height // 12))
    return sorted(found, key=lambda f: -f[0])[:8]


def trace(passes, address):
    """Points of one target through the frame: touched extension points, draws that reached or darkened them."""
    state, baseline, mixed = None, False, False
    touched, reaching, darkened = set(), set(), []
    width, height = 0, 0
    for p in passes:
        if p["address"] != address:
            continue
        width, height = p["size"]
        if p["clear"] and state is None:
            state, baseline = {n: p["clear"] for n in NAMES}, True
        for draw, addr, values in p["px"]:
            if addr != address:
                continue
            if state is None:
                state = dict(values)
                baseline = True
                first = list(values.values())
                mixed = not all(same(v, first[0]) for v in first)
                continue
            for name, value in values.items():
                if same(value, state.get(name, value)) and (value[3] == state.get(name, value)[3]):
                    continue
                touched.add(name)
                if is_extension(name, width, height):
                    reaching.add(draw)
                    if dark(value) and not dark(state[name]):
                        darkened.append(draw)
                state[name] = value
    return {"known": baseline and not mixed, "touched": touched, "reaching": reaching, "darkened": darkened,
            "size": (width, height)}


def analyze(cap):
    v = {"diagnosis": "unknown"}
    worlds = [p for p in cap["passes"] if p["role"] == "world"]
    v["world"] = worlds[0]["address"] if worlds else None
    v["clear"] = next((p["clear"] for p in worlds if p["clear"]), None)
    v["full_size"] = cap["display"] == cap["render"] and cap["render"][0] > 0
    sixteen = {p["address"] for p in cap["passes"] if p["address"] and aspect_class(*p["size"]) == "16:9" and p["size"][0] >= 960}
    v["intermediates"] = sorted(sixteen)
    v["composited"] = any(a in p["sampled"] for p in cap["passes"] if p["role"] == "display" or wider(*p["size"]) for a in sixteen)
    v["clipped"] = False
    for p in cap["passes"]:
        if not (p["role"] == "display" or wider(*p["size"])):
            continue
        width, height = p["size"]
        centre = height * 16 / 9
        for vw, sw in p["boxes"]:
            v["clipped"] |= any(w < width * 0.9 and abs(w - centre) / centre < 0.03 for w in (vw, sw))
    world = trace(cap["passes"], v["world"]) if v["world"] else None
    v["regions"] = dark_regions(cap["grid"], *cap["display"])
    v["known"] = bool(cap["pixels"] and world and world["known"])
    width, height = world["size"] if world else (0, 0)
    extension = [n for n in NAMES if is_extension(n, width, height)]
    v["points"] = len(extension)
    v["uncovered"] = len([n for n in extension if world and n not in world["touched"]])
    v["reaching"] = len(world["reaching"]) if world else 0
    v["darkened"] = world["darkened"] if world else []
    display_end = next((e for e in cap["ends"].values() if e["display"]), None)
    world_end = cap["ends"].get(v["world"]) if v["world"] else None
    v["display_dark"] = sum(1 for n in NAMES if display_end and is_extension(n, *display_end["size"]) and dark(display_end["points"][n])) if display_end else -1
    v["world_dark"] = sum(1 for n in extension if world_end and dark(world_end["points"][n])) if world_end else -1
    if v["clipped"]:
        v["diagnosis"] = "candidate viewport/scissor clipping"
    elif v["composited"]:
        v["diagnosis"] = "candidate fixed 16:9 intermediate"
    elif not v["known"]:
        v["diagnosis"] = "unknown"
    elif v["darkened"] and not (v["clear"] and dark(v["clear"])):
        v["diagnosis"] = "candidate dark draw"
    elif v["uncovered"]:
        v["diagnosis"] = "candidate missing world coverage"
    elif v["display_dark"] > 0 and v["world_dark"] == 0:
        v["diagnosis"] = "candidate post-process"
    return v


def yes(value):
    return "yes" if value else "no"


def summary(captures):
    lines = [f"offline analysis of {len(captures)} manual captures"]
    verdicts = []
    for i, c in enumerate(captures, 1):
        v = analyze(c)
        verdicts.append(v)
        regions = "; ".join(f"x=[{x0},{x1}] y=[{y0},{y1}] ({n} cells)" for n, x0, y0, x1, y1 in v["regions"]) or "none"
        lines += [f"manual capture {i} ({c['name']}): camera {c['camera']}, final target {c['render'][0]}x{c['render'][1]}, image {c['image']}",
                  f"  candidate dark regions: {regions}",
                  f"  fixed 16:9 intermediates: {yes(v['intermediates'])}{' (composited)' if v['composited'] else ''}",
                  f"  viewport/scissor clipping: {yes(v['clipped'])}",
                  f"  dark draw candidates: {yes(v['darkened'])}",
                  f"  missing coverage candidate: {yes(v['uncovered'])}",
                  f"  diagnosis: {v['diagnosis']}"]
    for i in range(1, len(captures)):
        a, b = verdicts[i - 1], verdicts[i]
        lines.append(f"capture {i + 1} vs {i}: dark regions {len(b['regions'])} vs {len(a['regions'])}, "
                     f"same largest region: {yes(bool(a['regions']) and bool(b['regions']) and a['regions'][0][1:] == b['regions'][0][1:])}")
    return "\n".join(lines)


def newest_run():
    base = ROOT / "build" / "logs" / "ultrawide-diagnostics"
    runs = sorted((p for p in base.glob("*") if p.is_dir()), key=lambda p: p.stat().st_mtime) if base.is_dir() else []
    return runs[-1] if runs else None


def load(folder):
    return [parse(p.read_text(encoding="utf-8", errors="replace")) for p in sorted(Path(folder).glob("uw_*_frame.txt"))]


def synthetic(kind, covered, blocker=False, clip=False, sixteen=False, dark_left=False):
    bg, art, black = "(0.302,0.284,0.272,1.000)", "(0.600,0.500,0.300,1.000)", "(0.000,0.000,0.000,1.000)"
    first = " ".join(f"{n}={art if covered or not is_extension(n, 3360, 1440) else bg}" for n in NAMES)
    lines = [f"DEBUG_SAULO True Ultrawide manual capture uw_{kind}_01",
             "camera x=-85.000 y=4.800 ortho=4.800 updates=10 (at the capture start)",
             "guest render target 3360x1440 aspect=2.333 aspect_class=21:9",
             "presented buffer 0x2000 3360x1440 aspect=2.333 aspect_class=21:9",
             "pixel readback: ok (3 samples, 21 points each)",
             "presented image file: uw_manual_01_present.png"]
    for r in range(12):
        cells = ["0.00,0.00,0.00" if dark_left and c < 8 else "0.60,0.50,0.30" for c in range(32)]
        lines.append(f"grid row {r:02d}: " + " ".join(cells))
    if sixteen:
        lines += ["", "#0 PASS draws 4 (indices 24) final_candidate=0 role=other, vs/ps 1 2",
                  "  color0 0x3000 R8G8B8A8Unorm 1920x1080 aspect=1.778 aspect_class=16:9"]
    lines += ["", "#1 PASS draws 3 (indices 18) final_candidate=0 role=world, vs/ps 1 2",
              "  color0 0x1000 R8G8B8A8Unorm 3360x1440 aspect=2.333 aspect_class=21:9",
              "  area 3360x1440 reduced=0 clear0=1 rgba=(0.302,0.284,0.272,1) clear0 ...",
              f"  viewport 0 0 {2560 if clip else 3360} 1440 depth 0 1 scissor 0 0 3360 1440 draws 3 (#0..#2)",
              "  draw d#0 vs 1 ps 2 prim 4 indices 6 inst 1 blend off mask 0xf viewport 0 0 3360 1440 scissor 0 0 3360 1440 tex none",
              f"  px d#0 0x1000 {first}"]
    if blocker:
        lines.append("  px d#2 0x1000 " + " ".join(f"{n}={black}" for n in NAMES if is_extension(n, 3360, 1440)))
    lines += ["", "#2 PASS draws 1 (indices 3) final_candidate=1 role=display, vs/ps 3 4",
              "  color0 0x2000 B8G8R8A8Srgb 3360x1440 aspect=2.333 aspect_class=21:9",
              "  viewport 0 0 3360 1440 depth 0 1 scissor 0 0 3360 1440 draws 1 (#3..#3)",
              "  samples 0x1000 R8G8B8A8Unorm 3360x1440 aspect=2.333 aspect_class=21:9"]
    if sixteen:
        lines.append("  samples 0x3000 R8G8B8A8Unorm 1920x1080 aspect=1.778 aspect_class=16:9")
    lines += ["", f"END 0x1000 3360x1440 display=0: {first}", f"END 0x2000 3360x1440 display=1: {first}"]
    return "\n".join(lines)


def self_test():
    failures = []

    def expect(name, ok):
        print(("  ok    " if ok else "  FAIL  ") + name)
        if not ok:
            failures.append(name)

    one = parse(synthetic("manual", False, dark_left=True))
    v = analyze(one)
    expect("parse: camera, sizes, grid, image, passes, ends", one["camera"] == (-85.0, 4.8, 4.8) and one["render"] == (3360, 1440)
           and len(one["grid"]) == 12 and one["image"] == "uw_manual_01_present.png" and len(one["passes"]) == 2
           and len(one["ends"]) == 2 and one["pixels"])
    expect("dark region from the grid: x=[0,840] y=[0,1440]", v["regions"] == [(96, 0, 0, 840, 1440)])
    expect("no baseline needed: a single capture is analysed", v["uncovered"] == 15 and v["diagnosis"] == "candidate missing world coverage")
    text = summary([one])
    for line in ("candidate dark regions: x=[0,840] y=[0,1440] (96 cells)", "missing coverage candidate: yes",
                 "diagnosis: candidate missing world coverage", "image uw_manual_01_present.png"):
        expect("summary: " + line, line in text)
    expect("a lit grid has no dark region", analyze(parse(synthetic("manual", True)))["regions"] == [])
    expect("two captures are compared", "capture 2 vs 1: dark regions 1 vs 1, same largest region: yes" in summary([one, one]))
    expect("blocker draw", analyze(parse(synthetic("manual", True, blocker=True)))["diagnosis"] == "candidate dark draw")
    expect("16:9 viewport", analyze(parse(synthetic("manual", True, clip=True)))["diagnosis"] == "candidate viewport/scissor clipping")
    expect("16:9 intermediate", analyze(parse(synthetic("manual", True, sixteen=True)))["diagnosis"] == "candidate fixed 16:9 intermediate")
    print(f"{'FAIL' if failures else 'ok'}: analyzer self-test, {len(failures)} failures")
    return 1 if failures else 0


def check_sites():
    """The camera telemetry hook (src/sos_uw_probe.c) sits on the verified bytes."""
    sys.path.insert(0, str(ROOT / "scripts"))
    from il2cpp_index import open_build

    source = (ROOT / "src" / "sos_uw_probe.c").read_text(encoding="utf-8")
    block = re.search(r"kPrefix\[SOS_UW_PROBE_PREFIX\] = \{([^}]*)\}", source)[1]
    prefix = bytes(int(b, 16) for b in re.findall(r"0x[0-9a-f]{2}", block))
    sites = dict((b, int(a, 16)) for b, a in re.findall(r'\{"CUSA57767-(1\.0\d)", (0x[0-9a-f]+)\}', source))
    runtime_block = re.search(r"kRuntimeSites\[\] = \{(.*?)\n\};", source, re.S)[1]
    runtime = dict((b, int(a, 16)) for b, a in re.findall(r'\{"CUSA57767-(1\.0\d)", (0x[0-9a-f]+)u?\}', runtime_block))
    failures = 0
    for build in ("1.00", "1.01"):
        index = open_build(build)
        if index is None:
            print(f"  FAIL  {build}: boot image and metadata not installed")
            failures += 1
            continue
        camera = index.method_address("Cinemachine.CinemachineBrain", "PushStateToUnityCamera", image="Cinemachine.dll")
        expected = sites.get(build, 0)
        camera_ok = (expected == camera and len(prefix) == 20
                     and bytes(index.boot.read(camera, len(prefix))) == prefix)
        print(f"  {'ok  ' if camera_ok else 'FAIL'}  {build}: PushStateToUnityCamera at {camera:#x}, probe site "
              f"{expected:#x}, 20-byte prologue {'matches' if camera_ok else 'differs'}")
        failures += not camera_ok
        runtime_site = 0
        try:
            launch = index.method_address("GameRuntime", "OnLaunchGame")
            code = bytes(index.boot.read(launch, 0x200))
            for match in re.finditer(rb"\x48\x8b\x0d....\x48\x89\xc3\x48\x8b\x81\xb8\x00\x00\x00\x48\x89\x18", code, re.S):
                disp = struct.unpack_from("<i", code, match.start() + 3)[0]
                runtime_site = launch + match.start() + 7 + disp
                break
        except LookupError:
            pass
        expected_runtime = runtime.get(build, 0)
        runtime_ok = runtime_site == expected_runtime and expected_runtime != 0
        print(f"  {'ok  ' if runtime_ok else 'FAIL'}  {build}: GameRuntime.instance class global at {runtime_site:#x}, "
              f"probe site {expected_runtime:#x}")
        failures += not runtime_ok
    return 1 if failures else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--check-sites" in argv:
        return check_sites()
    folder = Path(argv[0]) if argv else newest_run()
    if not folder or not folder.is_dir():
        print("no ultrawide-diagnostics run folder found")
        return 1
    captures = load(folder)
    print(f"run {folder}")
    print(summary(captures))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
