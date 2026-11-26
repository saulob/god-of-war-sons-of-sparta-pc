# Headless proof of the SteamOS / Gamescope touchscreen route: the production STEAM_TOUCH_CLICK_MODE claim against a fake
# X11 property store (claim on focus, the once-a-second rewrite, each root restored on focus loss and on every exit path,
# never in the launcher, the SOS_TOUCH_GAMESCOPE=0 kill switch), screen fingers as DualShock 4 touchpad contacts (picture
# coordinates, two fingers with their own ids, release, tap click), no synthetic-mouse double input, Windows and desktop
# unchanged, plus mutated copies the same test must reject. No window, X server, GPU, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$claim = 'gpu/shim/sos_gamescope_touch.h'
$policy = 'src/sos_touch_gamescope.h'
$screen = 'src/sos_touchscreen.h'
$route = 'gpu/shim/sos_touch_route.h'
$window = 'gpu/shim/window.cpp'
$gpu = 'gpu/shim/dcpgpu.cpp'
$pad = 'src/runtime_pad.c'
$kernel = 'src/runtime_kernel.c'
$runtime = 'src/runtime.c'
$services = 'src/runtime_services.c'
$launcher = @(Get-ChildItem -LiteralPath (Join-Path $root 'launcher') -File |
    Where-Object { $_.Extension -in '.c', '.cpp', '.h' } | ForEach-Object { 'launcher/' + $_.Name })

