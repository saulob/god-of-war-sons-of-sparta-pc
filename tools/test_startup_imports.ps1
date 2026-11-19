# Headless self-check for the startup import audit: the scanner is deterministic, consumes runtime
# evidence, keeps runtime-only hardware APIs optional, and the batch's resolver entries and ABI
# assertions are present. No game, launcher or GPU is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$python = Get-Python
if (-not $python) { Stop-Tool 'Python 3 not found. Run tools\setup.ps1.' }
$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_import_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

function Invoke-Scanner([string]$Image, [string]$Report, [string]$RuntimeLog) {
    $arguments = @((Join-Path $root 'tools\analyze-startup-imports.py'))
    if ($Image) { $arguments += @('--image', $Image) } else { $arguments += @('--build', 'CUSA57767-1.00') }
    if ($RuntimeLog) { $arguments += @('--runtime-log', $RuntimeLog) }
    $arguments += @('--report', $Report)
    $text = & $python.Exe @($python.Args) @arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "scanner failed for $Image" }
    return [IO.File]::ReadAllText($Report)
}

$failures = 0
function Check([bool]$Condition, [string]$Text) {
    if ($Condition) { Write-Ok $Text } else { Write-Bad $Text; $script:failures++ }
}

try {
    $log = Join-Path $root 'build\logs\sons_of_sparta_033.log'
    $first = Invoke-Scanner $null (Join-Path $temp 'first.txt') $log
    $second = Invoke-Scanner $null (Join-Path $temp 'second.txt') $log
    Check ($first -match 'HIGH: 0') '1.00 has no HIGH unresolved import'
    Check ($first -match 'HIGH_STARTUP_PROBE: 0') '1.00 has no unresolved startup probe'
    Check ($first -match 'MEDIUM: 0') '1.00 has no MEDIUM unresolved import'
    Check ($first -match 'implemented but unregistered: 0') '1.00 has no implemented-but-unregistered import'
    Check ((Get-FileHash (Join-Path $temp 'first.txt')).Hash -eq (Get-FileHash (Join-Path $temp 'second.txt')).Hash) `
        'scanner output is deterministic with runtime evidence'

    Check ($first -match '(?s)Runtime-confirmed APIs:.*?sceMoveInit') 'sceMoveInit is runtime-confirmed'
    Check ($first -match '(?s)Probe-classified imports.*?sceMouseInit') 'its startup sibling is a startup probe'
    Check ($first -match 'libSceCamera .*resolved=  8 unresolved=  0') 'libSceCamera is fully resolved'
    Check ($first -match 'libSceHmd .*unresolved=  0') 'libSceHmd is fully resolved'
    Check ($first -match 'libSceVrTracker .*unresolved=  0') 'libSceVrTracker is fully resolved'
    Check ($first -match '0x8f libSceMove') 'module evidence maps 0x8f to libSceMove'
    Check ($first -match '0x95 libSceIme') 'module evidence maps 0x95 to libSceIme'

    $image101 = Join-Path $root 'build\image\CUSA57767-1.01'
    if (Test-Path -LiteralPath $image101) {
        $report101 = Invoke-Scanner $image101 (Join-Path $temp '1.01.txt') $null
        Check ($report101 -match 'HIGH: 0') '1.01 has no HIGH unresolved import'
        Check ($report101 -match 'MEDIUM: 0') '1.01 has no MEDIUM unresolved import'
    } else {
        Write-Warn 'no build\image\CUSA57767-1.01; 1.01 audit skipped'
    }

    $sources = @{}
    foreach ($name in 'runtime_services.c', 'runtime_pad.c', 'runtime_savedata.c', 'runtime_move.c') {
        $sources[$name] = Get-Content -LiteralPath (Join-Path $root "src\$name") -Raw
    }
    $videoOut = Get-Content -LiteralPath (Join-Path $root 'gpu\shadps4\core\libraries\videoout\video_out.cpp') -Raw

    $exports = @(
        @('sceSystemServiceGetHdrToneMapLuminance', 'runtime_services.c'),
        @('sceSystemServiceParamGetString', 'runtime_services.c'),
        @('scePadDeviceClassGetExtendedInformation', 'runtime_pad.c'),
        @('scePadDeviceClassParseData', 'runtime_pad.c'),
        @('sceSaveDataSetupSaveDataMemory2', 'runtime_savedata.c'),
        @('sceSaveDataGetSaveDataMemory2', 'runtime_savedata.c'),
        @('sceSaveDataSetSaveDataMemory2', 'runtime_savedata.c'),
        @('sceSaveDataSyncSaveDataMemory', 'runtime_savedata.c'),
        @('sceMoveInit', 'runtime_move.c'),
        @('sceMoveOpen', 'runtime_move.c'),
        @('sceMoveGetDeviceInfo', 'runtime_move.c'),
        @('sceMoveReadStateLatest', 'runtime_move.c'),
        @('sceMoveReadStateRecent', 'runtime_move.c'),
        @('sceMoveGetExtensionPortInfo', 'runtime_move.c'),
        @('sceMoveSetVibration', 'runtime_move.c'),
        @('sceMoveSetLightSphere', 'runtime_move.c'),
        @('sceMoveResetLightSphere', 'runtime_move.c'),
        @('sceMoveClose', 'runtime_move.c'),
        @('sceMoveTerm', 'runtime_move.c'),
        @('sceImeKeyboardGetResourceId', 'runtime_services.c'),
        @('sceImeKeyboardGetInfo', 'runtime_services.c'),
        @('sceHttpUriParse', 'runtime_services.c'),
        @('sceSslGetCaCerts', 'runtime_services.c'),
        @('sceWebBrowserDialogOpen', 'runtime_services.c'),
        @('sceSocialScreenSetMode', 'runtime_services.c'),
        @('sceRemoteplayGetConnectionStatus', 'runtime_services.c'),
        @('sceCameraIsAttached', 'runtime_move.c'),
        @('sceHmdInitialize315', 'runtime_move.c'),
        @('sceVrTrackerInit', 'runtime_move.c')
    )
    foreach ($entry in $exports) {
        Check ($sources[$entry[1]] -match ('\{"' + [regex]::Escape($entry[0]) + '",')) "$($entry[0]) is registered"
    }
    Check ($first -match 'libSceIme .*resolved=  5 unresolved=  0') 'libSceIme is fully resolved'
    Check ($videoOut -match 'LIB_FUNCTION\("ktP9j1fN-zE"') 'sceVideoOutConfigureOptionsInitialize_ is registered'
    Check ($sources['runtime_move.c'] -match 'sizeof\(MoveData\)==64') 'Move data ABI is asserted'
    Check ($sources['runtime_move.c'] -match 'sizeof\(VrInitParam\)==128') 'VrTracker init ABI is asserted'
    Check ($sources['runtime_savedata.c'] -match 'sizeof\(MemoryGet2\)==64') 'SaveDataMemory2 request ABI is asserted'
    Check ($sources['runtime_savedata.c'] -match 'sizeof\(MemorySetup2\)==64') 'SaveDataMemory2 setup ABI is asserted'
    Check ($sources['runtime_pad.c'] -match 'sizeof\(DeviceClassExtendedInformation\)==20') `
        'pad device class ABI is asserted'
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures) { Write-Bad "$failures check(s) failed"; exit 1 }
Write-Ok 'all startup import checks passed'
exit 0
