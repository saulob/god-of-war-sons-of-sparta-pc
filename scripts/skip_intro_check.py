"""Static proof of what Skip Intro patches (src/sos_skip_intro.c), from the game's IL2CPP metadata and boot image.

    python scripts/skip_intro_check.py

For each installed build: the patched coroutine is UiScripts.SplashScreen.<Start>d__24.MoveNext at the table's offset, the
object layouts the patch relies on are the ones the metadata names, the ending the first state jumps to is what runs after
the video loop and still marks the game as booted, saves it, loads the FMOD banks and the scenes, the game's own save
system sets the settings defaults the patch waits for, and no branch from elsewhere lands in the code the patch reuses.
Nothing is executed (llvm-mc only decodes bytes); exit status 0 when every check passes.
"""
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from il2cpp_index import open_build, relative_calls  # noqa: E402

FIRST_STATE_JUMP, LOOP_EXIT, SKIP_CHECK, SKIP_BRANCH, ENDING, NEXT_FRAME, SAVE_CHECK, SAVE_READY = (
    0x1A9, 0x82F, 0x898, 0x8A6, 0xA0E, 0x9B2, 0x10AA, 0x10B5)
RESUME_CASE, RESUME_JUMP, VIDEO_LOOP, SAVE_LOOKUP, ENDING_FLAG = 0x555, 0x561, 0x80E, 0x1046, 0xF87
COROUTINE_LENGTH = 0x1A40
BRANCH = re.compile(r"(j\w+)\s+(-?\d+)$")
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


def site_table():
    text = (ROOT / "src/sos_skip_intro.c").read_text()
    rows = re.findall(r'\{"(CUSA\d+-[\d.]+)", (\d+)u((?:, 0x[0-9a-f]+){8})\}', text)
    return {row[0]: (int(row[1]),) + tuple(int(v, 16) for v in row[2].split(", ")[1:]) for row in rows}


def llvm_mc():
    return shutil.which("llvm-mc") or str(Path("C:/msys64/clang64/bin/llvm-mc.exe"))


