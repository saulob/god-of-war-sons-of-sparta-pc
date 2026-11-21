# Headless updater tests: versions, release JSON, SHA-256, ZIP safety, staging, jobs and rollback; the update controller
# against a scripted loopback HTTP server (never GitHub); the trg-updater helper on a temporary install folder. The tests
# are compiled directly with clang++ and start only console helper processes: no game, launcher, window or network.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$launcher = Join-Path $root 'launcher'
$updater = Join-Path $launcher 'updater'
$tests = Join-Path $root 'tests'
$trg = Join-Path $root 'third_party\trg-launcher'
if ($PSVersionTable.PSEdition -eq 'Core' -and ($IsLinux -or $IsMacOS)) {
    Write-Host '  [skip] the updater tests are built for Windows'
    exit 0
}
if (-not (Test-Path -LiteralPath (Join-Path $trg 'src\download.cpp'))) {
    Write-Host '  [skip] TRG Launcher is not fetched yet: run tools\build.ps1 once'
    exit 0
}

$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw),
    'project\(sons_of_sparta_pc VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'CMakeLists.txt has no project version' }

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_updater_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clang = Join-Path $tc.Bin 'clang.exe'
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$target = '--target=x86_64-w64-mingw32'

function Slash([string]$Path) { return $Path.Replace('\', '/') }

$exeUpdater = Join-Path $temp 'trg-updater.exe'
$exeMarker = Join-Path $temp 'update_restart_marker.exe'
$config = Join-Path $temp 'test_config.h'
@(
    "#define SOS_SOURCE_DIR `"$(Slash $root)`"",
    "#define SOS_UPDATER_SRC_DIR `"$(Slash $launcher)`"",
    "#define SOS_PORT_VERSION `"$version`"",
    "#define SOS_UPDATER_EXE `"$(Slash $exeUpdater)`"",
    "#define SOS_MARKER_EXE `"$(Slash $exeMarker)`""
) | Set-Content -LiteralPath $config -Encoding ASCII

function Compile-Object([string]$Source, [string]$Standard = 'c++20') {
    $object = Join-Path $temp (([IO.Path]::GetFileNameWithoutExtension($Source)) + '_' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.o')
    & $clangxx $target "-std=$Standard" '-Wall' '-Wextra' '-D_CRT_SECURE_NO_WARNINGS' '-include' $config `
        '-I' $launcher '-I' $tests '-I' (Join-Path $trg 'include') '-c' $Source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "could not compile $Source" }
    return $object
}

function Link-Test([string]$Output, [string[]]$Objects, [string[]]$Arguments = @()) {
    & $clangxx $target $Objects $Arguments '-pthread' '-o' $Output
    if ($LASTEXITCODE -ne 0) { throw "could not link $Output" }
}

$failures = New-Object System.Collections.Generic.List[string]
function Run-Test([string]$Name, [string]$Exe, [string[]]$Arguments = @()) {
    Write-Host "==> $Name"
    & $Exe @Arguments
    if ($LASTEXITCODE -eq 0) { Write-Host "  [ok]   $Name" -ForegroundColor Green }
    else { Write-Host "  [fail] $Name (exit $LASTEXITCODE)" -ForegroundColor Red; $failures.Add($Name) }
}

# One mutation, run in its own job: compile the mutated copy of one production file, link it into the suite that guards the
# rule and run it. Returns detected, missed or broken.
$mutationWork = {
    param($mutation, $c)
    $ErrorActionPreference = 'Stop'
    . (Join-Path $c.Tools '_mutation.ps1')
    function Invoke-TimedExe([string]$Exe, [string]$Arguments, [int]$Seconds) {
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = $Exe
        $info.Arguments = $Arguments
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.CreateNoWindow = $true
        $process = [System.Diagnostics.Process]::Start($info)
        $null = $process.StandardOutput.ReadToEndAsync()
        $null = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($Seconds * 1000)) {
            try { $process.Kill() } catch { $null = $_ }
            return 124
        }
        $process.WaitForExit()
        return $process.ExitCode
    }
    $result = [pscustomobject]@{ Name = $mutation.Name; State = 'broken' }
    $dir = Join-Path $c.Temp ('mutation_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    try {
        $copy = Join-Path $dir 'launcher'
        New-Item -ItemType Directory -Force -Path (Join-Path $copy 'updater') | Out-Null
        Get-ChildItem -LiteralPath $c.Launcher -File | Where-Object { $_.Extension -in '.h', '.cpp' } | Copy-Item -Destination $copy
        Get-ChildItem -LiteralPath $c.Updater -File | Where-Object { $_.Extension -in '.h', '.cpp' } | Copy-Item -Destination (Join-Path $copy 'updater')
        $source = Join-Path $dir $mutation.File
        New-Item -ItemType Directory -Force -Path (Split-Path $source -Parent) | Out-Null
        Copy-Item -LiteralPath (Join-Path $c.Root $mutation.File) -Destination $source -Force
        foreach ($edit in $mutation.Edits) {
            if ($edit.ContainsKey('All')) {
                $text = [IO.File]::ReadAllText($source).Replace("`r`n", "`n")
                if (-not $text.Contains($edit.Old)) { throw "mutation text not found in $($mutation.File): $($edit.Old)" }
                [IO.File]::WriteAllText($source, $text.Replace($edit.Old, $edit.New), (New-Object Text.UTF8Encoding($false)))
            } else {
                Edit-WorkspaceText $source $edit.Old $edit.New
            }
        }
        $suite = $c.Suites[$mutation.Test]
        $objects = New-Object System.Collections.Generic.List[string]
        $objects.AddRange([string[]]$suite.Objects)
        $recompile = @($mutation.File)
        if ($mutation.ContainsKey('Recompile')) { $recompile += $mutation.Recompile }
        foreach ($file in $recompile) {
            if ([IO.Path]::GetExtension($file) -eq '.h') { continue }
            $from = if ($file -eq $mutation.File) { $source } else { Join-Path $c.Root $file }
            $name = [IO.Path]::GetFileNameWithoutExtension($from)
            $standard = if ($file -like 'third_party*') { 'c++17' } else { 'c++20' }
            $object = Join-Path $dir ($name + '_mut.o')
            & $c.Clangxx $c.Target "-std=$standard" '-w' '-D_CRT_SECURE_NO_WARNINGS' '-include' $c.Config `
                '-I' $copy '-I' $c.Launcher '-I' $c.Tests '-I' (Join-Path $c.Trg 'include') '-c' $from '-o' $object 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { return $result }
            $pattern = '^' + [regex]::Escape($name) + '_[0-9a-f]{6}\.o$'
            $kept = @($objects | Where-Object { [IO.Path]::GetFileName($_) -notmatch $pattern })
            $objects.Clear()
            $objects.AddRange([string[]]$kept)
            $objects.Add($object)
        }
        $exe = Join-Path $dir 'mutant.exe'
        & $c.Clangxx $c.Target $objects.ToArray() $suite.Libraries '-pthread' '-o' $exe 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { return $result }
        $arguments = if ($mutation.ContainsKey('Section')) { $mutation.Section } else { '' }
        $result.State = if ((Invoke-TimedExe $exe $arguments 120) -eq 0) { 'missed' } else { 'detected' }
    } catch {
        $result.State = 'broken'
    } finally {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
    return $result
}

function Invoke-Mutations($suites) {
    . (Join-Path $PSScriptRoot 'updater_mutations.ps1')
    $context = @{
        Tools = $PSScriptRoot; Root = $root; Launcher = $launcher; Updater = $updater; Tests = $tests; Trg = $trg; Temp = $temp
        Clangxx = $clangxx; Target = $target; Config = $config; Suites = $suites
    }
    $jobs = New-Object System.Collections.Generic.List[object]
    $results = New-Object System.Collections.Generic.List[object]
    $collect = {
        param($finished)
        foreach ($job in $finished) {
            $output = @(Receive-Job -Job $job -ErrorAction SilentlyContinue | Where-Object { $_ -and $_.PSObject.Properties['State'] })
            $results.Add($(if ($output.Count -gt 0) { $output[-1] } else { [pscustomobject]@{ Name = $job.Name; State = 'broken' } }))
            Remove-Job -Job $job -Force
            $jobs.Remove($job) | Out-Null
        }
    }
    foreach ($mutation in $UpdaterMutations) {
        while ($jobs.Count -ge 4) {
            $null = Wait-Job -Job $jobs.ToArray() -Any -Timeout 5
            & $collect @($jobs | Where-Object { $_.State -ne 'Running' })
        }
        $job = Start-Job -Name $mutation.Name -ScriptBlock $mutationWork -ArgumentList $mutation, $context
        $jobs.Add($job)
    }
    while ($jobs.Count -gt 0) {
        $null = Wait-Job -Job $jobs.ToArray() -Any -Timeout 5
        & $collect @($jobs | Where-Object { $_.State -ne 'Running' })
    }
    $detected = 0
    foreach ($mutation in $UpdaterMutations) {
        $state = @($results | Where-Object { $_.Name -eq $mutation.Name })[0].State
        if ($state -eq 'detected') { $detected++; Write-Host "  mutation detected: $($mutation.Name)" }
        elseif ($state -eq 'missed') { Write-Host "  [fail] mutation '$($mutation.Name)' was not detected"; $failures.Add("mutation: $($mutation.Name)") }
        else { Write-Host "  [fail] mutation '$($mutation.Name)' did not build or run"; $failures.Add("mutation: $($mutation.Name)") }
    }
    Write-Host "updater mutations: $detected of $($UpdaterMutations.Count) detected"
}

try {
    $miniz = Join-Path $temp 'miniz.o'
    & $clang $target '-std=gnu11' '-w' '-c' (Join-Path $updater 'third_party\miniz\miniz.c') '-o' $miniz
    if ($LASTEXITCODE -ne 0) { throw 'could not compile miniz' }

    $core = 'github_release', 'install_transaction', 'json', 'sha256', 'update_job', 'update_paths', 'update_platform', 'update_zip', 'version' |
        ForEach-Object { Compile-Object (Join-Path $updater "$_.cpp") }
    $controller = 'update_controller', 'update_http' | ForEach-Object { Compile-Object (Join-Path $updater "$_.cpp") }
    $updateConfig = Compile-Object (Join-Path $launcher 'update_config.cpp')
    $trgCore = 'settings', 'platform', 'download' | ForEach-Object { Compile-Object (Join-Path $trg "src\$_.cpp") 'c++17' }

    $exeTests = Join-Path $temp 'sos-update-tests.exe'
    $unitObject = Compile-Object (Join-Path $tests 'update_tests.cpp')
    Link-Test $exeTests (@($unitObject) + $core + $miniz) @('-lshell32')
    Run-Test 'updater (no network)' $exeTests

    $exeNetwork = Join-Path $temp 'sos-update-network-tests.exe'
    $networkObject = Compile-Object (Join-Path $tests 'update_network_tests.cpp')
    Link-Test $exeNetwork (@($networkObject) + $updateConfig + $controller + $core + $trgCore + $miniz) `
        @('-lshell32', '-lws2_32', '-lwinhttp', '-lole32', '-lcomdlg32')
    Run-Test 'updater on a loopback server' $exeNetwork

    $exeHttp = Join-Path $temp 'sos-update-http-tests.exe'
    $httpObject = Compile-Object (Join-Path $tests 'update_http_tests.cpp')
    Link-Test $exeHttp (@($httpObject) + $controller[1] + $trgCore) `
        @('-lshell32', '-lws2_32', '-lwinhttp', '-lole32', '-lcomdlg32')
    Run-Test 'updater HTTP hardening on a loopback server' $exeHttp

    Link-Test $exeUpdater (@(Compile-Object (Join-Path $updater 'helper_main.cpp')) + $core[1] + $core[3] + $core[4] + $core[5] + $core[6]) `
        @('-static', '-mwindows', '-lshell32')
    Link-Test $exeMarker @(Compile-Object (Join-Path $tests 'update_restart_marker.cpp')) @('-static', '-mwindows', '-lshell32')
    $exeHelper = Join-Path $temp 'sos-update-helper-tests.exe'
    Link-Test $exeHelper @(Compile-Object (Join-Path $tests 'update_helper_tests.cpp')) @('-lshell32')
    Run-Test 'update helper' $exeHelper

    $networkLibraries = @('-lshell32', '-lws2_32', '-lwinhttp', '-lole32', '-lcomdlg32')
    $suites = @{
        http    = @{ Objects = @($httpObject) + $controller[1] + $trgCore; Libraries = $networkLibraries }
        network = @{ Objects = @($networkObject) + $updateConfig + $controller + $core + $trgCore + $miniz; Libraries = $networkLibraries }
        unit    = @{ Objects = @($unitObject) + $core + $miniz; Libraries = @('-lshell32') }
    }
    Invoke-Mutations $suites
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) failed: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'Updater tests passed.' -ForegroundColor Green
