# libSceAvPlayer

The game (CUSA57767 1.00 and 1.01, identical imports) uses 23 libSceAvPlayer functions, listed with call sites in
[avplayer-imports.md](avplayer-imports.md). They are served by shadPS4's FFmpeg player (`gpu/shadps4/core/libraries/avplayer`),
registered next to VideoOut in `gpu/shim/dcpgpu.cpp`. The game imports no libSceVideodec2 function.

## System modules

| ID | Library | Role here |
|---|---|---|
| 0xa5 | libSceAvPlayer | the player; loading it only counts a reference, the functions are always bound |
| 0xcf | libSceVideodec2 | loaded together with 0xa5 by the game; nothing imports it, decoding is FFmpeg's |

## Call chain of the first import

`sceAvPlayerSetLogCallback` is called at image+0x148685a inside the function at image+0x1485bf0, which has no direct
caller (it runs through a vtable on a Unity job worker). The function loads modules 0xa5 and 0xcf (0x1485d91, 0x1485df0),
optionally initialises net/ssl/http for HTTP sources, then calls, in order: `SetLogCallback` (callback 0x148a620, user data
the player object), `InitEx` (0x1486963), `PostInit` (0x1486acb), `SetAvSyncMode` (0x1486be9) and, further on, `Start`
(0x1486f41). `AddSourceEx` (0x1488d8d), `SetLooping` (0x1488dd0), `StreamCount`, `GetStreamInfo`, `EnableStream` and
`GetVideoDataEx` (0x1488537) are in the sibling function at 0x1487760. With the default configuration the game passes its
own file callbacks (open, close, read at offset, size), which run on the player's worker threads.

## Lineage and triage

Three implementations were compared: shadPS4 (`a808e762`, plus the later upstream changes to these files: the exclusive lock in
`Start`, the `FindStreams` result, the `CopyNV12Data` size check; all present here), the Alienation port and the Driveclub port.

| Difference | Class | Decision |
|---|---|---|
| Newer upstream commits touching avplayer, video_utils, avdec | UPSTREAM_NEWER | none exist |
| Worker threads attach a guest TCB (`avplayer_thread.h`) | ALIENATION_RECOMP_REQUIRED | ported |
| Self-join of decoder and demuxer threads cannot block | ALIENATION_RECOMP_REQUIRED | ported, tested |
| Media opens through `runtime_file_translate`, no handle streamer | ALIENATION_RECOMP_REQUIRED | ported |
| Start, Pause, Resume and Stop emit their events after releasing the source lock | ALIENATION_RECOMP_REQUIRED | ported |
| Decoder waits also wake at end of file | ALIENATION_RECOMP_REQUIRED | ported |
| Stop takes the source lock exclusively; state races after Stop guarded | ALIENATION_RECOMP_REQUIRED | ported |
| Stop catches C++ exceptions (Driveclub has the same) | DRIVECLUB_FIX | ported |
| `~AvPlayer` and `~AvPlayerState` stop the controller thread before the state or source goes away | DRIVECLUB_FIX | ported |
| `GetStreamInfo` tolerates a missing state | DRIVECLUB_FIX | ported (all calls guarded) |
| `GuestCall` wrapper around the game's callbacks | GAME_SPECIFIC | the guest TCB of the worker thread covers it |
| Intro pacing (`BbIntroPacing`) | GAME_SPECIFIC | not ported |
| `BlankVideo`, `FinishEarly`, boot diagnostics, crash context (skip-intro) | GAME_SPECIFIC | not ported |
| Handle streamer for non-host file backends | NOT_APPLICABLE | the runtime maps every guest path to a host file |

Changes made here beyond the three: handles are validated against a registry, so a closed or unknown handle is an error
instead of a use after free; `Close` from the event callback hands the deletion to the controller thread; `SetLogCallback` keeps the
callback; the demuxer routes packets by the FFmpeg index of the enabled stream; queue sizes are read under their lock;
the file streamer's path is copied before it reaches the game's open callback.

## Stubs the game imports

`sceAvPlayerJumpToTime` returns success for any valid handle and performs no seek. The game's seek routine (image+0x1484e50, which
`Prepare` calls right after `Start` with time 0) pauses the player, waits for the Pause event, arms `JUMP_COMPLETE`
(0x806A00A3, expected code at object+0x690), calls `JumpToTime`, polls `sceAvPlayerGetVideoDataEx` for up to ten seconds until a
frame with a different timestamp arrives, waits for the event (semaphore at +0x698, signed waiter counter at +0x6a0; a signal
that comes first stays in the counter) and then calls `Resume`. So `JumpToTime` delivers the warning through the game's event
callback before it returns, and a paused player resumes, which is what produces the new frame; the `Resume` that follows
succeeds once without a state change. `Start` waits for the Play event and `AddSource` for the Ready event the same way.
The time is in milliseconds (the second call, image+0x14857b8, passes the position minus two seconds). A worker stopped on
purpose (Stop, Close, restart) is not reported as the end of the media.

