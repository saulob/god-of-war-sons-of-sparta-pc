# God of War: Sons of Sparta — PC Port (v0.1.0)

Native Windows x64 and Linux x86_64 PC port of the PlayStation 4 release of *God of War: Sons of Sparta*
(`CUSA57767`, USA): the game's original x86-64 code runs directly on the PC with replacement PS4 system
libraries and a Vulkan renderer derived from shadPS4.

**No game files are included.** You must supply your own legally obtained PS4 package (`.pkg`) and install it
through the launcher.

## Versions

- **PC port version:** `0.1.0` (single authoritative source: `CMakeLists.txt`)
- **Supported PS4 game builds:**
  - `CUSA57767` **1.00 USA** (base package)
  - `CUSA57767` **1.01 USA** (official update 1.01 installed on top of 1.00)

The PC port version (`0.1.0`) identifies the native port, launcher, runtime, and renderer. The PS4 game build
version (`1.00` or `1.01`) identifies the original game content installed from your `.pkg` files. Both builds
expose equivalent functionality.

## Platforms & Requirements

- **Windows x64:** Windows 10 / 11 (64-bit) with a Vulkan 1.3-capable GPU and up-to-date graphics drivers.
- **Linux x86_64 AppImage:** Built on Ubuntu 20.04 (`glibc 2.31+`) with automatic extract-and-run fallback when
  FUSE is unavailable. Requires a Vulkan 1.3-capable GPU and host Vulkan ICD drivers. See [docs/LINUX.md](docs/LINUX.md).
- **Steam Deck / Linux status:** Experimental. The Linux x86_64 AppImage is built and verified against the
  headless test gate and packaging checks; live Linux and Steam Deck gameplay validation is performed manually.

## Features

- **Display & True Ultrawide Hor+:** Configurable render resolutions, Windowed / Borderless display modes,
  and 16:9, 21:9, and 32:9 True Ultrawide Hor+. True Ultrawide expands the horizontal field of view while
  preserving vertical view and HUD proportions (boot splash video stays 16:9; because the original game was
  authored for 16:9, some 2D backgrounds may reveal their original edges).
- **Graphics:** SMAA anti-aliasing, configurable anisotropic texture filtering (Game, 2x, 4x, 8x, 16x),
  optional FSR 1 upscaling and sharpening, VSync, and GPU selection.
- **Frame Rate:** 30, 60, 90, 120, 144, 165, 240 FPS, or Unlimited, with drift-free frame pacing and optional
  frame counter / title-bar FPS and resolution display.
- **Gameplay:**
  - **Skip Intro**
  - **Exit Game** integrated into the title menu
  - **Unlock Hermes Mode** (off by default; see below)
- **Controllers:** DualShock 4, DualSense, Xbox, and generic SDL3 gamepads, Automatic / PlayStation / Xbox
  button prompts and prepared Xbox glyphs, plus optional `user/gamecontrollerdb.txt` overrides.
- **Save Data & Trophies:** Native save data with atomic commit and backup support, plus full trophy tracking
  (`NPWR58134_00`, 36 trophies) viewable in the launcher.
- **Launcher & Updater:** Built-in installer, settings, controls, trophies, a Bug Report page with a
  privacy scrubber, optional crash dump and My reports, and a release updater.

## Hermes Mode

Hermes Mode is the game's original speedrun mode. The PC port provides an **Unlock Hermes Mode** toggle on
the launcher's **Gameplay** page, **disabled by default**.

When enabled, Hermes Mode is selectable immediately on the original GAME MODES screen alongside Campaign and
Erebus Mode without completing the campaign or entering the secret code. Turning the option off restores the
original unlock requirement; the original cheat-code unlock and existing Hermes saves continue to work normally.
As in the original game, the selected game mode is stored with the save and cannot be changed afterward.

## Installing the Game & Official Update

1. Extract the Windows ZIP to a writable folder (or mark the Linux `.AppImage` executable with `chmod +x`).
2. Start `sons_of_sparta.exe` (Windows) or `SonsOfSpartaPC-v0.1.0-x86_64.AppImage` (Linux).
3. On the **Play** page, click **Install game...** and select your legally obtained `CUSA57767` 1.00 `.pkg`.
4. *(Optional)* Click **Install update...** and select your official `CUSA57767` 1.01 update `.pkg`. You can
   return to 1.00 at any time using **Remove Update...** without touching your saves.
5. Click **Play** to start the game. Hold `Shift` while starting (or pass `--launcher`) to reopen the launcher
   if you turned off **Show this launcher**.

## Save Data & User Files

The port is portable by default:

- **Windows:** `user/`, `game/`, and `logs/` sit next to `sons_of_sparta.exe`.
- **Linux AppImage:** `user/`, `game/`, and `logs/` sit in the folder containing the `.AppImage` file. If that
  folder is read-only, data is stored under `$XDG_DATA_HOME/SonsOfSpartaPC` (default `~/.local/share/SonsOfSpartaPC`).
  Set `SOS_PORTABLE_DIR` to override the location.
- **Saves:** Stored under `user/data/savedata/` (with atomic swaps and backup copies).
- **Settings:** Stored in `user/sons_of_sparta.ini`.

