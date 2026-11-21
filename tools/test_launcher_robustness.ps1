# Headless proof of the launcher's placement diagnostics and restoration, startup stages and show gate, Escape ownership and
# the Bug Report session and card, on the production headers and sources with synthetic displays and time, plus mutated
# copies the same test must reject. No window, GPU, network, launcher or game.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$startup = 'launcher/launcher_startup.h'
$escape = 'launcher/launcher_escape.h'
$quit = 'launcher/launcher_quit.h'
$session = 'launcher/bug_report_session.h'
$placement = 'launcher/placement.h'
$host_cpp = 'launcher/host.cpp'
$launcher = 'launcher/launcher.cpp'
$page = 'launcher/bug_report_page.cpp'
$dialog = 'launcher/problem_dialog.cpp'
$keys = 'src/sos_keys.c'

$copy = @(Get-ChildItem -LiteralPath (Join-Path $root 'launcher') -Filter '*.h' | ForEach-Object { "launcher/$($_.Name)" })
$copy += @(Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.h' | ForEach-Object { "src/$($_.Name)" })
$copy += @($keys, 'launcher/host.cpp', $launcher, $page, $dialog, 'launcher/trophy_page.cpp', 'launcher/package_browser.cpp', 'gpu/shim/window.cpp')

$mutations = @(
    @{ Name = 'the placement line loses the monitor'; File = $startup; Old = 'const int index = DisplayIndexOf(facts.window, displays);'; New = 'const int index = 0;' },
    @{ Name = 'the placement line loses the scale'; File = $startup; Old = 'd.usable.width(), d.usable.height(), d.scale);'; New = 'd.usable.width(), d.usable.height(), 0.0f);' },
    @{ Name = 'a startup stage is no longer recorded'; File = $startup; Old = '        took_[index] = took;
        last_ = now_ns;'; New = '        last_ = now_ns;' },
    @{ Name = 'the slow stage threshold is disabled'; File = $startup; Old = 'took >= kSlowStageNs'; New = 'false' },
    @{ Name = 'the window is shown before the renderer and a frame are ready'; File = $startup; Old = 'if (hidden_mode_ || shown_ || !renderer_ || !presented_) {'; New = 'if (hidden_mode_ || shown_) {' },
    @{ Name = 'the first-frame readiness gate is removed'; File = $startup; Old = '|| !renderer_ || !presented_) {'; New = '|| !renderer_) {' },
    @{ Name = 'the window is shown on every frame'; File = $startup; Old = '        shown_ = true;
        return true;'; New = '        return true;' },
    @{ Name = 'the host shows the window without the gate'; File = $host_cpp; Old = 'if (!window_ || !show_gate_.TryShow()) {'; New = 'if (!window_ || hidden_) {' },
    @{ Name = 'the placement is logged on every frame'; File = $host_cpp; Old = '    frame_begin_ns_ = NowNs();
    ImGui_ImplSDLRenderer3_NewFrame();'; New = '    LogPlacement("frame", saved_placement_);
    frame_begin_ns_ = NowNs();
    ImGui_ImplSDLRenderer3_NewFrame();' },
    @{ Name = 'a minimized window forgets that it was maximized'; File = $placement; Old = 'if (!fullscreen && !state.minimized) {'; New = 'if (!fullscreen) {' },
    @{ Name = 'an off-screen placement loses its maximized state'; File = $placement; Old = 'restored.normal = {};'; New = 'restored = {};' },
    @{ Name = 'Escape closes the launcher'; File = $quit; Old = 'return !escape_pressed;'; New = 'return true;' },
    @{ Name = 'two owners claim the same Escape'; File = $escape; Old = 'if (!pressed_ || taken_) {'; New = 'if (!pressed_) {' },
    @{ Name = 'Escape closes the launcher while a dialog is active'; File = $launcher; Old = 'if (!play_.starting() && LauncherEscape().QuitAllowed()) {'; New = 'if (!play_.starting()) {' },
    @{ Name = 'Escape discards the Bug Report text'; File = $escape; Old = 'if (escape_pressed && was_active) {'; New = 'if (false) {' },
    @{ Name = 'the Bug Report field stops tracking its edit'; File = $page; Old = 'description_active_ = ImGui::IsItemActive();'; New = 'description_active_ = false;' },
    @{ Name = 'the error dialog stops claiming Escape'; File = $dialog; Old = 'LauncherEscape().Take()'; New = 'false' },
    @{ Name = 'Escape can be bound as a key'; File = $keys; Old = ' || scancode == SDL_SCANCODE_ESCAPE) return 0;'; New = ') return 0;' },
    @{ Name = 'a duplicate submission is allowed'; File = $session; Old = 'if (busy() || !package) {'; New = 'if (!package) {' },
    @{ Name = 'a dump failure marks the whole report as failed'; File = $session; Old = 'view.phase = result_->crash == CrashUpload::kFailed ? ReportPhase::kSentDumpFailed : ReportPhase::kSent;'; New = 'view.phase = result_->crash == CrashUpload::kFailed ? ReportPhase::kFailed : ReportPhase::kSent;' },
    @{ Name = 'success is shown before the service confirmed the report'; File = $session; Old = 'work->text_accepted ? ReportPhase::kUploadingDump : ReportPhase::kSendingText'; New = 'ReportPhase::kUploadingDump' },
    @{ Name = 'the dump retry state is lost'; File = $session; Old = '        retry_ = work;
'; New = '' },
    @{ Name = 'the receipt is rendered with the ticket'; File = $session; Old = '        view.ticket = result_->ticket;
        view.phase = result_->crash'; New = '        view.ticket = result_->ticket + result_->credential;
        view.phase = result_->crash' }
)

$code = Invoke-HostMutationTest -Name 'launcher_robustness' -Root $root -Test 'tests/launcher_robustness_tests.cpp' `
    -Copy $copy -IncludeDirs @('launcher', 'src') -RootIncludeDirs @('third_party/trg-launcher/include') `
    -Sources @($keys) -Libraries @('SDL3') -Mutations $mutations
if ($code -ne 0) { exit $code }

# The launcher's dialogs on a headless Dear ImGui context: compiled once, no mutations.
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')
$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_launcher_escape_gui_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $imgui = Join-Path $root 'gpu\third_party\imgui'
    $include = @('-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $root 'third_party\trg-launcher\include'),
                 '-I', $imgui, '-I', (Join-Path $prefix 'include'))
    $objects = New-Object System.Collections.Generic.List[string]
    $inputs = @(
        @{ File = (Join-Path $root 'tests\launcher_escape_gui_tests.cpp'); Flags = @('-Wall', '-Wextra', '-Werror') },
        @{ File = (Join-Path $root 'launcher\problem_dialog.cpp'); Flags = @('-Wall', '-Wextra', '-Werror') },
        @{ File = (Join-Path $root 'launcher\package_browser.cpp'); Flags = @('-Wall', '-Wextra', '-Werror') },
        @{ File = (Join-Path $root 'launcher\file_browser.cpp'); Flags = @('-Wall', '-Wextra', '-Werror') },
        @{ File = (Join-Path $root 'launcher\localization\ui_text.cpp'); Flags = @('-Wall', '-Wextra', '-Werror') }
    )
    foreach ($name in 'imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets') { $inputs += @{ File = (Join-Path $imgui "$name.cpp"); Flags = @('-w', '-O0') } }
    foreach ($input in $inputs) {
        $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($input.File) + '.o')
        & $clangxx '--target=x86_64-w64-mingw32' '-std=c++20' '-O1' @($input.Flags) @include '-c' $input.File '-o' $object
        if ($LASTEXITCODE -ne 0) { Write-Host "  [fail] could not compile $($input.File)"; exit 1 }
        $objects.Add($object)
    }
    $exe = Join-Path $temp 'launcher_escape_gui_tests.exe'
    & $clangxx '--target=x86_64-w64-mingw32' @objects '-L' (Join-Path $prefix 'lib') '-lSDL3' '-lshell32' '-lole32' '-lcomdlg32' '-o' $exe
    if ($LASTEXITCODE -ne 0) { Write-Host '  [fail] could not link the dialog test'; exit 1 }
    $run = Invoke-TimedProgram $exe
    Write-Host $run.Text.TrimEnd()
    exit $run.Code
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
