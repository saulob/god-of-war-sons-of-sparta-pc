# God of War: Sons of Sparta PC v@VERSION@

First release of this unofficial PC port. It runs the PlayStation 4 version of *God of War: Sons of Sparta* natively on Windows x64 and, experimentally, on Linux x86-64 and Steam Deck. It is a fan project, not affiliated with or endorsed by Sony Interactive Entertainment, Santa Monica Studio or any other rights holder

**No game files are included.** You need your own legally obtained *God of War: Sons of Sparta* PS4 package of a supported build: `CUSA57767` USA **1.00**, or `CUSA57767` USA **1.01** (the 1.01 update installed over 1.00). Install it from the launcher's **Play** page

## Highlights

- **True Ultrawide Hor+**: 21:9 and 32:9 screens show more of the world at the sides with the same vertical view, without stretching the gameplay image. The HUD and menus keep their proportions
- **Up to 4K**: render at 720p, Native (1080p), 1440p, 1800p or 4K
- **High frame rates**: 30, 60 (the original), 90, **120**, 144, 165 or 240 FPS, or Unlimited, with drift-free frame pacing
- **The Pit of Agonies from the beginning**: play the game's own The Pit of Agonies without finishing the story first
- **Local two-player co-op in The Pit**: the original local co-op, with Kratos and Deimos on the same PC. The story campaign stays single-player
- **Unlock The Pit Early**: the optional launcher setting that opens The Pit of Agonies in the title menu from the start
- **Unlock Hermes Mode**: the optional launcher setting that offers the game's own Hermes Mode from the start, without finishing the campaign or entering the secret code
- **Skip Intro**: skip the startup logos and video and continue to the title screen
- **Xbox and PlayStation controllers**: DualShock 4, DualSense, Xbox and other SDL gamepads
- **Automatic controller prompts**: Xbox symbols while an Xbox controller is used and PlayStation symbols otherwise, or forced to either. Xbox A/B/X/Y, LB/RB and LT/RT glyphs appear in the game's menus and dialogs, including the Settings dialog, the campfire menu and The Pit

Both unlock settings are optional and off by default. They change what the game offers when it starts; your campaign save is never modified

## Also included

- **Display**: Windowed and Borderless Fullscreen, monitor and graphics card selection, and the standard 16:9, 16:10, 4:3 and 5:4 shapes
- **Graphics**: SMAA anti-aliasing, anisotropic texture filtering up to 16x, AMD FSR 1 upscaling and RCAS sharpening
- **Exit Game** in the title menu
- Local **Trophies** (`NPWR58134_00`), offline on this PC
- **SaveData** with atomic writes and backups
- **Bug Report** page in the launcher, sent only when you confirm, and crash diagnostics in the logs
- Built-in launcher with a **self-updater** that keeps your installed game files, saves and settings
- Native **Windows x64** build and native **Linux x86-64 AppImage** (no Proton or Wine)

## Validation

- The complete headless release gate passed on Windows: 75 of 75 groups, `Failed: 0`
- The Windows and Linux release preparation passed, including the Linux build and its tests on Ubuntu 20.04, the AppImage's glibc 2.31 baseline, its library dependencies and its pinned runtime
- The supported-build contract covers both `CUSA57767` 1.00 and `CUSA57767` 1.01, and the build-dependent checks also passed against the real executables of both builds
- Windows and Linux feature parity was validated on the final binaries
- These are automated, headless checks. Gameplay testing so far has been on Windows; the Linux AppImage has not yet been run on real Linux or Steam Deck hardware

## VirusTotal

@VIRUSTOTAL_STATUS@

| File | SHA-256 | VirusTotal |
| --- | --- | --- |
| `@WINDOWS_ASSET@` | `@WINDOWS_SHA256@` | @WINDOWS_VIRUSTOTAL@ |
| `@LINUX_ASSET@` | `@LINUX_SHA256@` | @LINUX_VIRUSTOTAL@ |
| `sons_of_sparta.exe` | `@WINDOWS_EXE_SHA256@` | @WINDOWS_EXE_VIRUSTOTAL@ |
| `trg-updater.exe` | `@UPDATER_EXE_SHA256@` | @UPDATER_EXE_VIRUSTOTAL@ |

## Known limitations

- Linux and Steam Deck are experimental: the AppImage passes the automated checks, but it has not been runtime-tested on real hardware yet. Bug Reports from Linux and Steam Deck players are very welcome
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

The video core is derived from [shadPS4](https://github.com/shadps4-emu/shadPS4), and the loader, runtime and launcher grew from the earlier PS4 ports listed in the [README credits](https://github.com/saulob/god-of-war-sons-of-sparta-pc#credits). Thank you to everyone who sends Bug Reports and logs
