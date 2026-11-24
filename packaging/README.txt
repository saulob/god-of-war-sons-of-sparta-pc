God of War: Sons of Sparta — PC Port (v0.1.0)
==============================================

Native PC port of the PlayStation 4 release of God of War: Sons of Sparta
(CUSA57767, USA). The game's original x86-64 code runs natively on your PC
with replacement PS4 system libraries and a Vulkan renderer derived from
shadPS4.

NO GAME FILES ARE INCLUDED. Bring your own legally obtained PS4 package.

Supported Versions
------------------
- PC Port Version: 0.1.0
- Supported PS4 Game Builds:
    * CUSA57767 1.00 USA (base game)
    * CUSA57767 1.01 USA (official update 1.01)

Both 1.00 and 1.01 expose equivalent features.

Requirements
------------
- Windows x64 (Windows 10/11) or Linux x86_64 (glibc 2.31+ / Ubuntu 20.04+;
  Linux and Steam Deck gameplay are experimental in v0.1.0)
- Vulkan 1.3-capable GPU with current graphics drivers

Quick Start
-----------
1. Extract this archive to a writable folder (Windows) or mark the .AppImage
   executable with `chmod +x` (Linux).
2. Run `sons_of_sparta.exe` (Windows) or `SonsOfSpartaPC-v0.1.0-x86_64.AppImage`
   (Linux).
3. On the Play page, click "Install game..." and choose your CUSA57767 1.00 .pkg.
4. (Optional) Click "Install update..." and choose your official CUSA57767 1.01
   update .pkg. Use "Remove Update..." to revert to 1.00 without losing saves.
5. Configure Display, Graphics, Gameplay, and Controls, then click Play.
   Hold Shift on launch (or pass --launcher) to reopen the launcher later.

Key Features
------------
- True Ultrawide Hor+ (21:9 and 32:9) and configurable render resolutions
- Frame rate options: 30, 60, 90, 120, 144, 165, 240 FPS, or Unlimited
- SMAA anti-aliasing, anisotropic texture filtering (up to 16x), FSR 1
- Automatic / PlayStation / Xbox controller prompts and prepared Xbox icons
- Skip Intro, Exit Game in the title menu, and optional Unlock Hermes Mode
  (off by default; saves are never modified)
- Native SaveData with atomic writes and backups, plus full Trophy support

Save Data & Portable Files
--------------------------
- Windows: `user/`, `game/`, and `logs/` are created next to sons_of_sparta.exe.
- Linux AppImage: `user/`, `game/`, and `logs/` are created next to the AppImage
  file (or under $XDG_DATA_HOME/SonsOfSpartaPC if that directory is read-only).
  Set SOS_PORTABLE_DIR to override.
- Saves live under `user/data/savedata/`. Settings live in `user/sons_of_sparta.ini`.

Adding to Steam (Non-Steam Game)
--------------------------------
1. In Steam: Games -> Add a Non-Steam Game to My Library...
2. Select sons_of_sparta.exe (Windows) or the .AppImage (Linux / Steam Deck).
3. On Linux / Steam Deck, run natively (do NOT enable Proton compatibility).
4. Optional custom artwork can be assigned via right-click -> Manage in Steam.

Troubleshooting & Bug Reports
-----------------------------
- Update your GPU drivers if Vulkan initialization fails.
- Select your discrete GPU on the Display page on multi-GPU laptops.
- Keep "Allow overlay programs" off if third-party overlays cause instability.
- Report issues at https://github.com/saulob/god-of-war-sons-of-sparta-pc/issues
  with the relevant log from logs/.

Licenses & Source Code
----------------------
GPL-2.0-or-later (see LICENSE.txt, licenses/THIRD-PARTY-LICENSES.txt, and the full
component texts in licenses/components/).
Source code: https://github.com/saulob/god-of-war-sons-of-sparta-pc
