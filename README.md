<div align="center">

# God of War: Sons of Sparta - PC Port

### The PS4 version of God of War: Sons of Sparta running natively on Windows, and experimentally on Linux and Steam Deck, with True Ultrawide, up to 4K, 240 fps, The Pit of Agonies DLC and its local co-op from the start, and the TRG launcher

[![Latest release](https://img.shields.io/github/v/release/saulob/god-of-war-sons-of-sparta-pc?style=for-the-badge&label=release&color=e0bb32&labelColor=07101f&include_prereleases&cacheSeconds=300)](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/saulob/god-of-war-sons-of-sparta-pc/total?style=for-the-badge&color=e0bb32&labelColor=07101f&cacheSeconds=300)](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental-00acc1?style=for-the-badge&labelColor=07101f)

<sub>The game's original PS4 x86-64 code runs directly on the PC, with replacement PS4 system libraries and a Vulkan renderer derived from <a href="https://github.com/shadps4-emu/shadPS4">shadPS4</a>. It is not an emulator. <b>The game itself is not included</b>: bring your own legally obtained God of War: Sons of Sparta PS4 package.</sub>

</div>

<img width="1918" height="811" alt="2026-11-25_18-02" src="https://github.com/user-attachments/assets/a2f109ac-be18-44a5-89b5-9ad51de1db1d" />

## Features

- **True Ultrawide Hor+**: 21:9 and 32:9 show more of the world at the sides with the same vertical view, without stretching the gameplay image. The HUD and menus keep their proportions
- **Up to 4K**: render at 720p, Native (1080p), 1440p, 1800p or 4K. Above 1080p the game runs in its PlayStation 4 Pro mode
- **High frame rates**: 30, 60 (Original, default), 90, 120, 144, 165, 240 or Unlimited frames per second, with drift-free frame pacing
- **The Pit of Agonies from the beginning**: the game's own challenge mode, reachable without finishing the story first. See [The Pit of Agonies](#the-pit-of-agonies)
- **Local two-player co-op in The Pit**: the original local co-op, with Kratos and Deimos on the same PC. The story campaign stays single-player
- **Unlock The Pit Early** and **Unlock Hermes Mode**: two independent launcher options, off by default, that open The Pit and the game's own Hermes Mode from the start. Your campaign save is never modified. See [Hermes Mode](#hermes-mode)
- **Skip Intro**: skips the startup logos and video and continues to the title screen
- **Controllers**: DualShock 4, DualSense, Xbox and other SDL gamepads. **Controller prompts: Automatic** shows Xbox symbols while an Xbox controller is used and PlayStation symbols otherwise, or can be forced to either; the Xbox A/B/X/Y, LB/RB and LT/RT glyphs appear in the game's menus and dialogs, including the Settings dialog, the campfire menu and The Pit
- **Graphics**: SMAA anti-aliasing, anisotropic texture filtering (Game, 2x, 4x, 8x, 16x), AMD FSR 1 upscaling and RCAS sharpening, VSync and graphics card selection
- **Display**: Windowed and Borderless Fullscreen (**F11** switches while playing), monitor selection, and the 16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3 and 5:4 shapes
- **Exit Game** in the title menu
- **Local PS4 trophies** (`NPWR58134_00`, 36 trophies), offline on this PC, shown on the launcher's Trophies page
- **SaveData** with atomic writes and backups
- **Bug Report page** in the launcher, with a scrubbed log preview, an optional crash report and **My reports**. See [Reporting problems](#reporting-problems)
- **Self-updater**: the launcher can install a newer release of the port; the installed game files and the `user` and `logs` folders are kept
- **Native builds**: Windows x64, and an experimental native Linux x86-64 AppImage that needs no Proton or Wine

## The Pit of Agonies

The Pit of Agonies is the game's original challenge mode and is part of both supported builds. The **Unlock The Pit Early**
option on the launcher's **Gameplay** page, off by default, shows the game's own title menu entry before the main story is
completed. The Pit keeps its original local two-player co-op for Kratos and Deimos. The option marks nothing as completed
and is independent of **Unlock Hermes Mode**; turning it off restores the original unlock requirement. The original cheat
code still works and unlocks both modes.

<img width="1918" height="813" alt="2026-11-25_18-06" src="https://github.com/user-attachments/assets/251dd2f8-5ff0-417a-ad4e-231f2e249efb" />

## Hermes Mode

Hermes Mode is the game's original speedrun mode. The **Unlock Hermes Mode** option on the launcher's **Gameplay** page,
off by default, makes it selectable immediately on the original GAME MODES screen alongside Campaign and Erebus Mode,
without completing the campaign or entering the secret code. Turning the option off restores the original unlock
requirement; the original cheat-code unlock and existing Hermes saves keep working. As in the original game, the selected
game mode is stored with the save and cannot be changed afterwards.

## What you need

- Windows 10 or 11, 64-bit
- **Or, experimental:** 64-bit Linux on x86-64, including the Steam Deck, with glibc 2.31 or newer (see [Linux and Steam Deck](#linux-and-steam-deck))
- A graphics card and driver with **Vulkan 1.3**
- An x86-64 processor with **AVX** and **POPCNT**: Intel Core 2nd generation (Sandy Bridge) or newer, or AMD FX or newer.
  **AVX2 is not required.** The game's PS4 code also uses MOVBE, BMI1, LZCNT, F16C and SSE4a; on a processor without
  them the port supplies them in software
- **Your own legally obtained God of War: Sons of Sparta PS4 package** of a supported build:

| Build | Package |
| --- | --- |
| `CUSA57767` USA **1.00** | The base game package |
| `CUSA57767` USA **1.01** | The official 1.01 update, installed over 1.00 |

Both builds have the same features. The PC port version (`0.1.0`) identifies the port, launcher, runtime and renderer; the
game build (`1.00` or `1.01`) identifies the original game content installed from your own `.pkg` files.

## Installing

1. Download **SonsOfSpartaPC-v&lt;version&gt;-windows-x64.zip** from the [latest release](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/latest) and extract it into a folder you can write to.
2. Run **sons_of_sparta.exe**. The launcher opens.
3. On the **Play** page click **Install game...** and select your own `CUSA57767` 1.00 `.pkg` file.
4. *(Optional)* Click **Install update...** and select your own official `CUSA57767` 1.01 update `.pkg`. **Remove Update...** returns to 1.00 at any time; your saves are kept.
5. Press **PLAY**. To bring the launcher back after turning **Show this launcher** off, hold **Shift** while starting or start with `--launcher`.

**The game itself is not included**: the release contains no game package or executable, and the port does not download the game.

## Screenshots

<img width="1918" height="816" alt="2026-11-25_18-05" src="https://github.com/user-attachments/assets/08df1988-706b-437a-bb17-773cad726463" />

<img width="1918" height="819" alt="2026-11-25_18-05_1" src="https://github.com/user-attachments/assets/9dfbd707-bdf2-45b6-8176-74500ece230d" />

### Linux and Steam Deck

**Experimental.** This is a native Linux build, not Proton or Wine, with the same launcher and options as Windows; the only
Windows-only option is **Allow overlay programs**. The AppImage is built on Ubuntu 20.04 and passes the automated Linux
tests and packaging checks, but it has not yet been runtime-tested on real Linux or Steam Deck hardware. Please report
Linux-specific problems with the log (see [Reporting problems](#reporting-problems)).

1. Download **SonsOfSpartaPC-v&lt;version&gt;-x86_64.AppImage** from the [latest release](https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/latest) and make it executable (right-click > **Properties** > **Permissions** > **Is executable**, or `chmod +x`).
2. Run it. The launcher opens.
3. Install your own game package from the **Play** page, exactly as on Windows, then press **PLAY**.

An AppImage normally mounts itself with FUSE. Where that is not possible, it extracts itself and runs from there
automatically. Requirements and details are in [docs/LINUX.md](docs/LINUX.md).

### Adding to Steam

1. In Steam, choose **Games > Add a Non-Steam Game to My Library...** and browse to `sons_of_sparta.exe` (Windows) or the
   AppImage (Linux, or Steam Deck Desktop Mode).
2. On Linux and Steam Deck keep the AppImage executable and in a writable folder (or set `SOS_PORTABLE_DIR`). Do **not**
   force Proton: it is a native Linux program.
3. Optional custom Steam artwork can be set by right-clicking the library entry. See [docs/STEAM-ARTWORK.md](docs/STEAM-ARTWORK.md).

### Windows SmartScreen

Windows may warn the first time you run the program, because the release is not digitally signed. If you downloaded it
from this repository's releases page, choose **More info**, then **Run anyway**. The SHA-256 and VirusTotal link of every
release file are in its release notes.

### Saves and settings

Everything is kept next to the program, so copying the whole folder to another PC takes your data with it:

| Folder | Contents |
| --- | --- |
| `user/sons_of_sparta.ini` | Launcher and game settings |
| `user/data/savedata/` | Your saves, written atomically with backup copies |
| `user/bug_reports.json` | The receipts of the Bug Reports you sent |
| `logs/` | One log per run |
| `game/` | The installed game, unpacked from your own package |

On Linux these folders sit next to the AppImage file when that folder is writable, and under
`$XDG_DATA_HOME/SonsOfSpartaPC` (default `~/.local/share/SonsOfSpartaPC`) otherwise. `SOS_PORTABLE_DIR` chooses
another folder.

## Reporting problems

The easiest way is the launcher's **Bug Report** page: describe the problem, choose the log of that session and press
**Send report**. The log is scrubbed before the preview and the report are built: user and computer names, private
folders, addresses and secrets become placeholders, while crash and GPU diagnostics stay. The preview shows exactly what
is sent. When a crash dump was written, **Attach crash report** adds it; clear the box to send the log alone.

Every report returns a **Report ID**. **My reports** on the same page shows its status and developer replies, and offers
the player test answer (`Yes, fixed` / `No, still happening`) when a fix awaits it. It needs no account. If the service
cannot be reached, the page says so and keeps your text. See [docs/BUG_REPORT.md](docs/BUG_REPORT.md) for the protocol
and privacy model.

You can also open an [issue](https://github.com/saulob/god-of-war-sons-of-sparta-pc/issues) and attach the newest log
from the `logs` folder.

## Known limitations

- **Linux and Steam Deck** are experimental: built and checked automatically, not yet runtime-tested on real hardware.
- **Processors without AVX2** (Intel Sandy Bridge and Ivy Bridge, AMD FX): the instructions the port supplies are
  checked instruction by instruction against the real game code, but the game has not yet been played on such a
  processor. MOVBE and BMI1 are emulated as they run there, which may cost performance.
- **Shapes other than 16:9** are labelled experimental in the launcher: the game was authored for 16:9, so some 2D scenes
  may show the edges of their original backgrounds. The boot splash video stays 16:9.
- **4K and the other resolutions above 1080p** need a powerful graphics card.
- **Vulkan 1.3 is required**: update the graphics driver if Vulkan device creation fails. On laptops with two graphics
  cards, choose the discrete one on the launcher's **Display** page.
- **Overlays**: third-party Vulkan overlays (RivaTuner, Bandicam, OBS hooks) can interfere with graphics start-up. Keep
  **Allow overlay programs** off unless you need it.

## Building it yourself

### Windows (MSYS2 CLANG64)

```powershell
.\tools\setup.ps1                # checks the prerequisites
.\tools\build.ps1                # incremental development build
.\tools\test.ps1                 # the complete headless test suite
.\tools\package.ps1              # Release build, dist\SonsOfSpartaPC-v<version>-windows-x64\ and its verified ZIP
.\tools\package-release.ps1      # the whole local release: test gate, Windows ZIP, Linux AppImage, notes
```

`tools\test.ps1` checks both supported builds from the sources and needs no game installed; a local boot image or game
folder only adds evidence, and its absence is reported as `DATA UNAVAILABLE`, never as a pass or a failure.

`tools\package-release.ps1` builds the Linux AppImage in an Ubuntu 20.04 WSL2 distribution (`-WslDistro <name>` picks it,
see [docs/LINUX.md](docs/LINUX.md)), fails if either platform fails, and writes every VirusTotal entry as PENDING. After the
four files have been scanned and their URLs recorded, `tools\package-release.ps1 -NotesOnly` regenerates the notes without
building or touching any package (see [docs/RELEASE_SECURITY.md](docs/RELEASE_SECURITY.md)). Nothing uploads, tags or
publishes anything. The release version is the project `VERSION` in `CMakeLists.txt`.

### Linux (Ubuntu 20.04, x86-64)

```sh
./tools/build.sh --release --test
./tools/package-linux.sh
```

See [docs/LINUX.md](docs/LINUX.md) for the full Linux build and AppImage details.

### Repository layout

| Path | Purpose |
| --- | --- |
| `src/` | Native loader, PS4 compatibility runtime, patches and trophy engine |
| `gpu/` | shadPS4-derived Vulkan video core, shader recompiler, GnmDriver, VideoOut, AvPlayer |
| `launcher/` | TRG launcher: install, settings, controls, trophies, Bug Report and updater |
| `tools/` | Build, headless test and local release packaging scripts (Windows and Linux) |
| `scripts/` | Build parity, import preflight, lifecycle and patch verification gates |
| `tests/` | Headless unit and mutation test suites |
| `server/bug-report/` | Standalone Cloudflare Worker for Bug Reports, with its admin dashboard and tests |
| `packaging/` | Application icons, Linux desktop and AppStream metadata, packaged README and licenses |
| `assets/launcher/` | Launcher art and icons |
| `docs/` | Platform, import and engineering documentation |

## Sponsoring

God of War: Sons of Sparta PC is maintained and developed in my free time. If you enjoy the project or find it useful, consider supporting its continued development.

<p align="center">
<a href="https://github.com/sponsors/saulob" target="_blank"><img src="https://img.shields.io/badge/GitHub%20Sponsors-Sponsor-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors"></a>
</p>

## Credits

- God of War and God of War: Sons of Sparta belong to their respective rights holders. This is an unofficial fan-made PC port. It is not affiliated with or endorsed by Sony Interactive Entertainment, Santa Monica Studio or any other rights holder, and it contains no game package or executable.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) for the Vulkan renderer, shader recompiler, GnmDriver, VideoOut, AvPlayer and kernel event queues the video core is built on (GPL-2.0-or-later; see `gpu/VENDOR.txt`)
- [gow3_pc](https://github.com/FelipePrado0/gow3_pc) and its lineage, MediEvil PC and uncharted-ndc-native, through which the video core came to this port
- The Dragon's Crown Pro PC port, the base of the loader, launcher and renderer, and the dreams-pc and catherine-full-body-pc ports for parts of the PS4 system library stubs (see [docs/INVESTIGATION.md](docs/INVESTIGATION.md))
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, licensed under the MIT License
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al., and AMD FidelityFX Super Resolution 1
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) by Alex Barney for ATRAC9 audio decoding
- The licenses of the libraries the release includes are in the `licenses` folder of the ZIP

## AI disclosure

AI tools were used to assist with code analysis, development, debugging and documentation. Changes were directed, tested and reviewed by the maintainer.

## License

Original code in this project is licensed under the GNU General Public License v2.0 or later (GPL-2.0-or-later). See [LICENSE](LICENSE).

Third-party components retain their respective licenses and notices. See [packaging/licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt), with the full component license texts in `packaging/licenses/components/`.

The game package and executable are not included. God of War: Sons of Sparta and its assets are not covered by this project's license.
