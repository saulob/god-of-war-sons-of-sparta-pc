# God of War: Sons of Sparta (PS4) — Port Investigation

Findings from static inspection of the user's PS4 packages (`E:\Decomp_Downloads\Sons of Sparta\`).
No game code has been executed; everything here is from binary/metadata inspection.

## Package identification

| | Base | Update |
|---|---|---|
| File | `[SPSX]-God.of.War.Sons.of.Sparta-CUSA57767-USA-GAME-v1.00-7.00+.pkg` | `[SPSX]-God.of.War.Sons.of.Sparta-CUSA57767-USA-UPDATE-v1.01-7.00+.pkg` |
| Size | 5,592,252,416 bytes | 1,013,579,776 bytes |
| SHA-256 | `25ec0d6cb1260562a21238e3509817467f779e4620238ee615f0b08106d25bee` | `550c5f76a7aa90956ac5ac70ea410061ac1b9d9f07f182ed11f6cf7f02b3cdca` |
| Category | `gd` (base) | `gp` (patch) |
| Content ID | `UP9000-CUSA57767_00-SONSOFSPARTAPS40` | `UP9000-CUSA57767_00-SONSOFSPARTAPS40` |
| TITLE_ID | `CUSA57767` | `CUSA57767` |
| APP_VER | 01.00 | 01.01 |
| VERSION | 01.00 | 01.00 |
| Region | USA | USA |
| Package type | fake PKG (image key index 3, extractable with public keys) | fake PKG |

`SYSTEM_VER` in both `param.sfo` files is `0x12508000` (firmware 12.50). `TITLE_ID` `CUSA57767` was
verified from `param.sfo`, not assumed. Both packages are fake-signed and extractable with the
public keys in `scripts/pkg_keys.py`.

## Engine identification

**Unity 2022.3.62f3 (LTS), IL2CPP, not Mono.** Confirmed by:

- `eboot.bin` exports from library `PS4Player_IL2CPP` and imports 26 functions from `Il2cppUserAssemblies`.
- `Media/Metadata/global-metadata.dat` — IL2CPP metadata, sanity `0xAF1BB1FA`, metadata version 31.
- `Media/Modules/Il2cppUserAssemblies.prx` (135 MB) — the compiled C# game code (320 exports, 542,708 relocations).
- `Media/ScriptingAssemblies.json` — 62 UnityEngine modules + ~90 user/plugin assemblies (no Mono DLLs).
- `Media/RuntimeInitializeOnLoads.json` — 729 IL2CPP RuntimeInitializeOnLoad entries.

Unity data lives under `Media/` (not `Data/`). Rendering is **URP** (`Unity.RenderPipelines.Universal.*`
assemblies) with heavy 2D usage (`Unity.2D.*`, `AllIn1SpriteShaderAssembly`, `Pixelation.Runtime`,
`Elringus.SpriteDicing`). Audio is **FMOD Core + Studio** plus **Resonance Audio**. **Unity Burst**
(`lib_burst_generated.prx`) is present. Input uses the new Input System with a PS4 backend
(`Unity.InputSystem.PS4.dll`). No video middleware (no FMV files; cinematics are Timeline/Playables).

`Media/boot.config` enables `gfx-enable-native-gfx-jobs=1` (Unity native graphics jobs) and disables HDR.

### Executables and modules (base 1.00, SHA-256)

| Module | Size | SHA-256 |
|---|---|---|
| `eboot.bin` | 31,032,166 | `19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31` |
| `Media/Modules/Il2cppUserAssemblies.prx` | 135,851,802 | `e9407a892d5cd85d87d6699bb6dfed24e68a696a59e919d8bc0fe69ad6de5cec` |
| `Media/Modules/PS4Util.prx` | 6,060 | `18d501be8c358e4f1f62a1a14ce7983254e80dbfd51530189a9e97678cb6ef2f` |
| `Media/Metadata/global-metadata.dat` | 19,383,176 | `51d70419e9b396de31c8032f7ee199b04c418ebf1a104b5998663733f15cdbeb` |
| `Media/Plugins/libfmod.prx` | 1,693,596 | `217d997387c54dfe007b226e73e6e793d0382264479e82cc594358468ca9a161` |
| `Media/Plugins/libfmodstudio.prx` | 1,693,493 | `34d4d7a9a740495991b4d057c89be57ceecf268f13d95bb55cbb182834ec93c0` |
| `Media/Plugins/libresonanceaudio.prx` | 680,033 | `6114ef4b3f416a5872ab44c478bf8e4acce22a48b42b8ce247dc030b9f68784c` |
| `Media/Plugins/lib_burst_generated.prx` | 80,773 | `188898d73d74c0b3c8477a9a9443c42fffb36fcc1127135d5aebd7c17a5c49d3` |
| `Media/Plugins/CommonDialog.prx` | 37,430 | `75ced7c7af8a23f6fb0bc0f941235e345df68d889f4e31900ee36dbf8c43d967` |
| `Media/Plugins/SaveData.prx` | 31,807 | `e9a91bfc0a82ba7088a115e0a8541eee7cc9f7e6a9ecd8b7e0233c78cb15672d` |
| `Media/Plugins/UnityNpToolkit2.prx` | 206,145 | `e9a6d0ab232d43bac0d3ab1bc3e0ff8a6cf1ebb1096a42b729f145988b40bbc0` |

The update (1.01) replaces `eboot.bin` (`aafe8a2ab8e55e0044fe371248749026c494a07d6864fbb6a7fa89fe39999d58`),
`global-metadata.dat`, `Il2cppUserAssemblies.prx`, `globalgamemanagers` and much of the data tree; it also
adds `sce_discmap_patch.plt`. `sce_module/*.prx` are the standard Unity PS4 set (13 modules), loaded on demand.

