# God of War: Sons of Sparta PC v@VERSION@

A bugfix release of this unofficial PC port, on top of v0.1.0. It focuses on compatibility, game installation and crash diagnostics. The port runs the PlayStation 4 version of *God of War: Sons of Sparta* natively on Windows x64 and, experimentally, on Linux x86-64 and Steam Deck. It is a fan project, not affiliated with or endorsed by Sony Interactive Entertainment, Santa Monica Studio or any other rights holder

**No game files are included.** You need your own legally obtained *God of War: Sons of Sparta* PS4 package of a supported build: `CUSA57767` USA **1.00**, or `CUSA57767` USA **1.01** (the 1.01 update installed over 1.00). Install it from the launcher's **Play** page

Updating from v0.1.0 keeps your installed game files, saves and settings

## What's fixed in v0.1.1

### Fixed

- **Linux and Steam Deck startup crash**: every v0.1.0 Linux log we received, on Steam Deck and on Intel Linux systems, stopped silently while the game was starting. The game's garbage collector was told the wrong stack bounds for the main game thread and crashed while scanning it. The runtime now reports the stack the game really runs on. This affects the native Linux build and AppImage only; Windows is unchanged
- **Processors without AVX2**: AVX2 was never needed by the game, but v0.1.0 refused Intel processors without LZCNT (such as Sandy Bridge and Ivy Bridge) and had no fallback for F16C. The port now supplies LZCNT, TZCNT and F16C in software, alongside its existing MOVBE, BMI1 and SSE4a fallbacks. Processors with AVX and without AVX2 are supported; processors without AVX are still not supported
- **Game installation in long Windows folder paths**: installing the game package into a deeply nested folder could fail partway through (around 43%) once a file path went past the classic Windows 260-character limit. The installer now works with long paths, its staging and rollback leave the installed game untouched when an install fails or is cancelled, and its error messages name the step, file and system error. This change is in the installer
- **Intel GPUs**: Intel graphics on Linux, such as the Arc 140T (Core Ultra 7 255H), now use a renderer profile without fragment shader barycentrics, with corrected integer values on the driver interpolation path. The Intel Linux reports received for v0.1.0 ended at the same point as the Steam Deck crash above, so that fix also applies to them
- **SteamOS and Vulkan reliability**:
  - A lost Vulkan surface is rebuilt for the same window instead of failing on every recreation
  - When the preferred video memory is full, buffer memory and frame images fall back to another memory type instead of failing, which helps low-VRAM and shared-memory systems such as the Steam Deck
  - Drivers without sparse buffers get fully resident buffer arenas
  - The pipeline cache is kept per game build and written atomically, so a crash or a 1.00/1.01 switch cannot leave a damaged or mismatched cache
  - When the display refresh rate changes while playing (Steam Deck 60/90 Hz, another monitor), frame pacing follows it without a burst of catch-up frames
- **Steam Deck and Gamescope launcher and input**:
  - **Start** on a controller presses **PLAY** in the launcher from any page, and under Gamescope the launcher accepts controller buttons even without keyboard focus
  - In Gaming Mode the touchscreen acts as the DualShock 4 touchpad (two fingers, a tap clicks it). `SOS_TOUCH_GAMESCOPE=0` keeps Gamescope's own touch handling
  - A Steam-launched game on a Wayland desktop uses X11 so Steam Input applies the game's own controller layout

### Diagnostics improved

These changes do not fix a crash by themselves. They make the next log from an affected system much more useful

- **Linux crash reports**: a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) now writes a crash report to the log with the startup phase, the last completed step, the crashing thread, recent PS4 calls and GPU activity and a backtrace, instead of ending silently as v0.1.0 could. The log is flushed before a fatal exit, including GPU library assertions
- **Startup steps**: every log starts with a short system summary, and each step from PLAY to the first frame is logged with its duration and result, with a warning when a step takes unusually long
- **First GPU operations**: the first shader translation, pipeline creation, draw and swapchain acquire are logged, so a crash before the first frame shows which one was in progress
- **Intel/Vulkan startup crash diagnostics on Windows**: crashes inside a driver or other DLL now name the module, its offset and its file version, the log describes the rendering pass in progress (including unbound colour attachments), and renderer threads have names. This was prompted by an Intel UHD 630 crash report; **that crash is not confirmed fixed yet**
- **Installer diagnostics**: a failed install logs its stage, progress, file path lengths, the failing file operation and the system error
- **Bug Reports**: the launcher's preview and the report dashboard classify each session (launcher only, failed install, startup hang or crash, device loss, crash during play, normal exit and others), so reports reach the right investigation faster

## Highlights

The features of v0.1.0 are unchanged:

- **True Ultrawide Hor+**: 21:9 and 32:9 screens show more of the world at the sides with the same vertical view, without stretching the gameplay image. The HUD and menus keep their proportions
- **Up to 4K**: render at 720p, Native (1080p), 1440p, 1800p or 4K
- **High frame rates**: 30, 60 (the original), 90, **120**, 144, 165 or 240 FPS, or Unlimited, with drift-free frame pacing
- **The Pit of Agonies from the beginning**: play the game's own The Pit of Agonies without finishing the story first
- **Local two-player co-op in The Pit**: the original local co-op, with Kratos and Deimos on the same PC. The story campaign stays single-player
- **Unlock The Pit Early**: the optional launcher setting that opens The Pit of Agonies in the title menu from the start
- **Unlock Hermes Mode**: the optional launcher setting that offers the game's own Hermes Mode from the start, without finishing the campaign or entering the secret code
- **Skip Intro**: skip the startup logos and video and continue to the title screen
- **Xbox and PlayStation controllers**: DualShock 4, DualSense, Xbox and other SDL gamepads
- **Automatic controller prompts**: Xbox symbols while an Xbox controller is used and PlayStation symbols otherwise, or forced to either

