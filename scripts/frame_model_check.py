"""Static check of the facts the Frame rate setting rests on, for every prepared build.

    python scripts/frame_model_check.py

The Unity player flips once per vblank when vSyncCount is 1, and its physics step is a fixed 1/60 s, so a guest vblank above
60 Hz gives new frames while the simulation keeps its step. Checks, per build, that the executable derives the video-out flip
rate from the sync interval as `interval != 1` and that the TimeManager holds the 1/60 fixed step. Nothing is executed.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import prompt_assets_check as assets  # noqa: E402
import ps4_module as pm  # noqa: E402

CLASS_TIME_MANAGER = 5
# mov edi,[rbx+0x2b0] ; xor esi,esi ; cmp r14d,1 ; setne sil ; call sceVideoOutSetFlipRate
SET_FLIP_RATE_FROM_INTERVAL = bytes.fromhex("8bbbb002000031f64183fe01400f95c6e8")
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def image_of(path):
    module = pm.read_module(path)
    loads = [p for p in module["ph"] if p["type"] == 1]
    image = bytearray(max(p["vaddr"] + p["memsz"] for p in loads))
    for p in loads:
        image[p["vaddr"]:p["vaddr"] + p["filesz"]] = module["elf"][p["offset"]:p["offset"] + p["filesz"]]
    return bytes(image)


def main():
    builds = sorted((ROOT / "game" / "prepared").glob("CUSA*/eboot.bin"))
    if not builds:
        print("  [skip] no prepared eboot.bin found")
        return 0
    for eboot in builds:
        name = eboot.parent.name
        print(name)
        sites = [m.start() for m in re.finditer(re.escape(SET_FLIP_RATE_FROM_INTERVAL), image_of(eboot))]
        check(len(sites) == 1, "the sync interval sets the flip rate to (interval != 1): one site" + (f" at 0x{sites[0]:x}" if sites else ""))
        managers = Path(eboot.parent / "Media" / "globalgamemanagers")
        if not managers.is_file():
            print("  [skip] globalgamemanagers is not installed")
            continue
        data, objects = assets.parse(managers)
        steps = [struct.unpack_from("<4f", data, offset) for offset, _size, class_id in objects.values()
                 if class_id == CLASS_TIME_MANAGER]
        check(len(steps) == 1, "one TimeManager")
        if steps:
            fixed, maximum, scale, _particle = steps[0]
            check(abs(fixed - 1 / 60) < 1e-6, f"the fixed physics step is 1/60 s ({fixed:.6f})")
            check(abs(maximum - 1 / 3) < 1e-6, f"the longest frame the game simulates is 1/3 s ({maximum:.6f})")
            check(scale == 1.0, "the time scale starts at 1")
    print(f"{len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
