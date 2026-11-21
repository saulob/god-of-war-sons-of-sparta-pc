"""Process lifecycle invariants of the host: how the game ends, how the window starts, and how the cursor is driven.

    python scripts/window_lifecycle_check.py --area shutdown            exit paths and thread-local teardown
    python scripts/window_lifecycle_check.py --area display             startup display mode and F11
    python scripts/window_lifecycle_check.py --area cursor              cursor wiring in the game window
    python scripts/window_lifecycle_check.py --area AREA --self-test    prove each invariant fails on a mutated copy of the sources

All of it is shared host code: nothing here reads a build's eboot, so one run covers CUSA57767 1.00 and 1.01.

The process always ends with _Exit/_exit, never exit(): exit() would run static and thread-local destructors (the Vulkan
loader, the copy pool's joins, the presenter and the GPU thread) under the window, GPU, audio and guest threads that keep
running. A normal teardown therefore never runs, and no thread-local object may need one.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

LOADER = "src/loader.c"
DCPGPU = "gpu/shim/dcpgpu.cpp"
WINDOW = "gpu/shim/window.cpp"
WINDOW_HEADER = "gpu/shim/sdl_window.h"
CURSOR = "gpu/shim/dcp_cursor.h"
GNM = "gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp"
SWAPCHAIN = "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp"
PRESENTER = "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp"
COPY_POOL = "gpu/shim/dcp_copy_pool.cpp"
LAUNCHER_SETTINGS = "launcher/settings.cpp"
RUNTIME_SETTINGS = "gpu/shim/dcp_settings.cpp"

SCAN_ROOTS = ("src", "gpu/shim", "gpu/shadps4")
INPUT_ROOTS = ("launcher", "src", "gpu/shim")
CODE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}


def strip_code(text):
    """The text without comments, and with string and character literals emptied."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif text.startswith('R"', i) and (m := re.match(r'R"([^()\\ ]{0,16})\(', text[i:i + 24])):
            end = text.find(")" + m.group(1) + '"', i)
            i = n if end < 0 else end + len(m.group(1)) + 2
            out.append('""')
        elif text[i] == '"' or (text[i] == "'" and not (i > 0 and i + 1 < n and text[i - 1] in "0123456789abcdefABCDEF" and text[i + 1] in "0123456789abcdefABCDEF")):
            quote = text[i]
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            out.append(quote + quote)
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def read_tree(roots, suffixes=CODE_SUFFIXES):
    return {path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8", errors="replace")
            for root in roots for path in sorted((ROOT / root).rglob("*")) if path.is_file() and path.suffix in suffixes}


def function_body(text, signature):
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.index("{", start)
    depth = 0
    for at in range(brace, len(text)):
        depth += (text[at] == "{") - (text[at] == "}")
        if depth == 0:
            return text[brace:at + 1]
    return ""


def in_order(text, *needles):
    at = -1
    for needle in needles:
        at = text.find(needle, at + 1)
        if at < 0:
            return False
    return True


# ---------------------------------------------------------------- shutdown

EXIT_CALL = re.compile(r"(?<![\w.:>])(?:std::)?(?:exit|quick_exit|_cexit|ExitProcess)\s*\(")
THREAD_LOCAL = re.compile(r"\bthread_local\b")
RISKY_TYPE = re.compile(r"unique_ptr|shared_ptr|std::j?thread|mutex|condition_variable|function<|\bUnique|vk::(?!Vertex)|\bVk[A-Z]|"
                        r"SDL_|Scheduler|Backend|Presenter|Rasterizer|Loader|Swapchain|Instance|Device|Allocator")

