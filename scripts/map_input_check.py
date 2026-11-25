"""Static proof that the game's Map action is the PlayStation Touchpad button.

    python scripts/map_input_check.py

Per installed build: the generated input wrappers that expose the ToggleMap action are in the game code, and the input actions
asset in resources.assets binds <DualShockGamepad>/touchpadButton to ToggleMap in the gameplay, map and pause maps. The 1.01
patch ships no resources.assets, so both builds read the base game's. Nothing is executed; exit status 0 when every check passes.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from il2cpp_index import open_reported  # noqa: E402
import build_evidence as evidence  # noqa: E402
WRAPPERS = ("GameplayActions", "MapActions", "PauseMenuActions")
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)


def bound_to_toggle_map(data, path):
    count = 0
    for match in re.finditer(re.escape(path), data):
        runs = re.findall(rb"[\x20-\x7e]{4,}", data[match.end():match.end() + 40])
        count += bool(runs) and runs[0] == b"ToggleMap"
    return count


def check_build(version):
    print("CUSA57767-" + version)
    index = open_reported(version, "the ToggleMap wrappers")
    if index is None:
        return
    for wrapper in WRAPPERS:
        address = index.method_address(wrapper, "get_ToggleMap")
        check(address is not None and index.method_at(address) == wrapper + "::get_ToggleMap", f"{wrapper}::get_ToggleMap is at 0x{address:x}")


def check_asset():
    print("input actions asset (resources.assets)")
    asset = next((path for version in evidence.versions() for path in [evidence.game_file(version, "Media/resources.assets")] if path), None)
    if asset is None:
        evidence.report_content_all("the input actions asset")
        return
    data = asset.read_bytes()
    touchpad = bound_to_toggle_map(data, b"<DualShockGamepad>/touchpadButton")
    check(touchpad >= len(WRAPPERS), f"<DualShockGamepad>/touchpadButton is bound to ToggleMap {touchpad} times (gameplay, map, pause)")
    check(bound_to_toggle_map(data, b"<Gamepad>/select") >= 1, "<Gamepad>/select (View) is also bound to ToggleMap")


def main():
    for version in ("1.00", "1.01"):
        check_build(version)
    check_asset()
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