Both unlock settings are optional and off by default. Your campaign save is never modified

## Also included

- **Display**: Windowed and Borderless Fullscreen, monitor and graphics card selection, and the standard 16:9, 16:10, 4:3 and 5:4 shapes
- **Graphics**: SMAA anti-aliasing, anisotropic texture filtering up to 16x, AMD FSR 1 upscaling and RCAS sharpening
- Local **Trophies** (`NPWR58134_00`), **SaveData** with atomic writes and backups, and **Exit Game** in the title menu
- **Bug Report** page in the launcher, sent only when you confirm
- Built-in launcher with a **self-updater** that keeps your installed game files, saves and settings
- Native **Windows x64** build and native **Linux x86-64 AppImage** (no Proton or Wine)

## Validation

- The complete headless release gate passed on Windows: 91 of 91 groups, `Failed: 0`. It now includes regression groups built from the v0.1.0 reports: the Linux main stack crash, the Steam Deck and Intel Arc Linux logs, the long-path installer failure, processors without AVX2 and the Intel UHD 630 crash diagnostics
- The Windows and Linux release preparation passed, including the Linux build and its tests on Ubuntu 20.04, the AppImage's glibc 2.31 baseline, its library dependencies and its pinned runtime
- The supported-build contract covers both `CUSA57767` 1.00 and `CUSA57767` 1.01, and the build-dependent checks also passed against the real executables of both builds
- Windows and Linux feature parity was validated on the final binaries
- These are automated, headless checks. Gameplay testing has been on Windows; the Linux fixes are based on real Linux and Steam Deck reports but have not yet been confirmed on those systems

## VirusTotal

@VIRUSTOTAL_STATUS@

| File | SHA-256 | VirusTotal |
| --- | --- | --- |
| `@WINDOWS_ASSET@` | `@WINDOWS_SHA256@` | @WINDOWS_VIRUSTOTAL@ |
| `@LINUX_ASSET@` | `@LINUX_SHA256@` | @LINUX_VIRUSTOTAL@ |
| `sons_of_sparta.exe` | `@WINDOWS_EXE_SHA256@` | @WINDOWS_EXE_VIRUSTOTAL@ |
| `trg-updater.exe` | `@UPDATER_EXE_SHA256@` | @UPDATER_EXE_VIRUSTOTAL@ |

## Known limitations

- Linux and Steam Deck remain experimental. v0.1.1 includes fixes based on reports from real Linux and Steam Deck systems, but runtime coverage is still more limited than on Windows. Bug Reports from Linux and Steam Deck players are very welcome
- Processors without AVX2 are covered by compatibility tests (Intel Sandy Bridge and Ivy Bridge, AMD FX), but the game has not yet been played on such a processor. MOVBE and BMI1 are emulated as they run there, which may cost performance
- The Intel UHD 630 startup crash on Windows is still under investigation; this release adds the diagnostics needed to find its cause
- Shapes other than 16:9 are labelled experimental in the launcher: some 2D scenes may show the edges of the original backgrounds
- 4K and the other render resolutions above 1080p use the game's PlayStation 4 Pro mode and need a powerful graphics card

## Downloads

### Windows 10 / 11 x64

[Download @WINDOWS_ASSET@](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v@VERSION@/@WINDOWS_ASSET@)

[Windows SHA-256 file](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v@VERSION@/@WINDOWS_ASSET@.sha256)

Extract the ZIP into a writable folder and run **sons_of_sparta.exe**

### Linux x86-64 / Steam Deck (experimental)

[Download @LINUX_ASSET@](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v@VERSION@/@LINUX_ASSET@)

[AppImage SHA-256 file](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v@VERSION@/@LINUX_ASSET@.sha256)

Make the AppImage executable and run it. It falls back to extract-and-run automatically where FUSE is not available

Full installation and configuration information is in the [README](https://github.com/saulob/god-of-war-sons-of-sparta-pc#readme)

## SHA-256

| File | SHA-256 |
| --- | --- |
| `@WINDOWS_ASSET@` | `@WINDOWS_SHA256@` |
| `@LINUX_ASSET@` | `@LINUX_SHA256@` |
| `sons_of_sparta.exe` | `@WINDOWS_EXE_SHA256@` |
| `trg-updater.exe` | `@UPDATER_EXE_SHA256@` |

The `.sha256` files attached to the release contain the ZIP and AppImage hashes in the form `<sha256> <file name>`

## Credits

The video core is derived from [shadPS4](https://github.com/shadps4-emu/shadPS4), and the loader, runtime and launcher grew from the earlier PS4 ports listed in the [README credits](https://github.com/saulob/god-of-war-sons-of-sparta-pc#credits). Thank you to everyone who sent Bug Reports and logs from v0.1.0: this release is built on them