def decode(blob, seeds):
    """Instruction boundaries of the function by recursive descent: {offset: (size, text)}."""
    instructions, pending = {}, list(seeds)
    while pending:
        at = pending.pop()
        if at in instructions or not 0 <= at < len(blob):
            continue
        listing = " ".join("0x%02x" % b for b in blob[at:])
        out = subprocess.run([llvm_mc(), "--disassemble", "-triple=x86_64", "--show-encoding"], input=listing,
                             capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            encoding = re.search(r"encoding: \[(.*)\]", line)
            if not encoding:
                continue
            size, text = len(encoding.group(1).split(",")), line.split("#")[0].strip()
            if at in instructions:
                break
            instructions[at] = (size, text)
            branch = BRANCH.match(text)
            if branch:
                pending.append(at + size + int(branch.group(2)))
            if re.match(r"(jmp|ud2|ret)", text):
                break
            at += size
    return instructions


def branches_into(instructions, lo, hi):
    hits = []
    for at, (size, text) in instructions.items():
        branch = BRANCH.match(text)
        if branch and not lo <= at < hi and lo < at + size + int(branch.group(2)) < hi:
            hits.append(at)
    return hits


def switch_targets(boot, coroutine):
    code = bytes(boot.read(coroutine, 0x200))
    lea = code.index(bytes([0x48, 0x8D, 0x0D]), 0x160)
    table = coroutine + lea + 7 + struct.unpack_from("<i", code, lea + 3)[0]
    return [table + struct.unpack("<i", boot.read(table + 4 * k, 4))[0] - coroutine for k in range(11)]


def check_startup_delay(index, move_next, task_delay):
    names = index.all_method_names()
    movenext = [index._address(m) for t in index.type_indices("<InitSaveSystem>d__44") for m, record in index.type_methods(t)
                if index.meta.string(record[0]) == "MoveNext"]
    check(move_next in movenext, "the save system's start is <InitSaveSystem>d__44.MoveNext at the table's offset")
    check(names.get(task_delay) == "System.Threading.Tasks.Task::Delay", "the call target is System.Threading.Tasks.Task.Delay")
    code = bytes(index.boot.read(move_next, 0x890))
    immediates = [m.start() for m in re.finditer(re.escape(bytes.fromhex("10270000")), code)]
    call_target = move_next + 0x1FE + 5 + struct.unpack_from("<i", code, 0x1FF)[0]
    check(immediates == [0x1F8] and code[0x1F7] == 0xBF and code[0x1FC:0x1FF].hex() == "31f6e8" and call_target == task_delay,
          "the only 10000 in the coroutine is the argument of its Task.Delay call (mov edi,10000 ; xor esi,esi ; call)")
    calls = names_calls(index, move_next, 0x890)
    check(calls.index("SaveSystem::LoadAll") > calls.index("System.Threading.Tasks.Task::Delay"),
          "the delay is awaited before LoadAll, and the coroutine waits on nothing else first")
    initialize = bytes(index.boot.read(index.method_address("SaveSystem", "Initialize"), 0x30))
    check(index.field_offset("SaveSystem", "awaitInit") == 0x58 and initialize[0x0B] == 0xE8 and initialize[0x10:0x14].hex() == "48894358",
          "SaveSystem.Initialize stores the task InitSaveSystem returns in awaitInit (+0x58) after the call")
    check(code.find(bytes.fromhex("49c7465800000000")) >= 0,
          "the coroutine clears awaitInit when its work is done, so the delay has to stay asynchronous")


def method_addresses(index, type_name, method_name):
    return [index._address(m) for t in index.type_indices(type_name) for m, record in index.type_methods(t)
            if index.meta.string(record[0]) == method_name]


def delay_waits(index, start, length, delays):
    """[(offset, milliseconds)] of every `mov edi,<ms> ; xor esi,esi ; call` that ends in one of the Task.Delay addresses."""
    code = bytes(index.boot.read(start, length))
    found = []
    for m in re.finditer(re.escape(bytes([0xBF])), code):
        at = m.start()
        if at + 12 <= len(code) and code[at + 5:at + 8] == bytes([0x31, 0xF6, 0xE8]):
            target = start + at + 12 + struct.unpack_from("<i", code, at + 8)[0]
            if target in delays:
                found.append((at + 1, struct.unpack_from("<i", code, at + 1)[0]))
    return found, code


def callers_of(index, name):
    names = index.all_method_names()
    targets = {a for a, n in names.items() if n == name}
    first, code = index.game_code()
    return sorted({index.method_at(at) for at, _ in relative_calls(code, first, targets)})


def check_save_polls(index, task_delay, task_delay_other, load_save_task, check_file_exist):
    names = index.all_method_names()
    check(names.get(task_delay) == names.get(task_delay_other) == "System.Threading.Tasks.Task::Delay",
          "both delay addresses are System.Threading.Tasks.Task.Delay overloads")
    check(load_save_task in method_addresses(index, "<LoadSaveTask>d__44", "MoveNext"),
          "the load task is <LoadSaveTask>d__44.MoveNext at the table's offset")
    check(check_file_exist in method_addresses(index, "<CheckFileExist>d__6", "MoveNext"),
          "the existence check is <CheckFileExist>d__6.MoveNext at the table's offset")
    check(index.method_bounds(load_save_task)[1] - load_save_task == 0x3430 and index.method_bounds(check_file_exist)[1] - check_file_exist == 0x3610,
          "the two coroutines have the table's lengths")
    delays = {task_delay, task_delay_other}
    waits, load_code = delay_waits(index, load_save_task, 0x3430, delays)
    check([ms for _, ms in waits] == [100] * 25, "LoadSaveTask awaits Task.Delay(100) 25 times and nothing else")
    waits, exist_code = delay_waits(index, check_file_exist, 0x3610, delays)
    check(sorted(ms for _, ms in waits) == [100] * 12 + [500] * 2,
          "CheckFileExist awaits Task.Delay(100) 12 times and Task.Delay(500) twice, nothing else")
    arithmetic = re.compile(b"(\x83[\xc0-\xc7\xf8-\xff]\x64|\x3d\x64\x00\x00\x00|\x81[\xc0-\xc7\xf8-\xff]\x64\x00\x00\x00)")
    check(not arithmetic.search(load_code) and not arithmetic.search(exist_code),
          "no wait is counted or compared against 100: the delays are polling intervals, not timeouts")
    owners = callers_of(index, "PS4SaveTasks::LoadSaveTask")
    check(len(owners) == 1 and owners[0].startswith("<PS4_Load>d__") and owners[0].endswith("::MoveNext"),
          "LoadSaveTask is called only by the save load coroutine PS4Controller.PS4_Load")
    check("<CheckFileExist>d__6::MoveNext" in callers_of(index, "PS4SaveTasks::CheckFileExist") and
          callers_of(index, "PS4SaveTasks::CommitSaveTask") != [],
          "CheckFileExist is the existence check and the write path (CommitSaveTask) has its own code, untouched")


def check_title_intro(index, title_check, base_tick):
    intro = "StateMachineStateTypes.UI.MainMenu.MainMenuState_Intro"
    tick = index.method_address(intro, "TickState")
    check(title_check == tick + 0x3C, "the switch check is at the intro's TickState + 0x3c")
    check(index.method_address("StateMachineStateTypes.UI.MainMenu.MainMenuState", "TickState") == base_tick,
          "the call before the check is MainMenuState.TickState")
    queue = index.method_address(intro, "QueueSwitchToNextState")
    check(bytes(index.boot.read(queue, 5)).hex() == "c6476c01c3", "QueueSwitchToNextState sets the byte at +0x6c and returns")
    check(callers_of(index, "StateMachineStateTypes.UI.MainMenu.MainMenuState_Intro::TickState") == [],
          "nothing calls the intro's TickState directly: only the state machine's virtual tick reaches it")


def check_title_guard(index, key):
    text = (ROOT / "src/sos_skip_intro.c").read_text()
    row = re.search(r'\{"' + re.escape(key) + r'", (0x[0-9a-f]+), (0x[0-9a-f]+)\}', text)
    cancel = index.method_address("Misc.ExecuteEventOnAnimationFinished", "CancelCoroutine")
    check(row is not None and int(row.group(1), 16) == cancel,
          "the title animation watcher's CancelCoroutine is at the guard table's offset")
    check(row is not None and int(row.group(2), 16) == 0x10, "the guard reads the il2cpp object's native pointer at +0x10")
    entry = bytes(index.boot.read(cancel, 14)).hex()
    check(entry == "554889e5535048" + "8b77584885f674",
          "CancelCoroutine starts by loading the wait routine at +0x58 and testing it")


def names_calls(index, start, length):
    names = index.all_method_names()
    code = bytes(index.boot.read(start, length))
    found = []
    for m in re.finditer(bytes([0xE8]), code):
        if m.start() + 5 <= len(code):
            target = start + m.start() + 5 + struct.unpack_from("<i", code, m.start() + 1)[0]
            if target in names:
                found.append(names[target])
    return found


def check_build(key, version, table):
    print(key)
    index = open_build(version)
    if index is None:
        print("  [skip] not installed: the game code was not read")
        return
    if not check(key in table, "the site table has the build"):
        return
    size, coroutine, init_save_system, task_delay, task_delay_other, load_save_task, check_file_exist, title_check, base_tick = table[key]
    check(index.boot.size == size, "the boot image has the table's size")
    movenext = [index._address(m) for t in index.type_indices("<Start>d__24") for m, record in index.type_methods(t)
                if index.meta.string(record[0]) == "MoveNext"]
    check(coroutine in movenext, "the coroutine is a <Start>d__24.MoveNext at the table's offset")
    check_startup_delay(index, init_save_system, task_delay)
    check_save_polls(index, task_delay, task_delay_other, load_save_task, check_file_exist)
    check_title_intro(index, title_check, base_tick)
    check_title_guard(index, key)
    splash_start = index.method_address("UiScripts.SplashScreen", "Start")
    check(index.method_at(splash_start) == "UiScripts.SplashScreen::Start", "UiScripts.SplashScreen.Start exists")

    layout = {
        ("<Start>d__24", "<>1__state"): 0x10, ("<Start>d__24", "<>4__this"): 0x20,
        ("UiScripts.SplashScreen", "videoPlayer"): 0x30, ("UiScripts.SplashScreen", "skipFinished"): 0x90,
        ("UiScripts.SplashScreen", "isSplashScreenVisualSequenceFinished"): 0xA2,
        ("SaveSystem", "isInitialized"): 0x11, ("SaveSystem", "currentGlobalData"): 0x20,
        ("GlobalSaveDataV1", "previouslyBootedGame"): 0x14, ("GlobalSaveDataV1", "previouslyInitializedToDefaults"): 0x1C,
    }
    for (type_name, field), offset in layout.items():
        try:
            actual = index.field_offset(type_name, field)
        except LookupError:
            actual = None
        check(actual == offset, f"{type_name}.{field} is at +0x{offset:x}")

    code = bytes(index.boot.read(coroutine, COROUTINE_LENGTH))
    exit_target = LOOP_EXIT + 6 + struct.unpack_from("<i", code, LOOP_EXIT + 2)[0]
    check(code[LOOP_EXIT - 4:LOOP_EXIT + 2].hex() == "ffd084c00f84" and exit_target == ENDING,
          "the video loop leaves for the ending when the video is not playing")
    skip_target = SKIP_CHECK + 14 + struct.unpack_from("<i", code, SKIP_CHECK + 10)[0]
    check(skip_target == NEXT_FRAME and code[NEXT_FRAME + 19:NEXT_FRAME + 28].hex() == "41c744241006000000",
          "without the user skip the video loop yields for a frame as state 6, which resumes at the video loop")
    check(code[FIRST_STATE_JUMP - 12:FIRST_STATE_JUMP].hex() == "4d85ed41c7442410ffffffff" and
          code[FIRST_STATE_JUMP:FIRST_STATE_JUMP + 2].hex() == "0f84",
          "the first state is <>1__state = -1 followed by a null check of the splash screen")
    check(code[SAVE_CHECK - 5] == 0xE8 and code[SAVE_CHECK:SAVE_CHECK + 8].hex() == "4885c0747d807811",
          "the save check tests the save system for null and isInitialized (+0x11)")
    check(code[0x1117:0x111B].hex() == "c6471401", "the ending sets previouslyBootedGame (+0x14) on currentGlobalData")

    targets = switch_targets(index.boot, coroutine)
    resume_target = RESUME_JUMP + 6 + struct.unpack_from("<i", code, RESUME_JUMP + 2)[0]
    check(targets[6] == RESUME_CASE and code[RESUME_CASE + 12:RESUME_CASE + 14].hex() == "0f85" and resume_target == VIDEO_LOOP,
          "state 6 resumes at the video loop through the branch the patch redirects")
    yields = [m.start() for m in re.finditer(re.escape(bytes.fromhex("41c744241006000000")), code)]
    check(yields == [NEXT_FRAME + 19], "state 6 is written only by the video loop's frame yield the wait reuses")
    check(code[SAVE_LOOKUP:SAVE_LOOKUP + 3].hex() == "4c8b35" and code[SAVE_LOOKUP + 7:SAVE_LOOKUP + 12].hex() == "49837e3800",
          "the ending's save system lookup starts with the class statics and needs only r12, r13 and r15")
    check(code[ENDING_FLAG:ENDING_FLAG + 10].hex() == "6641c785a100000000" + "01" and ENDING < ENDING_FLAG < SAVE_LOOKUP,
          "the ending sets isSplashScreenVisualSequenceFinished (+0xa2) before the save check")
    flag_writes = sorted(m.start() for pattern in ("41c685a2000000", "6641c785a1")
                         for m in re.finditer(re.escape(bytes.fromhex(pattern)), code))
    check(flag_writes == [0xE96, ENDING_FLAG],
          "the finished flag is set only by the ending and by the tail of the user skip")

    names = index.all_method_names()
    callees = {}
    for m in re.finditer(bytes([0xE8]), code):
        at = m.start()
        if at + 5 <= len(code):
            target = coroutine + at + 5 + struct.unpack_from("<i", code, at + 1)[0]
            if target in names:
                callees.setdefault(names[target], []).append(at)
    for name in ("SaveSystem::SaveGlobalData", "SceneManager::LoadScene", "RuntimeManager::LoadBank"):
        found = [at for callee, offsets in callees.items() if callee.endswith(name) for at in offsets]
        check(bool(found) and all(at > ENDING for at in found), f"{name} is called only from the ending's code")

    defaults = index.method_address("GlobalSaveDataV1", "TryInitializingToDefaultsIfNewlyCreated")
    start, game_code = index.game_code()
    callers = {index.method_at(start + m.start()) for m in re.finditer(b'[' + bytes([0xE8, 0xE9]) + b']', game_code)
               if m.start() + 5 <= len(game_code)
               and start + m.start() + 5 + struct.unpack_from("<i", game_code, m.start() + 1)[0] == defaults}
    check({"SaveSystem::TryInitializingDefaultsOnGlobalSaveLoaded", "SaveSystem::OnFinishedLoadingStaticDependencies"} <= callers,
          "the save system itself sets previouslyInitializedToDefaults once the global save is loaded or created")

    instructions = decode(code, [0] + switch_targets(index.boot, coroutine))
    check(len(instructions) > 1000, f"the coroutine decodes ({len(instructions)} instructions)")
    check(all(point in instructions for point in (FIRST_STATE_JUMP, RESUME_JUMP, SKIP_BRANCH, NEXT_FRAME, ENDING, SAVE_LOOKUP,
                                                  SAVE_CHECK, SAVE_READY)),
          "every patch point is an instruction boundary")
    check(not branches_into(instructions, SKIP_BRANCH, NEXT_FRAME),
          "no branch from outside lands in the user skip the patch reuses")
    check(not branches_into(instructions, SAVE_CHECK, SAVE_READY),
          "no branch lands inside the save check the patch replaces")


def main():
    table = site_table()
    check(set(table) == {"CUSA57767-1.00", "CUSA57767-1.01"}, "the site table holds 1.00 and 1.01")
    for key, version in (("CUSA57767-1.00", "1.00"), ("CUSA57767-1.01", "1.01")):
        check_build(key, version, table)
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