## Import inventory summary

3,308 import sites across 67 PS4 libraries. The `eboot.bin` alone imports 851 functions from 39 libraries
(the full Unity PS4 player surface). `Il2cppUserAssemblies.prx` imports only 381 functions from 11 libraries
(`libc`, `libkernel`, `libScePosix`, `libSceFios2`, `libSceNet`, `libSceNetCtl`, `libSceRtc`,
`libSceSystemService`, `libSceSysmodule`, `libSceAppContent`, `PS4Util`).

Boot-critical libraries (eboot + game module + plugins + `libc` + `libSceFios2`): `libc`, `libkernel`,
`libSceGnmDriver`, `libScePosix`, `libSceFios2`, `libSceLibcInternal`, `libSceSysmodule`, `libScePad`,
`libSceVideoOut`, `libSceAudioOut`, `libSceAudioIn`, `libSceAudio3d`, `libSceAjm`, `libSceSaveData`,
`libSceSaveDataDialog`, `libSceSystemService`, `libSceUserService`, `libSceAppContent`, `libSceCommonDialog`,
`libSceMsgDialog`, `libSceImeDialog`, `libSceAvPlayer`, plus the dialog/network/VR families
(`libSceWebBrowserDialog`, `libSceSigninDialog`, `libSceErrorDialog`, `libSceHttp`, `libSceSsl`, `libSceNet`,
`libSceNetCtl`, `libSceNpManager`, `libSceNpWebApi`, `libSceNpToolkit2`, `libSceHmd`, `libSceVrTracker`,
`libSceCamera`, `libSceMove`, `libSceRemoteplay`, `libSceSocialScreen`).

Unity/PS4-specific system libraries (small, mostly stubbable): `libkernel_unity` (2 imports),
`libkernel_cpumode_platform` (`sceKernelIsProspero`), `libSceRazorCpu` (3), `ulobjmgr` (2),
`libSceGnmDriverResourceRegistration` (1), `libSceDiscMap` (2), `libSceCommonDialog` (`sceCommonDialogInitialize`),
`libSceLibcInternalExt` (`sceLibcHeapGetTraceInfo`).

The optional `sce_module/*.prx` (Face/Hand/Head tracking, NpToolkit2, S3DConversion, FontGsm, JobManager,
AudioLatencyEstimation) pull in the PSN trophy/matching stack and the camera/VR stack; these load only when
the game calls `sceSysmoduleLoadModule` and can be stubbed for offline bring-up.

## PS4 vs PS5 comparison (vs SoS-PS5)

| Component | PS4 (this build) | PS5 (SoS-PS5) | Reuse |
|---|---|---|---|
| Engine | Unity 2022.3 IL2CPP + Burst | Unity 2022.3 IL2CPP + Burst | same |
| Audio | FMOD 2.02.x Core+Studio, Resonance | FMOD 2.02.06 Core+Studio | same (FMOD ABI near-identical) |
| Save format | `GOWSOSSAVE999/GoW_SoS999.dat`, 12-byte header + UTF-32LE JSON | identical (confirmed in PS4 `global-metadata.dat`) | same |
| Addressables | `Media/StreamingAssets/aa/PS4/` | `Media/StreamingAssets/aa/PS5/` | similar (platform folder differs) |
| GPU API | GNM / GCN (libSceGnmDriver) | AGC / RDNA (libSceAgcDriver) | **different** |
| Input | PS4 libScePad (Unity Input System PS4) | PS5 libScePad | different |
| Save API | PS4 libSceSaveData | PS5 libSceSaveData (`sce_sdmemory`) | different |
| Title ID | CUSA57767 | PPSA28997 | different |

SoS-PS5's GPU/shader/AGC code and its PS5 system-library stubs must **not** be reused for PS4. Its FMOD
implementation, save-format knowledge, IL2CPP class layouts and the UnityFS-8 AssetBundle reader are reusable.
SoS-PS5 does **not** parse `global-metadata.dat` either — the game's own IL2CPP runtime (inside `eboot.bin`)
reads it. The port does not need to parse IL2CPP metadata to boot.

## Architecture decision

**Native relink (Option A) with shadPS4-derived compatibility libraries (Option D)** — the same architecture
as every existing Saulo PS4 port, because the game code is x86-64 and runs directly:

1. Process the plaintext SELF/ELF modules offline (`scripts/ps4_module.py` today, a boot-image builder like
   `launcher/boot_image.cpp` for the runtime).
2. Relocate and link `eboot.bin`, `Il2cppUserAssemblies.prx`, `PS4Util.prx`, the FMOD/Resonance/Burst plugins
   and `sce_module/*.prx` natively; resolve imports by `NID#library` against host implementations.
3. Provide the PS4 system libraries (`libc`/`libkernel`/`libScePosix`/`libSceFios2`/`libScePad`/`libSceAudioOut`/
   `libSceSaveData`/…) and a shadPS4-derived GNM→Vulkan video core for `libSceGnmDriver`/`libSceVideoOut`.

Static recompilation (Option B) has no advantage — the code is already native x86-64. The strongest existing
base is **Dragon's Crown Pro** (most mature loader/launcher/renderer lineage), with **dreams-pc** and
**catherine-full-body-pc** contributing the `ulobjmgr`/`libSceRazorCpu`/`libkernel_cpumode_platform` stubs and
the `libSceAudioOut`/`libSceGnmDriver`/Vulkan PS4 shim. SoS-PS5 contributes game-specific knowledge only.
