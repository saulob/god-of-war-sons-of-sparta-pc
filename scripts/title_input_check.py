"""Static proof of how PRESS ANY BUTTON reads the pad and of what Skip Intro, the Xbox prompt patch and the Automatic prompt hook
do to that path, from the game's IL2CPP metadata and boot image.

    python scripts/title_input_check.py

Per installed build: MainMenuState_WaitForInputPress takes only raw InputSystem events, the switch out of it needs the menu
open and idle, the natural end of the intro and the Skip Intro claim write the same byte, and no Xbox prompt site or hooked prompt
method lies in or is called from the title path. Nothing is executed; exit status 0 when every check passes.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from il2cpp_index import open_build  # noqa: E402

STATE = "StateMachineStateTypes.UI.MainMenu.MainMenuState"
WAIT = STATE + "_WaitForInputPress"
INTRO = STATE + "_Intro"
SITE_TYPES = ("InputOverseer", "UIControlTypeListener", "UiScripts.Widgets.WidgetKeybindElement")
FORBIDDEN = re.compile(r"Gamepad::get_current|Time::|Input::|Keyboard::get_current|InputSystem::(Update|get_settings)")
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def claim_constants():
    text = (ROOT / "src/sos_skip_intro.c").read_text()
    row = re.search(r"enum \{ kStateMainMenu = (0x[0-9a-f]+), kMenuState = (0x[0-9a-f]+), kMenuClosed = (\d+), "
                    r"kMenuProcessingLoad = (0x[0-9a-f]+) \};", text)
    return tuple(int(v, 0) for v in row.groups()) if row else None


def body(index, type_name, method):
    start, end = index.method_bounds(index.method_address(type_name, method))
    return start, bytes(index.boot.read(start, end - start))


def callees(index, start, code):
    names = index.all_method_names()
    found = set()
    for m in re.finditer(bytes([0xE8]), code):
        if m.start() + 5 <= len(code):
            target = start + m.start() + 5 + struct.unpack_from("<i", code, m.start() + 1)[0]
            if target in names:
                found.add(names[target])
    return found


def check_build(version):
    print("CUSA57767-" + version)
    index = open_build(version)
    if index is None:
        print("  [skip] not installed: the game code was not read")
        return
    constants = claim_constants()
    check(constants is not None, "the Skip Intro claim names the menu fields it reads")
    if constants is None:
        return
    main_menu, menu_state, closed, processing = constants
    check(index.field_offset(STATE, "mainMenu") == main_menu, "MainMenuState.mainMenu is at the claim's offset")
    check(index.field_offset(STATE, "switchToNextState") == 0x6C, "MainMenuState.switchToNextState is at +0x6c")
    check(index.field_offset("MenuSystem.Menu", "<CurrentState>k__BackingField") == menu_state, "Menu.CurrentState is at the claim's offset")
    check(index.field_offset("MenuSystem.MainMenu", "<IsProcessingLoadButtonVisibility>k__BackingField") == processing,
          "MainMenu.IsProcessingLoadButtonVisibility is at the claim's offset")
    states = [v for t in index.type_indices("State") for v in [index.enum_values("State")] if "Closed" in v]
    check(bool(states) and states[0]["Closed"] == closed and states[0]["Active"] == 0, "State.Closed is the value the claim tests")

    path = {}
    for type_name, methods in ((WAIT, ("EnterState", "TickState", "ExitState", "CheckForInputPress", "IsDpadPressedInEvent",
                                       "QueueSwitchToState")),
                               (INTRO, ("EnterState", "TickState", "QueueSwitchToNextState", "Coroutine_DelayEnterState")),
                               (STATE, ("TickState", "QueueSwitchToState"))):
        for method in methods:
            path[f"{type_name.rsplit('.', 1)[1]}::{method}"] = body(index, type_name, method)

    wait_calls = {name: callees(index, *path[name]) for name in path if name.startswith("MainMenuState_WaitForInputPress")}
    has = lambda name, suffix: any(c.endswith(suffix) for c in wait_calls[name])  # noqa: E731
    check(has("MainMenuState_WaitForInputPress::EnterState", "InputEventListener::op_Addition"),
          "entering PRESS ANY BUTTON subscribes to InputSystem.onEvent")
    check(has("MainMenuState_WaitForInputPress::ExitState", "InputEventListener::op_Subtraction"),
          "leaving it unsubscribes from InputSystem.onEvent")
    check(has("MainMenuState_WaitForInputPress::CheckForInputPress", "InputControlExtensions::GetFirstButtonPressOrNull"),
          "a press is the first pressed button control of the incoming event (a released-to-pressed state in the event)")
    check(not any(FORBIDDEN.search(c) for calls in wait_calls.values() for c in calls),
          "it reads no Gamepad.current, Input, Time, or frame count: no delta, frame or clock enters the decision")

    _, check_code = path["MainMenuState_WaitForInputPress::CheckForInputPress"]
    check(bytes.fromhex("80bb8c00000000") in check_code,
          "a press already taken (inputAlreadyEntered +0x8c) ends the handler")
    check(index.field_offset(WAIT, "inputAlreadyEntered") == 0x8C, "WaitForInputPress.inputAlreadyEntered is at +0x8c")

    _, tick = path["MainMenuState_WaitForInputPress::TickState"]
    closed_test = bytes.fromhex("83b888000000") + bytes([closed])
    busy_test = bytes.fromhex("80b830010000" "00")
    check(closed_test in tick and busy_test in tick and tick.index(closed_test) < tick.index(busy_test),
          "the switch out of PRESS ANY BUTTON needs the menu not closed and not processing the load button")
    check(bytes.fromhex("807e6c00") in tick or bytes.fromhex("41807e6c00") in tick, "it waits for the queued switch byte at +0x6c")

    _, queue = path["MainMenuState_Intro::QueueSwitchToNextState"]
    check(queue[:5].hex() == "c6476c01c3", "the intro's natural end writes the byte +0x6c = 1")
    stub = re.search(r"kTitleStubTail\[\] = \{(.*?)\};", (ROOT / "src/sos_skip_intro.c").read_text(), re.S)
    stub_bytes = bytes(int(v, 16) for v in re.findall(r"0x[0-9a-f]{2}", stub.group(1))) if stub else b""
    check(bytes.fromhex("41c6466c01") in stub_bytes, "the Skip Intro claim writes the same byte, and nothing else of the intro state")
    _, intro_tick = path["MainMenuState_Intro::TickState"]
    check(b"\x49\x8b\x76\x78" in intro_tick, "both paths then run the intro's own switch to nextState (+0x78) through StateMachine.SwitchToState")
    check(any(c.endswith("StateMachine::SwitchToState") for c in callees(index, *path["MainMenuState_Intro::TickState"])),
          "that switch is the state machine's SwitchToState, which runs the next state's EnterState")

    sites = prompt_sites(version)
    check(len(sites) == 14, "the Xbox prompt patch has 14 sites")
    ranges = {name: (start, start + len(code)) for name, (start, code) in path.items()}
    inside = [(hex(site), name) for site in sites for name, (low, high) in ranges.items() if low <= site < high]
    check(not inside, f"no Xbox prompt site lies in a method of the title path (hits: {inside})")
    hook_low, hook_high = hook_range(version)
    check(index.method_at(hook_low) == "InputOverseer::ResolveIconsForCurrentInput" and index.method_at(hook_high - 1) == "InputOverseer::ResolveIconsForCurrentInput",
          "the Automatic prompt hook is on InputOverseer.ResolveIconsForCurrentInput")
    overlapping = [name for name, (low, high) in ranges.items() if low < hook_high and hook_low < high]
    check(not overlapping, f"the hooked method is not a method of the title path (overlaps: {overlapping})")
    patched = skip_intro_writes(version, index)
    check(not any(hook_low <= at < hook_high for at in patched), "Skip Intro writes nothing inside the hooked method")
    methods = {index.method_at(at) for at in patched}
    check(methods == {"<Start>d__24::MoveNext", "<InitSaveSystem>d__44::MoveNext", "<LoadSaveTask>d__44::MoveNext",
                      "<CheckFileExist>d__6::MoveNext", INTRO + "::TickState"},
          f"Skip Intro writes only the splash coroutine, the save waits and the intro's switch check (writes in {sorted(methods)})")
    title_check = table_row(version)[7]
    in_intro = [at for at in patched if index.method_at(at) == INTRO + "::TickState"]
    check(in_intro == list(range(title_check, title_check + 5)),
          "its only write in the title path is the five bytes of the intro's switch check")
    reached = {c for name in path for c in callees(index, *path[name])}
    check(not any(c.split("::")[0] in SITE_TYPES for c in reached),
          "no method of the title path calls a method of a type the Xbox prompt patch or the Automatic hook changes")


def table_row(version):
    text = (ROOT / "src/sos_skip_intro.c").read_text()
    for key, size, rest in re.findall(r'\{"(CUSA\d+-[\d.]+)", (\d+)u((?:, 0x[0-9a-f]+){8})\}', text):
        if key.endswith(version):
            return (int(size),) + tuple(int(v, 16) for v in rest.split(", ")[1:])
    raise LookupError(version)


def skip_intro_writes(version, index):
    """Image offsets the Skip Intro patch writes: the coroutine's points, the delay immediates and the title check."""
    _, c, init_save_system, task_delay, task_delay_other, load_save_task, check_file_exist, title_check, _ = table_row(version)
    offsets = [c + 0x1A9 + i for i in range(6)] + [c + 0x561 + i for i in range(6)] + [c + 0x8A6 + i for i in range(51)]
    offsets += [c + 0x10AA + i for i in range(11)] + [init_save_system + 0x1F8 + i for i in range(4)]
    offsets += [title_check + i for i in range(5)]
    for start, length in ((load_save_task, 0x3430), (check_file_exist, 0x3610)):
        code = bytes(index.boot.read(start, length))
        for m in re.finditer(bytes([0xBF, 0x64, 0x00, 0x00, 0x00, 0x31, 0xF6, 0xE8]), code):
            target = start + m.start() + 8 + 4 + struct.unpack_from("<i", code, m.start() + 8)[0]
            if target in (task_delay, task_delay_other):
                offsets += [start + m.start() + 1 + i for i in range(4)]
    return offsets


def hook_range(version):
    text = (ROOT / "launcher/controller_prompts_sites.h").read_text()
    row = re.search(r"kHook_" + version.replace(".", "_") + r" = \{0x([0-9a-f]+)u, 0x([0-9a-f]+)u", text)
    return int(row.group(1), 16), int(row.group(1), 16) + int(row.group(2), 16)


def prompt_sites(version):
    text = (ROOT / "launcher/controller_prompts_sites.h").read_text()
    section = text.split("/* CUSA57767-1.01 */")
    chosen = section[0] if version == "1.00" else section[1]
    return [int(m.group(1), 16) for m in re.finditer(r"\{0x([0-9a-f]+)u, \d,", chosen)]


def main():
    for version in ("1.00", "1.01"):
        check_build(version)
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
