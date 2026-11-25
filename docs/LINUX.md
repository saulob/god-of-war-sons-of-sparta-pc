# Linux Build & AppImage Packaging (`x86_64`)

God of War: Sons of Sparta PC targets **Linux x86_64** with a minimum baseline of **Ubuntu 20.04 (`glibc 2.31`)** so the AppImage runs across Ubuntu 20.04+, modern desktop distributions, and SteamOS / Steam Deck.

> **Runtime status (v0.1.0):** Experimental. Linux builds, headless tests, ELF/glibc checks, and AppImage extraction checks are verified automatically; live graphical gameplay on Linux and Steam Deck requires manual runtime validation.

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
