# Linux Build & AppImage Packaging (`x86_64`)

God of War: Sons of Sparta PC targets **Linux x86_64** with a minimum baseline of **Ubuntu 20.04 (`glibc 2.31`)** so the AppImage runs across Ubuntu 20.04+, modern desktop distributions, and SteamOS / Steam Deck.

> **Runtime status (v0.1.1):** Experimental. Linux builds, headless tests, ELF/glibc checks, and AppImage extraction checks are verified automatically. v0.1.1 includes fixes based on reports from real Linux and Steam Deck systems, but runtime coverage is still more limited than on Windows.

## Toolchain & Dependencies

- Compiler: Clang (C11 / C++23), CMake 3.24+, Ninja
- Libraries: SDL3, Vulkan loader/headers, VulkanMemoryAllocator, FreeType, libpng, zlib, OpenSSL (libssl/libcrypto), fmt, Boost headers, tsl::robin_map, Zydis, libxxhash, glslang
- Video decoder: LGPL-2.1-or-later FFmpeg 8.1.3, always built from `gpu/ffmpeg.txt` by `tools/build-ffmpeg.sh` into `cache/deps/ffmpeg` and never taken from `/usr/local` or the distribution — never a GPL-3.0 distribution build
- Packaging tools: `patchelf`, `readelf`, `objcopy`, `rsync`, pinned `appimagetool 1.9.1`, and the pinned static musl type-2 AppImage runtime (`patches/appimage-runtime/baseline.env`)

## Building & Headless Tests

From a Linux checkout (or inside an Ubuntu 20.04 WSL build container):

```sh
./tools/build.sh --release --test
```

`tools/build.sh` configures a non-PIE (`-fno-pie` / `-no-pie`) `Release` CMake + Ninja build under `build/cmake/linux-release` (keeping host heap and TLS below 1 TiB for guest compatibility), compiles `sons_of_sparta` and `dcpgpu_link_smoke`, and runs the Linux headless C/C++ and Python test suites (without opening any window, GPU renderer, or audio device).

## Packaging the AppImage

```sh
./tools/package-linux.sh
```

`tools/package-linux.sh`:

1. Verifies the build host and every shipped ELF binary require no glibc symbol newer than `GLIBC_2.31`.
2. Strips debug symbols from `sons_of_sparta` into `dist/SonsOfSpartaPC-v<ver>-x86_64-symbols/`.
3. Bundles non-host shared libraries (including LGPL FFmpeg, SDL3, fmt, xxhash, Zydis, FreeType, OpenSSL) into `usr/lib/` while excluding host GPU drivers, Vulkan layers, glibc, and X11/Wayland system libraries. It stages the full component license texts under `licenses/components/` and each bundled library's package or source license under `licenses/system/`.
4. Places `libstdc++.so.6` and `libgcc_s.so.1` in `usr/lib/compat/` so `AppRun` loads them only when the host libraries are older than the build's required `GLIBCXX`/`GCC` symbol versions.
5. Removes build `RUNPATH` entries and sets `DT_RPATH` to `$ORIGIN/../lib` (`$ORIGIN` on libraries).
6. Scans the AppDir for forbidden files, game binaries, local machine paths, and debug markers.
7. Packages with pinned `appimagetool 1.9.1` and the pinned static musl type-2 runtime (which automatically falls back to extract-and-run when FUSE is unavailable).
8. Re-extracts the AppImage (`--appimage-extract`) headlessly, verifies every file matches AppDir, and generates `dist/SonsOfSpartaPC-v<ver>-x86_64.AppImage.sha256`.

## WSL Orchestration from Windows

`tools/package-release-linux.sh` mirrors the Windows working tree into `~/.cache/god-of-war-sons-of-sparta-pc-build` (excluding `.git`, `game`, `user`, `cache`, `build`, `dist`) via `rsync --checksum --no-times --delete`, runs `tools/build.sh --release --test` and `tools/package-linux.sh`, copies the `.AppImage`, `.AppImage.sha256`, and `-symbols/` back to `dist/`, and verifies `sha256sum -c`.

## Portable Data & Steam Deck

The AppImage mount itself is read-only. `sons_of_sparta` stores `game/`, `user/`, and `logs/` in the directory containing the `.AppImage` file. If that directory is not writable, it falls back to `$XDG_DATA_HOME/SonsOfSpartaPC` (`~/.local/share/SonsOfSpartaPC`). Set `SOS_PORTABLE_DIR` to choose a custom folder.

On Steam Deck, add the `.AppImage` as a non-Steam game in Desktop Mode and run it natively without forcing Proton.

