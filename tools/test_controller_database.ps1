# Headless tests of user/gamecontrollerdb.txt and unmapped joystick diagnostics: pure unit and source-wiring tests,
# real SDL3 virtual-joystick integration scenarios in isolated child processes, and mutated temporary copies of
# src/sos_pad_mapping.h and src/runtime_pad.c that must each be rejected. No window, game, launcher or audio is opened.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_controller_db_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$prefix = Split-Path $tc.Bin -Parent
$clang = Join-Path $tc.Bin 'clang.exe'
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$unitTest = Join-Path $root 'tests\controller_database_tests.cpp'
$sdlTest = Join-Path $root 'tests\controller_database_sdl_tests.c'

$copyFiles = @('src/sos_pad_mapping.h', 'src/runtime_pad.c', 'src/runtime.h', 'src/compat_win.h', 'src/dcpgpu.h',
               'src/sos_keys.h', 'src/sos_pad_kind.h', 'src/sos_sdl_compat.h', 'src/loader.c')

function New-Workspace([string]$Dir) {
    foreach ($rel in $copyFiles) {
        Copy-ToWorkspace $Dir (Join-Path $root $rel) $rel | Out-Null
    }
}

function Build-Unit([string]$Dir, [string]$Exe, [string[]]$Extra = @()) {
    $srcDir = $Dir.Replace('\', '/')
    & $clangxx '--target=x86_64-w64-mingw32' '-std=c++23' '-Wall' '-Wextra' '-O1' @Extra `
        "-DSOS_SOURCE_DIR=$srcDir" '-I' (Join-Path $Dir 'src') $unitTest '-o' $Exe
}

function Build-Sdl([string]$Dir, [string]$Exe, [string[]]$Extra = @()) {
    $padSrc = (Join-Path $Dir 'src\runtime_pad.c').Replace('\', '/')
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-O1' '-D_WIN32_WINNT=0x0A00' @Extra `
        '-I' (Join-Path $Dir 'src') '-I' (Join-Path $prefix 'include') `
        (Get-StringDefine 'PAD_SOURCE' $padSrc) $sdlTest `
        '-L' (Join-Path $prefix 'lib') '-lSDL3' '-lpthread' '-o' $Exe
}

$result = 0
try {
    $base = Join-Path $temp 'base'
    New-Workspace $base

    $unitExe = Join-Path $base 'unit.exe'
    Build-Unit $base $unitExe @('-Werror')
    if ($LASTEXITCODE -ne 0) { throw 'could not compile controller_database_tests.cpp' }
    $unitRun = Invoke-TimedProgram $unitExe
    Write-Host $unitRun.Text.TrimEnd()
    if ($unitRun.Code -ne 0) { $result = 1 }

    $sdlExe = Join-Path $base 'sdl.exe'
    Build-Sdl $base $sdlExe @('-Werror')
    if ($LASTEXITCODE -ne 0) { throw 'could not compile controller_database_sdl_tests.c' }
    $sdlRun = Invoke-TimedProgram $sdlExe 90
    Write-Host $sdlRun.Text.TrimEnd()
    if ($sdlRun.Code -ne 0) { $result = 1 }

    $unitMutations = @(
        @{ Name = 'wrong user path used'; File = 'src/sos_pad_mapping.h';
           Old = 'static const char name[] = "gamecontrollerdb.txt";';
           New = 'static const char name[] = "wrong_db.txt";' },
        @{ Name = 'unmapped joystick logs every event'; File = 'src/sos_pad_mapping.h';
           Old = 'if (seen->ids[i] == id) return 0;';
           New = 'if (seen->ids[i] == id && 0) return 0;' },
        @{ Name = 'GUID omitted from unmapped diagnostic'; File = 'src/sos_pad_mapping.h';
           Old = 'guid ? guid : ""';
           New = '(guid ? "" : "")' },
        @{ Name = 'VID:PID omitted when available'; File = 'src/sos_pad_mapping.h';
           Old = 'if (vendor || product) {';
           New = 'if (0 && (vendor || product)) {' },
        @{ Name = 'removal fails to clean per-device log-once state'; File = 'src/sos_pad_mapping.h';
           Old = 'seen->count = kept;';
           New = '(void)kept;' },
        @{ Name = 'local path exposed in load log'; File = 'src/sos_pad_mapping.h';
           Old = 'snprintf(line, size, "Runtime: loaded %d user controller %s from user/gamecontrollerdb.txt",' + "`n" +
                 '                 added, added == 1 ? "mapping" : "mappings");';
           New = 'snprintf(line, size, "Runtime: loaded %d user controller %s from %s",' + "`n" +
                 '                 added, added == 1 ? "mapping" : "mappings", path ? path : "");' }
    )

    $sdlMutations = @(
        @{ Name = 'user gamepad database not loaded'; File = 'src/runtime_pad.c'; Scenario = 'valid-and-hotplug';
           Old = 'if (sdl_ready>0) load_user_gamepad_mappings();';
           New = 'if (sdl_ready>0) { (void)load_user_gamepad_mappings; }' },
        @{ Name = 'mapping load moved after device classification'; File = 'src/runtime_pad.c'; Scenario = 'valid-and-hotplug';
           Old = "    if (!sdl_ready) {`n        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) sos_prepare_sdl_gamepad();`n        sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;`n        if (sdl_ready>0) load_user_gamepad_mappings();`n    }";
           New = "    static int deferred_load;`n    if (!sdl_ready) {`n        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) sos_prepare_sdl_gamepad();`n        sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;`n        deferred_load = sdl_ready > 0;`n    }";
           Old2 = "    SDL_free(ids);`n    if (changed) publish_pad_kind();";
           New2 = "    SDL_free(ids);`n    if (deferred_load) { deferred_load = 0; load_user_gamepad_mappings(); }`n    if (changed) publish_pad_kind();" },
        @{ Name = 'mapped controller incorrectly reported as unmapped'; File = 'src/runtime_pad.c'; Scenario = 'missing';
           Old = 'if (SDL_IsGamepad(ids[j]) || !sos_unmapped_first_sight(&seen,ids[j])) continue;';
           New = 'if (!sos_unmapped_first_sight(&seen,ids[j])) continue;' }
    )

    $detected = 0
    $total = $unitMutations.Count + $sdlMutations.Count

    foreach ($m in $unitMutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Workspace $work
        Edit-WorkspaceText (Join-Path $work $m.File) $m.Old $m.New
        $mutExe = Join-Path $work 'unit.exe'
        $srcDir = $work.Replace('\', '/')
        $incDir = Join-Path $work 'src'
        $build = {
            & $clangxx '--target=x86_64-w64-mingw32' '-std=c++23' '-Wall' '-Wextra' '-O1' '-w' `
                "-DSOS_SOURCE_DIR=$srcDir" '-I' $incDir $unitTest '-o' $mutExe
        }.GetNewClosure()
        if (Test-MutationDetected $m.Name $build $mutExe) { $detected++ } else { $result = 1 }
    }

    foreach ($m in $sdlMutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Workspace $work
        Edit-WorkspaceText (Join-Path $work $m.File) $m.Old $m.New
        if ($m.ContainsKey('Old2')) { Edit-WorkspaceText (Join-Path $work $m.File) $m.Old2 $m.New2 }
        $mutExe = Join-Path $work 'sdl.exe'
        Build-Sdl $work $mutExe @('-w')
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $mutExe)) {
            Write-Host "  [fail] mutation '$($m.Name)' did not compile"
            $result = 1
            continue
        }
        $scenDir = Join-Path $work 'scen'
        New-Item -ItemType Directory -Force -Path $scenDir | Out-Null
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = $mutExe
        $info.Arguments = "scenario $($m.Scenario) `"$scenDir`""
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.CreateNoWindow = $true
        $proc = [System.Diagnostics.Process]::Start($info)
        $out = $proc.StandardOutput.ReadToEndAsync()
        $err = $proc.StandardError.ReadToEndAsync()
        $finished = $proc.WaitForExit(30000)
        if (-not $finished) { try { $proc.Kill() } catch {} }
        $code = if ($finished) { $proc.ExitCode } else { 124 }
        $proc.Dispose()
        if ($code -ne 0) {
            Write-Host "  mutation detected: $($m.Name)"
            $detected++
        } else {
            Write-Host "  [fail] mutation '$($m.Name)' was not detected"
            $result = 1
        }
    }

    Write-Host "Controller database mutations: $detected of $total detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