`sceAvPlayerChangeStream` and `sceAvPlayerSetTrickSpeed` return success without effect, as upstream does. The log callback
is stored and never invoked (its `va_list` cannot be built for guest code).

## End of file and state races

The game checks the result of `sceAvPlayerPause` and `sceAvPlayerResume` (image+0x14850a9 and +0x1485281: a non-zero result is
logged and the player is treated as failed), so neither may fail at the end of a clip. The decoders can reach the end of the
file between a state check and the transition that follows, so `Pause`, `Resume` and `Start` accept
`EndOfFile` as the outcome instead of logging `EndOfFile -> Pause`. The controller thread changes `Play` and `Buffering` only
from the state it inspected, so it cannot overwrite a newer `Pause` or `Stop` from the game thread. `Stop` after the end of the
file succeeds (the demuxer has released its own thread by then) and sends no second `StateStop` when the end-of-file one went
out. At the end of the demuxed file each decoder is flushed (`avcodec_send_packet(nullptr)`) so the frames it holds back are
delivered; `StateStop` follows only once every queued frame was consumed.

## Skip Intro

The player is not involved: every source plays completely (`tools/avplayer_tests.cpp`). Skip Intro patches the game's
own code in memory (`src/sos_skip_intro.c`, only for a build whose code matches the fingerprints, all or nothing).

`UiScripts.SplashScreen.Start` is a coroutine (`<Start>d__24.MoveNext`): presentation, startup video, then an ending that
marks the game as booted, saves the global data, loads the FMOD banks and the next scenes. The patch sends the first state to the ending's save system lookup, ahead of the ending's log lines and flags. The save check
is replaced by a wait: until `SaveSystem.isInitialized` and `currentGlobalData.previouslyInitializedToDefaults` hold, the
coroutine yields a frame (state 6, whose resume is redirected to the same lookup), so only the lookup repeats. When the
save system is ready and the splash screen's `isSplashScreenVisualSequenceFinished` is still false, the ending runs once and
its own pass of the check continues to the booted mark, the save and the scene loads. A first boot and a returning boot take
the same path. No input is injected and the booted flag is never written by the port.

`SaveSystem.InitSaveSystem` awaits `Task.Delay(10000)` before `LoadAll`, which loads the global data and every slot and then
sets `isInitialized`; the presentation used to cover that wait. With Skip Intro on, the argument becomes 1 ms: `Initialize`
stores the returned task in `awaitInit` after the call and the coroutine clears it when done, so the await has to stay
asynchronous (`Delay(0)` would complete it synchronously). Only the argument changes, so the await machinery is the game's
own, and the wait for the save system above stays as the safety net. `scripts/skip_intro_check.py` proves the layouts and
the flow per build.

`LoadAll` loads the global data and each slot through `PS4Controller.PS4_Load`, whose `PS4SaveTasks.LoadSaveTask` and
`CheckFileExist` poll the flag the native save events clear with `await Task.Delay(100)`: 25 waits in the load task and 12
(plus two 500 ms retries) in the existence check, about 15 per boot. They are intervals, not timeouts (nothing counts them),
and the host's save calls finish at once, so each wait only delays. With Skip Intro on each 100 becomes 1, which keeps the
await asynchronous (the flag is cleared on the main thread); the 500 ms retries and every write path stay as they are.

## Skip Intro: the title intro

`MainMenuState_Intro` has no input to answer: its `TickState` switches to the next state (PRESS START) when `QueueSwitchToNextState`
has set the flag at `this+0x6c`, which the intro animation does at its end, seconds after the state starts. With Skip Intro on,
that flag test calls a stub (the design of Odin Sphere's `launcher/skip_intro.c`) that asks `sos_skip_intro_claim`. The claim is
granted once, when the intro has ticked three times (its start coroutine has run) and its main menu is open and not processing the
load button, which are the conditions `MainMenuState_WaitForInputPress.TickState` needs before it leaves its state; then the stub
sets the same flag and the game's own switch runs. Every later tick, and every later visit to the title, runs the original test.
`scripts/title_input_check.py` proves the offsets, the flag and the title's input path from the game code.

## FFmpeg

`gpu/ffmpeg.txt` pins FFmpeg 8.1.3, built LGPL with only the MP4 demuxer and the H.264, HEVC and AAC decoders (the codecs
the player exposes). `tools\build.ps1` runs `tools/build-ffmpeg.sh` in MSYS2 CLANG64 when `cache\deps\ffmpeg` does not hold
the current pin, passes the prefix to CMake (`DCP_FFMPEG_PREFIX`) and installs the DLLs beside the loader. `tools\setup.ps1`
checks the MSYS2 tools the build needs. The first build takes a few extra minutes.

## Tests

`tools\test_avplayer.ps1` runs the thread tests, the import coverage of every prepared build, and, when the pinned FFmpeg
exists, the API tests: structure layouts, the 23 registered NIDs, invalid handles, Init and Close, the log callback, path
mapping, demux and decode of a clip generated for the run, pause, jump (a no-op), stop and restart, end of file, looping and closing
while busy or from the callback. `DCP_FFMPEG_PREFIX` points the test at another FFmpeg prefix.