In the launcher, **Start** on a controller (the Deck's ☰ button) presses PLAY from any page, as do Enter and a tap or
click on PLAY. Under Gamescope the launcher accepts controller buttons even when its window does not hold keyboard
focus. A Steam-launched game on a Wayland desktop asks SDL for X11 first, so Steam Input applies the game's own
layout; Gaming Mode (Gamescope) is left as it is.

**Touchscreen in Gaming Mode** (SteamOS, Steam Deck and other Gamescope handhelds): Gamescope turns a touch into a
mouse pointer and a left click unless its touch click mode is Passthrough, and it keeps one mode for the whole
compositor: the newest `STEAM_TOUCH_CLICK_MODE` write on any of its Xwayland roots wins, and Steam writes its own mode
on its root again. While the game window has focus the game therefore asks for Passthrough through libX11 (loaded at
run time) on its own display and on every other local Gamescope root, and rewrites it about once a second. Each root's
previous value is given back on focus loss and on every way the game ends (closing the window, Exit Game, the game's
own exit); a value Steam wrote in the meantime is left alone. A crash ends the process without that step, and Steam's
next write gives the mode back. The screen is then the DualShock 4 touch surface: up to two fingers are two touchpad
contacts with their own ids, mapped where they land on the game picture (the letterbox bars are outside it), and a
short single-finger tap also clicks the touchpad (the game's Map). A touch never also reaches the game as a mouse:
SDL's mouse copy of it is dropped for the game (the F1 menu still takes taps). The launcher never asks for
Passthrough, so its taps stay mouse clicks; Windows and desktop Linux sessions are unchanged.
`SOS_TOUCH_GAMESCOPE=0` keeps Gamescope's own touch handling. The log shows `Touchscreen telemetry:
event=touch_route_ready video=<x11|wayland> gamescope=<yes|no> routed=<yes|no> direct_devices=<n>`, then
`gamescope_touch_claim roots=<n> compositor=<yes|no>` (or `gamescope_touch_claim_failed reason=<x11|display>`), the
bounded `gamescope_touch_reassert` and `gamescope_touch_release`, and per touch `touchscreen_detected`,
`touchscreen_touchpad_used` and `touchscreen_synthetic_mouse_suppressed`.

**Display refresh changes**: when the display refresh changes while the game runs (the Steam Deck switched between 60
and 90 Hz, the window moved to another monitor) and stays changed for half a second, the guest vblank behind the Frame
rate setting follows it from the next vblank, without a burst of catch-up frames, and the log shows `VideoOut: display
refresh changed A -> B Hz, guest vblank X -> Y Hz`. The game's speed does not change: it times itself with the system
clock and steps its physics at 1/60 s; only the number of new frames a second follows the display.

## Startup diagnostics

Every log starts with `System:` lines (port version, kernel, distribution, SteamOS, Gamescope, Gaming or Desktop Mode,
display session, memory, overlays and Vulkan layers by name only). Each step from PLAY to the first frame logs a
flushed `Stage:` line with its duration and result: the input that pressed PLAY, the game files recheck, the graphics
preflight, the settings save, the launcher teardown, the runtime handoff, Vulkan instance, GPU selection, device,
swapchain, module registration, guest entry and first present. A stage that runs past its budget is reported by a
`Stage watchdog:` line at 1x, 2x, 4x and 8x the budget; the watchdog never stops the game. Launcher input events and
focus changes are logged a few times each (`Launcher input:`).

A fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) writes one `CRASH: signal` report with the startup phase,
the last completed stage, the faulting thread (`Crash: thread <tid> (<name>)`, a guest thread by its guest name), the
first guest GPU operations and which one was in flight (`Crash: first GPU operations: ...`), the last guest memory
operations and PS4 calls, the GPU trace (last GPU events, unfinished submissions and their pipelines) and a host
backtrace, then the process ends by that same signal: the system's core dump policy (`ulimit -c`, `core_pattern`,
systemd-coredump) and the exit status Steam sees are unchanged. SIGTERM and SIGINT (Steam's Exit game) stay a normal
quit; SIGHUP is logged before it ends the process. A GPU library assertion (exit status 23) drains the log before the
process ends, so its message and trace reach the file.

Between the renderer's own first present and the game's first frame (`Runtime: first present`), the first guest shader
translation, SPIR-V module (with its interpolation route), graphics and compute pipeline (with every `VkResult` and
whether the pipeline binary cache is used), draw, dispatch and swapchain acquire each log one `Runtime: first <operation>
started: ...` and one `... finished: ...` line. `Window: surface ...` lines give the window's size in pixels (what the
swapchain follows), its logical size, pixel density and display scale (Wayland fractional scaling, HiDPI).