$mutations = @(
    @{ Name = 'the kill switch is ignored'; File = $policy; Old = 'return !(e->opt_out && !strcmp(e->opt_out, "0"));'; New = 'return 1;' },
    @{ Name = 'the launcher may claim Passthrough'; File = $policy; Old = 'if (!e->linux_host || !e->game_window || !e->gamescope'; New = 'if (!e->linux_host || !e->gamescope' },
    @{ Name = 'Windows claims too'; File = $policy; Old = 'if (!e->linux_host || !e->game_window'; New = 'if (!e->game_window' },
    @{ Name = 'Passthrough is never rewritten while focused'; File = $claim; Old = "        checked_ms_ = now_ms;`n        return WriteAll();"; New = "        checked_ms_ = now_ms;`n        return false;" },
    @{ Name = 'Passthrough is rewritten on every poll'; File = $claim; Old = ' || now_ms - checked_ms_ < SOS_GAMESCOPE_TOUCH_RECHECK_MS) {'; New = ') {' },
    @{ Name = 'a release restores nothing'; File = $claim; Old = 'if (sos_touch_mode_release(&root.mode, read && have, current, &value) && Write(root, value)) {'; New = 'if (false) {' },
    @{ Name = 'a release overwrites a newer Steam value'; File = $policy; Old = 'if (!owned || !have || current != SOS_GAMESCOPE_TOUCH_PASSTHROUGH) return 0;'; New = 'if (!owned) return 0;' },
    @{ Name = 'a stale Passthrough is re-imposed on release'; File = $policy; Old = '    if (root->saved == SOS_GAMESCOPE_TOUCH_PASSTHROUGH) return 0;'; New = '' },
    @{ Name = 'the claim keeps its first saved value'; File = $policy; Old = 'if (!root->owned || (have && current != SOS_GAMESCOPE_TOUCH_PASSTHROUGH)) {'; New = 'if (!root->owned) {' },
    @{ Name = 'a desktop X server is claimed'; File = $claim; Old = 'if (!mine && !api_->intern(display, SOS_GAMESCOPE_TOUCH_MARKER_ATOM, 1)) {'; New = 'if (false) {' },
    @{ Name = 'a released claim keeps rewriting'; File = $claim; Old = "        root_count_ = 0;`n        claimed_ = false;"; New = "        claimed_ = false;" },
    @{ Name = 'the exit-path release is not serialised with the window thread'; File = $claim; Old = "    int Release() {`n        Lock lock{busy_};"; New = "    int Release() {" },
    @{ Name = 'one finger only'; File = $screen; Old = '#define SOS_TOUCH_CONTACTS 2'; New = '#define SOS_TOUCH_CONTACTS 1' },
    @{ Name = 'contact ids are reused'; File = $screen; Old = 's->next_id = (uint8_t)((s->next_id + 1) & 0x7f);'; New = 's->next_id = 0;' },
    @{ Name = 'the letterbox is ignored'; File = $screen; Old = 'SosTouchArea area = {(float)((surface_width - width) / 2) / sw, (float)((surface_height - height) / 2) / sh,'; New = 'SosTouchArea area = {0.0f, 0.0f,' },
    @{ Name = 'the y axis uses the x range'; File = $screen; Old = '*y = (uint16_t)lroundf(sos_touch_clamp01(v) * (float)SOS_TOUCH_MAX_Y);'; New = '*y = (uint16_t)lroundf(sos_touch_clamp01(v) * (float)SOS_TOUCH_MAX_X);' },
    @{ Name = 'a lifted finger never leaves'; File = $screen; Old = "            if (c->click_reads <= 0) {`n                memset(c, 0, sizeof(*c));"; New = "            if (c->click_reads <= 0) {`n                (void)0;" },
    @{ Name = 'a tap does not click'; File = $screen; Old = 'c->click_reads = tap ? SOS_TOUCH_CLICK_READS : 0;'; New = 'c->click_reads = 0;' },
    @{ Name = 'a drag clicks'; File = $screen; Old = 'const int tap = !s->multi && !c->moved &&'; New = 'const int tap = !s->multi &&' },
    @{ Name = 'blocking keeps the contacts'; File = $screen; Old = '    if (s->blocked) sos_touchscreen_release_all(s);'; New = '' },
    @{ Name = 'synthetic mouse reaches the game with the finger'; File = $route; Old = 'return TouchGeneratedMouse(event) ? Use::DropMouse : Use::Unchanged;'; New = 'return Use::Unchanged;' },
    @{ Name = 'a real mouse is dropped on the route'; File = $route; Old = 'return event.motion.which == SDL_TOUCH_MOUSEID;'; New = 'return true;' },
    @{ Name = 'the desktop takes the route too'; File = $route; Old = "    if (!routed) {`n        return Use::Unchanged;"; New = "    if (false) {`n        return Use::Unchanged;" },
    @{ Name = 'a trackpad becomes the touchscreen'; File = $route; Old = '&& type == SDL_TOUCH_DEVICE_DIRECT;'; New = ';' },
    @{ Name = 'focus loss keeps Passthrough'; File = $window; Old = "            ReleaseMouse();`n            ReleaseGamescopeTouch();`n            break;`n        case SDL_EVENT_WINDOW_FOCUS_GAINED:"; New = "            ReleaseMouse();`n            break;`n        case SDL_EVENT_WINDOW_FOCUS_GAINED:" },
    @{ Name = 'Passthrough is rewritten without focus'; File = $window; Old = 'if (g_gamescope_touch.Claimed() && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&'; New = 'if (g_gamescope_touch.Claimed() &&' },
    @{ Name = 'the window skips the kill switch'; File = $window; Old = 'env.opt_out = std::getenv("SOS_TOUCH_GAMESCOPE");'; New = 'env.opt_out = nullptr;' },
    @{ Name = 'the window passes touch-made mouse events to the game'; File = $window; Old = "        if (touch == SosTouchRoute::Use::DropMouse) { // never a mouse press as well as a touchpad contact"; New = "        if (false) {" },
    @{ Name = 'the window thread exit keeps Passthrough'; File = $gpu; Old = '        dcpgpu_touch_release(); // Gamescope touch mode back to each root: no destructor runs after _Exit'; New = '' },
    @{ Name = 'sceKernelExitProcess keeps Passthrough'; File = $kernel; Old = "    runtime_log_exit_request(`"sceKernelExitProcess`",status,(uintptr_t)__builtin_return_address(0));`n    runtime_online_leave();`n    dcpgpu_touch_release();"; New = "    runtime_log_exit_request(`"sceKernelExitProcess`",status,(uintptr_t)__builtin_return_address(0));`n    runtime_online_leave();" },
    @{ Name = 'the touchscreen never reaches the pad'; File = $pad; Old = 'for (int i=0;i<screen && i<2;++i) d->touches[d->touch_count++]=(PadTouch){.x=tx[i],.y=ty[i],.id=tid[i]};'; New = '(void)screen;' },
    @{ Name = 'the pad sampler reads the screen off the route'; File = $window; Old = "    if (!Frontend::g_touch_routed.load(std::memory_order_relaxed)) {`n        return 0;`n    }`n    SosTouchSample sample;"; New = "    SosTouchSample sample;" }
)
$copy = @($claim, $policy, $screen, $route, $window, $gpu, $pad, $kernel, $runtime, $services, 'src/dcpgpu.h') + $launcher
$code = Invoke-HostMutationTest -Name 'touchscreen' -Root $root -Test 'tests/touchscreen_steamos_tests.cpp' `
    -Copy $copy -IncludeDirs @('src') -Mutations $mutations
exit $code
