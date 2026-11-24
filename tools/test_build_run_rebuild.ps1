# tools\build.ps1 -Run starts from no dcpgpu objects, library or loader; a build without -Run stays incremental. The deletion is
# run on a mock build tree and the order of the steps is read from the script. No game, launcher, build or window.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')

$failures = 0
function Check([bool]$Ok, [string]$Name) {
    if ($Ok) { Write-Host "  ok    $Name" } else { Write-Host "  [fail] $Name"; $script:failures++ }
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ('sos_build_run_test_' + [guid]::NewGuid().ToString('N'))

function New-MockBuild([string]$Dir) {
    $files = 'gpu\CMakeFiles\dcpgpu.dir\shim\window.cpp.obj', 'gpu\CMakeFiles\dcpgpu.dir\shadps4\video_core\a\b.cpp.obj',
        'gpu\libdcpgpu.a', 'bin\sons_of_sparta.exe', 'bin\dcpgpu_link_smoke.exe', 'bin\assets\launcher\x.png',
        'CMakeFiles\sons_of_sparta.dir\src\loader.c.obj', 'CMakeFiles\dcpgpu_link_smoke.dir\src\gpu_link_smoke.cpp.obj',
        'launcher\CMakeFiles\sos_launcher.dir\launcher.cpp.obj', 'gpu\libother.a', 'CMakeCache.txt', 'build.ninja',
        '.ninja_deps', '.ninja_log', 'generated\sos_version.h'
    foreach ($file in $files) {
        $path = Join-Path $Dir $file
        New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        Set-Content -LiteralPath $path -Value 'x'
    }
}

try {
    $mock = Join-Path $temp 'build'
    New-MockBuild $mock
    Remove-GpuProductionArtifacts $mock
    $left = @(Get-ChildItem -LiteralPath $mock -Recurse -File | ForEach-Object { $_.FullName.Substring($mock.Length + 1) })
    Check (@(Get-ChildItem -LiteralPath (Join-Path $mock 'gpu\CMakeFiles\dcpgpu.dir') -Force).Count -eq 0) 'the dcpgpu object directory is emptied'
    Check (-not (Test-Path -LiteralPath (Join-Path $mock 'gpu\libdcpgpu.a'))) 'libdcpgpu.a is deleted'
    Check (-not (Test-Path -LiteralPath (Join-Path $mock 'bin\sons_of_sparta.exe'))) 'sons_of_sparta.exe is deleted'
    $expected = 'bin\dcpgpu_link_smoke.exe', 'bin\assets\launcher\x.png', 'CMakeFiles\sons_of_sparta.dir\src\loader.c.obj',
        'CMakeFiles\dcpgpu_link_smoke.dir\src\gpu_link_smoke.cpp.obj', 'launcher\CMakeFiles\sos_launcher.dir\launcher.cpp.obj',
        'gpu\libother.a', 'generated\sos_version.h'
    Check (@($expected | Where-Object { $left -notcontains $_ }).Count -eq 0) 'runtime, launcher, smoke and other objects and outputs are kept'
    Check (($left -contains '.ninja_deps') -and ($left -contains '.ninja_log')) '.ninja_deps and .ninja_log are kept'
    Check (($left -contains 'CMakeCache.txt') -and ($left -contains 'build.ninja')) 'the CMake cache and build graph are kept'
    Remove-GpuProductionArtifacts $mock
    Remove-GpuProductionArtifacts (Join-Path $temp 'missing')
    Check $true 'missing files and directories are not an error'

    $mockLocked = Join-Path $temp 'locked'
    New-MockBuild $mockLocked
    $stream = [IO.File]::Open((Join-Path $mockLocked 'bin\sons_of_sparta.exe'), 'Open', 'ReadWrite', 'None')
    try {
        $threw = $false
        try { Remove-GpuProductionArtifacts $mockLocked } catch { $threw = $true }
        Check $threw 'a file that cannot be deleted stops the build with an error'
    }
    finally { $stream.Dispose() }

    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build.ps1'))
    function Position([string]$Pattern) { $m = [regex]::Match($text, $Pattern); if ($m.Success) { $m.Index } else { -1 } }
    $lock = Position 'Enter-BuildLock \$buildDir'
    $configure = Position "Write-Step 'Configuring'"
    $force = Position 'Remove-GpuProductionArtifacts \$buildDir'
    $ninja = Position "Invoke-Native ninja @\('-C', \`$buildDir, 'sons_of_sparta'\)"
    $unlock = Position 'Exit-BuildLock \$buildLock'
    $launch = Position '& \$loader @arguments'
    $guard = [regex]::Match($text, '(?s)if \(\$Run\) \{\s*Write-Step ''Forcing GPU production rebuild for -Run''\s*Remove-GpuProductionArtifacts \$buildDir\s*\}')
    Check ($guard.Success -and ([regex]::Matches($text, 'Remove-GpuProductionArtifacts').Count -eq 1)) 'the forced deletion runs only for -Run, once'
    Check ($lock -ge 0 -and $configure -gt $lock -and $force -gt $configure) 'the deletion follows the lock and the configuration'
    Check ($force -ge 0 -and $ninja -gt $force) 'the deletion runs before ninja'
    Check ($unlock -gt $ninja -and ([regex]::Matches($text, 'Exit-BuildLock').Count -eq 1)) 'the lock is held from before the deletion until after ninja'
    Check ($launch -gt $unlock -and $launch -gt $ninja) 'the game launches only after the build'
    Check ([regex]::IsMatch($text, 'if \(\$buildResult -ne 0\) \{ Stop-Tool') -and [regex]::IsMatch($text, '(?s)catch \{\s*Stop-Tool') -and $launch -gt (Position 'Close-BuildLog 0')) 'a failed deletion or build stops through Stop-Tool before the launch'
    Check (-not [regex]::IsMatch($text, 'Remove-Item[^\r\n]*(ninja|CMakeCache|build\.ninja)')) 'build.ps1 deletes no ninja or CMake files'
}
finally { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }

if ($failures) { exit 1 }
Write-Host '  -Run forces the dcpgpu rebuild under the build lock; a build without -Run is incremental'
exit 0