# Every thread_local of the C++ host, by declaration: it is destroyed with its thread, so each must only free memory.
THREAD_LOCALS = {
    "gpu/shadps4/common/recursive_lock.cpp": ["thread_local std::vector<RecursiveLockState> g_recursive_locks"],
    "gpu/shadps4/common/thread.cpp": ["thread_local ThreadTimer thread_timer"],
    "gpu/shadps4/shader_recompiler/frontend/fetch_shader.cpp": [
        "thread_local const FetchShaderSnapshot* compiler_snapshot",
        "thread_local std::unordered_map<const u32*, CachedParse> cache"],
    "gpu/shadps4/shader_recompiler/info.h": [
        "static inline thread_local UdSnapshot ud_snapshots[MaxUdSnapshots]",
        "static inline thread_local u32 num_ud_snapshots"],
    "gpu/shadps4/video_core/renderer_opengl/gl_guest_watch.cpp": [
        "constinit thread_local u64 retried_page", "constinit thread_local u32 retry_count",
        "constinit thread_local std::chrono::steady_clock::rep retry_window_start"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_async_compiler.h": ["static inline thread_local unsigned worker_index"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_bind_helper.h": ["static inline thread_local bool on_helper"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h": ["static inline thread_local bool on_stage_b"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp": [
        "thread_local std::vector<AmdGpu::Image> images", "thread_local std::vector<u64> hashes",
        "thread_local std::vector<AmdGpu::Sampler> samplers", "thread_local std::vector<AmdGpu::Buffer> buffers",
        "thread_local std::vector<vk::VertexInputAttributeDescription2EXT> attributes",
        "thread_local std::vector<vk::VertexInputBindingDescription2EXT> bindings",
        "thread_local std::vector<AmdGpu::Buffer> buffers",
        "thread_local std::vector<PreparedVertexInputs::Range> ranges, merged",
        "thread_local std::vector<u8> range_index"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp": [
        "thread_local std::array<boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>, Shader::MaxStageTypes> rings"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h": [
        "static inline thread_local const AmdGpu::Regs* stage_regs",
        "static inline thread_local const AmdGpu::ComputeProgram* stage_cs"],
    "gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp": ["thread_local std::vector<u32> regions"],
    "gpu/shim/dcp_copy_pool.cpp": [
        "thread_local bool in_copy_thread", "thread_local const std::uint32_t tag", "thread_local Batch batch"],
    "gpu/shim/dcp_write_log.cpp": ["static thread_local const std::uint32_t tid"],
}


def thread_local_declarations(tree):
    found = {}
    for path, raw in tree.items():
        if Path(path).suffix == ".c" or "thread_local" not in raw:
            continue
        text = strip_code(raw)
        for m in THREAD_LOCAL.finditer(text):
            start = text.rfind(";", 0, m.start()) + 1
            start = max(start, text.rfind("{", 0, m.start()) + 1, text.rfind("}", 0, m.start()) + 1)
            prefix = text[start:m.start()].split()
            qualifiers = " ".join(w for w in prefix if w in ("static", "inline", "constinit", "extern"))
            end = min(i for i in (text.find(c, m.end()) for c in ";={") if i >= 0)
            declaration = " ".join(((qualifiers + " ") if qualifiers else "") .split() + ["thread_local"] + text[m.end():end].split())
            found.setdefault(path, []).append(declaration)
    return found


def main_after_gpu_init(loader):
    start = loader.find("dcpgpu_init(&gpu)")
    return strip_code(loader[start:]) if start >= 0 else None


def shutdown_checks(tree):
    results = []
    scanned = {path: text for path, text in tree.items() if path.split("/")[0] in ("src", "gpu")}
    exits = [path for path, raw in scanned.items()
             if re.search(r"\bexit\s*\(|quick_exit|_cexit|ExitProcess", raw) and EXIT_CALL.search(strip_code(raw))]
    results.append(("no host source ends the process with exit(), quick_exit() or ExitProcess(): the process ends with "
                    "_Exit/_exit, so no static or thread-local destructor runs under the live threads"
                    + (f" (found in {', '.join(exits)})" if exits else ""), not exits))

    loader = tree[LOADER]
    fail = function_body(loader, "static void fail(")
    results.append(("the loader's fail() ends the process through runtime_exit(), also after the video core is running",
                    "runtime_exit(1)" in fail and not EXIT_CALL.search(strip_code(fail))))
    tail = main_after_gpu_init(loader)
    results.append(("main() never returns once the video core is initialised (a return would run the C runtime's exit)",
                    tail is not None and not re.search(r"\breturn\b", tail) and "runtime_exit(1);" in tail))

    window = tree[DCPGPU]
    thread = function_body(window, "g_window_thread = std::thread(")
    results.append(("closing the window ends the process on the window thread: log finalised and flushed, then _Exit(0)",
                    in_order(thread, "while (window->PollEvents())", "runtime_log_mark_clean_exit();", "runtime_log_finalize();",
                             "runtime_log_flush();", "std::_Exit(0);")))
    destroys = [path for path, raw in scanned.items() if path.startswith("gpu/") and re.search(
        r"\bdelete\s+(?:g_)?window\b|(?:unique|shared)_ptr<\s*(?:Frontend::)?WindowSDL|make_(?:unique|shared)<\s*(?:Frontend::)?WindowSDL", raw)]
    results.append(("the game window is allocated once and never destroyed, so ~WindowSDL, SDL_DestroyWindow and the "
                    "SDL Vulkan loader unload never run before the process ends"
                    + (f" (found in {', '.join(destroys)})" if destroys else ""),
                    "new Frontend::WindowSDL(" in window and not destroys))

    gpu_teardown = re.compile(r"\bSDL_Quit\b|SDL_QuitSubSystem|SDL_UnloadObject|SDL_Vulkan_UnloadLibrary|\bFreeLibrary\b|\bpresenter\.reset\(|"
                              r"\bpresenter\s*=\s*nullptr|\bliverpool\.reset\(|\bliverpool\s*=\s*nullptr|d3d12|D3D12")
    found = sorted(path for path, raw in scanned.items() if gpu_teardown.search(strip_code(raw)) if re.search(
        r"SDL_Quit|SDL_QuitSubSystem|SDL_UnloadObject|SDL_Vulkan_UnloadLibrary|FreeLibrary|presenter|liverpool|d3d12|D3D12", raw))
    results.append(("no host code unloads a library, quits SDL, resets the presenter or the command processor, or touches the "
                    "D3D12 runtime: the NVIDIA nested-unload path of the PS5 port does not exist here"
                    + (f" (found in {', '.join(found)})" if found else ""), not found))

    declared = thread_local_declarations(tree)
    unknown = sorted(f"{path}: {d}" for path, items in declared.items() for d in items if d not in THREAD_LOCALS.get(path, []))
    missing = sorted(f"{path}: {d}" for path, items in THREAD_LOCALS.items() for d in items if d not in declared.get(path, []))
    results.append(("every thread_local of the C++ host is in the inventory (a new one must be classified here first)"
                    + (f" (unclassified: {'; '.join(unknown[:3])})" if unknown else "")
                    + (f" (gone from the sources: {'; '.join(missing[:3])})" if missing else ""), not unknown and not missing))
    risky = sorted(f"{path}: {d}" for path, items in declared.items() for d in items
                   if RISKY_TYPE.search(d.replace("thread_local", " ")))
    results.append(("no thread_local holds a thread, lock, smart pointer, callback, Vulkan or SDL object: each destructor "
                    "only frees memory or closes a timer handle" + (f" (risky: {'; '.join(risky[:3])})" if risky else ""), not risky))
    timer = function_body(tree["gpu/shadps4/common/thread.cpp"], "~ThreadTimer()")
    results.append(("the only thread_local with a destructor of its own, ThreadTimer, just closes its timer handle",
                    "Reset();" in timer and "join" not in timer and "lock" not in timer))

    pool = tree[COPY_POOL]
    pool_destructor = function_body(pool, "~Pool()")
    results.append(("the copy pool joins its threads in a static destructor, which is only safe because that destructor never runs",
                    "static Pool pool;" in pool and ".join()" in pool_destructor))
    results.append(("no exit watchdog, exit-time wait or launcher wait exists to be replaced: the launcher resumes the game "
                    "process and does not wait for it",
                    not re.search(r"WaitForSingleObject\([^)]*(?:hProcess|process)", strip_code(tree["launcher/game_process.cpp"]))
                    and "ResumeThread(g_game.hThread)" in tree["launcher/game_process.cpp"]))
    return results


# ---------------------------------------------------------------- display

INJECTION = re.compile(r"\b(SendInput|PostMessage[AW]?|SendMessage[AW]?|keybd_event|SDL_PushEvent|SDL_SendKeyboardKey|VK_F11|WM_KEYDOWN)\b")


def display_checks(tree):
    results = []
    window = tree[WINDOW]
    code = strip_code(window)
    constructor = code[code.find("WindowSDL::WindowSDL("):code.find("void WindowSDL::LogGamescopeWindow(")]
    apply = function_body(code, "void WindowSDL::ApplyDisplayMode(")
    poll = function_body(code, "bool WindowSDL::PollEvents(")
    results.append(("the window is created hidden, so nothing is ever shown in the wrong mode",
                    "SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true" in constructor))
    results.append(("the startup order is: create hidden, fit to the monitor, apply the display mode, show, read the drawable size",
                    in_order(constructor, "SDL_CreateWindowWithProperties(props)", "sos::FitWindowToArea(",
                             "ApplyDisplayMode(DcpSettings::Get().display_mode);", "SDL_ShowWindow(window);",
                             "SDL_GetWindowSizeInPixels(window, &w, &h);", "width = w;", "height = h;")))
    results.append(("Borderless is the desktop fullscreen mode of the window, requested once in ApplyDisplayMode and synced",
                    in_order(apply, "const bool borderless = mode == DcpGraphics::Borderless;", "SDL_SetWindowFullscreenMode(window, nullptr)",
                             "SDL_SetWindowFullscreen(window, borderless);", "SDL_SyncWindow(window);")
                    and code.count("SDL_SetWindowFullscreen(") == 1 and "SDL_WINDOW_FULLSCREEN" not in constructor + apply))
    callers = [m.start() for m in re.finditer(r"ApplyDisplayMode\(", code)]
    results.append(("the display mode is applied at startup before the window is shown and afterwards only when the setting changes",
                    len(callers) == 3 and "if (const int mode = DcpSettings::Get().display_mode; mode != display_mode) {\n        ApplyDisplayMode(mode);" in poll))
    results.append(("Windowed keeps a normal resizable window of the configured size and is never forced fullscreen at creation",
                    "SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true" in constructor
                    and "SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_width" in constructor
                    and "SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_height" in constructor
                    and "SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN" not in constructor))
    toggle = function_body(code, "void WindowSDL::ToggleBorderless(")
    results.append(("F11 stays a runtime key: a key press that is not a repeat flips Windowed and Borderless and saves the mode",
                    "event.key.scancode == DcpSettings::Get().window_mode_key" in poll and "window_shortcut && !event.key.repeat" in poll
                    and "mode = mode.load() == DcpGraphics::Windowed ? DcpGraphics::Borderless : DcpGraphics::Windowed;" in toggle
                    and "DcpSettings::SaveDisplayMode();" in toggle))
    injections = sorted(path for path, raw in tree.items() if path.split("/")[0] in INPUT_ROOTS and INJECTION.search(strip_code(raw)))
    results.append(("no startup key injection exists in the launcher, the loader or the window: no SendInput, PostMessage, "
                    "synthetic F11 or posted key event" + (f" (found in {', '.join(injections)})" if injections else ""), not injections))
    results.append(("the launcher and the runtime exchange the display mode only through the saved setting display_mode",
                    'kDisplayModeKey[] = "display_mode"' in tree["launcher/settings.h"] and 'key == "display_mode"' in tree[RUNTIME_SETTINGS]
                    and '{{"display_mode", DisplayModeKey()}}' in tree[RUNTIME_SETTINGS]
                    and "SOS_START_BORDERLESS" not in "".join(tree[p] for p in (LOADER, WINDOW, "launcher/launcher.cpp"))))
    gpu = strip_code(tree[DCPGPU])
    results.append(("the presenter, and with it the surface and the first swapchain, is created only after the window "
                    "constructor returned in the Borderless state",
                    in_order(gpu, "new Frontend::WindowSDL(", "g_window_ready = true;", "g_window_cv.wait(lock, [] { return g_window_ready; });",
                             "Libraries::GnmDriver::RegisterLib(&resolver);")
                    and "presenter = VideoCore::CreatePresenter(*g_window, liverpool.get());" in tree[GNM]))
    swapchain = strip_code(tree[SWAPCHAIN])
    presenter = strip_code(tree[PRESENTER])
    results.append(("the first swapchain takes its extent from the window's drawable size, and the presenter recreates it "
                    "only when that size or the surface changes, never for the startup mode",
                    "Create(window.GetWidth(), window.GetHeight());" in swapchain
                    and "window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()" in presenter
                    and not re.search(r"swapchain\.Recreate\([^)]*\)[^;]*;[^}]*(?:Borderless|display_mode)", presenter)
                    and "display_mode" not in swapchain + presenter))
    return results


# ---------------------------------------------------------------- cursor

def cursor_checks(tree):
    results = []
    window = strip_code(tree[WINDOW])
    header = strip_code(tree[CURSOR])
    update = function_body(window, "void WindowSDL::UpdateCursor(")
    poll = function_body(window, "bool WindowSDL::PollEvents(")
    results.append(("the cursor state is updated once per window poll with the real fullscreen and focus flags and a millisecond clock",
                    "(flags & SDL_WINDOW_INPUT_FOCUS)" in update and "(flags & SDL_WINDOW_FULLSCREEN)" in update
                    and "cursor.Update(true, focused, DcpOverlay::CapturesInput(), mouse_capture.Captured(), fullscreen,\n"
                        "                                           SDL_GetTicks())" in update
                    and poll.count("UpdateCursor();") == 1 and window.count("UpdateCursor();") == 1))
    results.append(("only mouse motion of the game window restarts the idle timer: the event's window id is compared with the game window's",
                    "cursor.MouseMoved(event.motion.windowID, SDL_GetWindowID(window), SDL_GetTicks());" in poll
                    and "cursor.MouseMoved()" not in window))
    results.append(("SDL_ShowCursor and SDL_HideCursor are called only when the state reports a change, plus the window's destructor",
                    window.count("SDL_HideCursor()") == 1 and window.count("SDL_ShowCursor()") == 2
                    and in_order(update, "if (const auto visible = cursor.Update(", "if (*visible) {", "SDL_ShowCursor();", "} else {", "SDL_HideCursor();")
                    and "SDL_ShowCursor();" in function_body(window, "WindowSDL::~WindowSDL(")))
    others = sorted(path for path, raw in tree.items() if path not in (WINDOW,) and re.search(r"SDL_(?:Show|Hide)Cursor\b", raw)
                    and path.split("/")[0] != "launcher")
    results.append(("no other host code shows or hides the cursor" + (f" (found in {', '.join(others)})" if others else ""), not others))
    launcher = [p for p, raw in tree.items() if p.startswith("launcher/") and re.search(r"SDL_HideCursor|dcp_cursor|DcpCursor", raw)]
    results.append(("the launcher never hides the cursor and does not use the game cursor state"
                    + (f" (found in {', '.join(launcher)})" if launcher else ""), not launcher))
    results.append(("the cursor state allocates nothing: no container, string, smart pointer or new in the header",
                    not re.search(r"std::(?:vector|string|map|unordered_map|unique_ptr|shared_ptr|function)|\bnew\b|\bmalloc\b", header)))
    results.append(("the idle delay is five seconds and applies to fullscreen only",
                    "kIdleHideMs = 5000;" in header and "if (!idle_started_ || !fullscreen || !focused || overlay) {" in header))
    return results


AREAS = {"shutdown": shutdown_checks, "display": display_checks, "cursor": cursor_checks}
TREE_ROOTS = {"shutdown": SCAN_ROOTS + ("launcher",), "display": INPUT_ROOTS + ("gpu/shadps4/core", "gpu/shadps4/video_core/renderer_vulkan"),
              "cursor": ("launcher", "src", "gpu/shim", "gpu/shadps4/video_core")}


def read_area(area):
    roots = TREE_ROOTS[area]
    tree = read_tree([r for r in roots if r != "launcher"])
    tree.update(read_tree(["launcher"], {".cpp", ".h"}))
    return tree


# ---------------------------------------------------------------- self test

def mutations(area):
    """(file, old, new, what), applied to one copy of the sources each."""
    if area == "shutdown":
        return [
            (LOADER, "runtime_exit(1); }", "exit(1); }", "fail() goes back to exit() with the video core running"),
            (LOADER, "    runtime_exit(1);\n}\n", "    return 1;\n}\n", "main() returns into the C runtime's exit"),
            (DCPGPU, "std::_Exit(0);", "std::exit(0);", "the window thread ends the process with exit()"),
            (DCPGPU, "runtime_log_flush();\n        sos_startup_phase(\"process exit\");", "sos_startup_phase(\"process exit\");", "the log is not flushed before _Exit"),
            (DCPGPU, "std::_Exit(0);", "delete window;\n        std::_Exit(0);", "the window is destroyed on the way out"),
            (COPY_POOL, "thread_local Batch batch;", "thread_local Batch batch;\nthread_local std::jthread backend;", "a thread-local joins a thread in its destructor"),
            (COPY_POOL, "thread_local bool in_copy_thread = false;", "thread_local bool in_copy_thread = false;\nthread_local std::unique_ptr<int> extra;", "an unclassified thread-local appears"),
            ("gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp", "    thread_local std::vector<u64> hashes;\n", "", "a classified thread-local disappears without the inventory following"),
            (WINDOW, "WindowSDL::~WindowSDL() {", "WindowSDL::~WindowSDL() {\n    SDL_Quit();", "the window destructor quits SDL"),
            (GNM, "presenter = VideoCore::CreatePresenter(*g_window, liverpool.get());", "presenter = VideoCore::CreatePresenter(*g_window, liverpool.get());\n    presenter.reset();", "the presenter is reset"),
            ("gpu/shim/dcp_gpu_list.cpp", "static std::unique_ptr<vk::detail::DynamicLoader> loader;", "static std::unique_ptr<vk::detail::DynamicLoader> loader;\n    FreeLibrary(nullptr);", "a library is unloaded"),
            ("gpu/shadps4/common/thread.cpp", "~ThreadTimer() {\n        Reset();", "~ThreadTimer() {\n        std::lock_guard guard(mutex);\n        Reset();", "the timer destructor takes a lock"),
        ]
    if area == "display":
        return [
            (WINDOW, "SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true", "SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, false", "the window is created visible"),
            (WINDOW, "    ApplyDisplayMode(DcpSettings::Get().display_mode);\n    SDL_ShowWindow(window);", "    SDL_ShowWindow(window);\n    ApplyDisplayMode(DcpSettings::Get().display_mode);", "the mode is applied after the window is shown"),
            (WINDOW, "    SDL_ShowWindow(window);\n", "    SDL_ShowWindow(window);\n    SDL_SetWindowFullscreen(window, true);\n", "a windowed start is followed by a synthetic toggle"),
            (WINDOW, "    SDL_ShowWindow(window);\n", "    SDL_ShowWindow(window);\n    ApplyDisplayMode(DcpGraphics::Borderless);\n", "a second mode application follows the first show"),
            (WINDOW, "SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true", "SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, false", "the window stops being resizable"),
            (WINDOW, "window_shortcut && !event.key.repeat", "window_shortcut", "F11 reacts to key repeat"),
            (WINDOW, "SDL_SyncWindow(window);", "", "the mode request is not synced"),
            ("launcher/launcher.cpp", "#include \"sos_launcher.h\"", "#include \"sos_launcher.h\"\nstatic void SyntheticF11(HWND w) { PostMessageW(w, WM_KEYDOWN, VK_F11, 0); }", "the launcher posts a synthetic F11"),
            (LOADER, "#include \"runtime.h\"", "#include \"runtime.h\"\nstatic void Press(void) { keybd_event(0x7A, 0, 0, 0); }", "the loader injects a key"),
            (DCPGPU, "g_window_cv.wait(lock, [] { return g_window_ready; });", "", "the presenter no longer waits for the finished window"),
            (SWAPCHAIN, "Create(window.GetWidth(), window.GetHeight());\n    std::printf", "Create(1280, 720);\n    std::printf", "the first swapchain ignores the window size"),
            (PRESENTER, "window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||\n        swapchain.NeedsRecreation()",
             "DcpSettings::Get().display_mode != 0 || swapchain.NeedsRecreation()", "the swapchain is recreated for the startup mode"),
            (RUNTIME_SETTINGS, 'key == "display_mode"', 'key == "display_style"', "the runtime stops reading the display mode setting"),
            (RUNTIME_SETTINGS, '{{"display_mode", DisplayModeKey()}}', '{{"display_style", DisplayModeKey()}}', "F11 stops saving the display mode"),
        ]
    return [
        (WINDOW, "fullscreen,\n                                           SDL_GetTicks())", "false,\n                                           SDL_GetTicks())", "the window reports windowed to the cursor"),
        (WINDOW, "cursor.MouseMoved(event.motion.windowID, SDL_GetWindowID(window), SDL_GetTicks());", "cursor.MouseMoved(0, 0, SDL_GetTicks());", "mouse motion is accepted from any window"),
        (WINDOW, "            SDL_HideCursor();\n", "            SDL_HideCursor();\n        }\n        SDL_HideCursor();\n        {\n", "the cursor is hidden without a state change"),
        (WINDOW, "const bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;", "const bool fullscreen = true;", "fullscreen is assumed"),
        (WINDOW, "    UpdateMouseCapture();\n    UpdateCursor();\n", "    UpdateMouseCapture();\n    UpdateCursor();\n    UpdateCursor();\n", "the cursor is updated twice per poll"),
        (WINDOW, "SDL_GetTicks())) {", "SDL_GetTicksNS())) {", "the clock is not in milliseconds"),
        ("launcher/host.cpp", "SDL_Quit();", "SDL_Quit();\n    SDL_HideCursor();", "the launcher hides the cursor"),
        (CURSOR, "kIdleHideMs = 5000;", "kIdleHideMs = 3000;", "the idle delay changes"),
        (CURSOR, "if (!idle_started_ || !fullscreen || !focused || overlay) {", "if (!idle_started_ || !focused || overlay) {", "the idle timer runs while windowed"),
        (CURSOR, "    std::uint64_t last_motion_ms_ = 0;", "    std::uint64_t last_motion_ms_ = 0;\n    std::vector<int> history_;", "the cursor state allocates"),
    ]


def self_test(area):
    base = read_area(area)
    check = AREAS[area]
    results = [("the real sources pass", all(ok for _t, ok in check(base)))]
    for path, old, new, what in mutations(area):
        copy = dict(base)
        if path not in copy or old not in copy[path]:
            results.append((f"mutation did not apply: {what}", False))
            continue
        copy[path] = copy[path].replace(old, new, 1)
        results.append((f"fails when {what}", not all(ok for _t, ok in check(copy))))
    ok = True
    for name, passed in results:
        print(f"  [{'ok  ' if passed else 'fail'}] {name}")
        ok = ok and passed
    print(f"Window lifecycle {area} gate self-test {'passed' if ok else 'FAILED'}: {len(results)} cases")
    return 0 if ok else 1


def main(argv):
    if "--area" not in argv or argv[argv.index("--area") + 1] not in AREAS:
        print(__doc__)
        return 2
    area = argv[argv.index("--area") + 1]
    if "--self-test" in argv:
        return self_test(area)
    results = AREAS[area](read_area(area))
    for text, ok in results:
        print(f"  [{'ok  ' if ok else 'fail'}] {text}")
    failed = sum(1 for _text, ok in results if not ok)
    print(f"Window lifecycle {area} FAILED: {failed} of {len(results)} invariants broken" if failed
          else f"Window lifecycle {area} passed: {len(results)} invariants, shared host code for CUSA57767 1.00 and 1.01")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