## Bug Reports

The launcher's **Bug Report** page sends one gameplay log, your description and, when the **Attach crash report**
box is ticked, the matching crash dump. The box is offered for a session with a valid paired dump; clear it to send
the log alone. The log is scrubbed before either the preview or the report is built: user and computer names,
private folders, addresses and secrets become placeholders, while crash and GPU diagnostics stay. The preview shows
exactly the package that is sent.

Every report returns a **Report ID** and a private receipt. The receipt stays in `user/bug_reports.json` and is
never shown in the report, the log or the dashboard. **My reports** on the same page checks the status, shows
developer replies, offers the player test answer (`Yes, fixed` / `No, still happening`) when a fix awaits it,
and can resend a crash dump that failed to upload. It needs no account and no online gameplay.

The service is the standalone Sons Cloudflare Worker at
`wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws`, with a SQLite Durable Object under
`server/bug-report`. A clean installation works without any endpoint configuration. `SOS_BUG_REPORT_URL` and
`user/bug_report_url.txt` remain available as development or local overrides. The private `/admin` dashboard
needs the dashboard token; players never do. See [docs/BUG_REPORT.md](docs/BUG_REPORT.md) for the protocol,
privacy model, deployment and overrides.

## Adding to Steam (Non-Steam Game)

Adding the port to Steam does not require bundling or moving original game files:

1. In Steam, choose **Games → Add a Non-Steam Game to My Library...** and browse to `sons_of_sparta.exe`
   (Windows) or `SonsOfSpartaPC-v0.1.0-x86_64.AppImage` (Linux / Steam Deck Desktop Mode).
2. On Linux / Steam Deck, ensure the AppImage is executable (`chmod +x`) and keep it in a writable folder (or
   set `SOS_PORTABLE_DIR`). Do **not** force Proton compatibility—this is a native Linux binary.
3. Optional custom Steam artwork (vertical cover `600x900`, header `920x430`, hero `3840x1240`, transparent
   logo, and icon) can be set in Steam by right-clicking the library entry (**Manage → Set custom artwork /
   Set custom background / Set custom logo**). See [docs/STEAM-ARTWORK.md](docs/STEAM-ARTWORK.md) for the
   asset list and the artwork still awaiting an approved source.

## Known Limitations & Troubleshooting

- **Vulkan 1.3 required:** Update your GPU drivers if Vulkan device creation fails. On laptops with multiple
  GPUs, select the discrete GPU on the launcher's **Display** page.
- **Overlays:** Third-party Vulkan overlays (e.g., RivaTuner, Bandicam, OBS hooks) can interfere with graphics
  initialization. Keep **Allow overlay programs** off unless needed.
- **Linux / Steam Deck:** Live runtime gameplay is experimental in v0.1.0. If FUSE is unavailable on your
  system, the AppImage automatically falls back to extract-and-run.
- **Reporting issues:** Use the launcher's **Bug Report** page (see [docs/BUG_REPORT.md](docs/BUG_REPORT.md)):
  choose the log of the session that failed, describe what happened and, if a crash dump was written, attach it.
  The log and preview are scrubbed in the launcher. A release build already points at the production service; if
  the service cannot be reached, the page says so, keeps your text and never claims the report was sent. You can
  also open the report ID later from **My reports**.

## Building from Source

### Windows (MSYS2 CLANG64)

```powershell
.\tools\setup.ps1                # check prerequisites
.\tools\build.ps1                # incremental RelWithDebInfo build
.\tools\test.ps1                 # complete headless validation suite
.\tools\package.ps1              # Release build + verified Windows ZIP in dist\
.\tools\package-release.ps1      # local Windows + Linux release build & verification (never publishes)
```

### Linux (Ubuntu 20.04 / x86_64)

```sh
./tools/build.sh --release --test
./tools/package-linux.sh
```

See [docs/LINUX.md](docs/LINUX.md) for full Linux build and AppImage details.

## Repository Layout

| Path | Purpose |
|---|---|
| `src/` | Native loader, PS4 compatibility runtime, patches, and trophy engine |
| `gpu/` | shadPS4-derived Vulkan video core, shader recompiler, GnmDriver, VideoOut, AvPlayer |
| `launcher/` | TRG launcher: Play/install, Display/Graphics/Gameplay/Controls/Trophies/About/Bug Report & updater |
| `tools/` | Build, headless test, and local release packaging scripts (Windows & Linux) |
| `scripts/` | Build parity, graphics/PRX import preflight, lifecycle, and patch verification gates |
| `tests/` | Headless unit and mutation test suites |
| `server/bug-report/` | Standalone Cloudflare Worker, SQLite Durable Object, admin dashboard and tests |
| `packaging/` | Application icons, Linux desktop/AppRun/AppStream metadata, packaged README, and licenses |
| `assets/launcher/` | Launcher icons and sponsor heart artwork (`icons/SOURCES.md`) |
| `docs/` | Platform, import, and engineering documentation |

## License

This port is licensed under the GNU General Public License v2.0 (see `LICENSE`). Third-party notices are
listed in `packaging/licenses/THIRD-PARTY-LICENSES.txt`, with the full component license texts in
`packaging/licenses/components/`.
