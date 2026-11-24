# Build safety and incrementality contracts of tools\build.ps1: -Run must not reset the GPU artifacts, an unchanged
# runtime identity must leave them alone, a changed compiler, configuration, FFmpeg pin or video-core dependency pin or
# patch must reset them, and a changed dependency pin must be noticed even when its checkout directory exists. Exercised
# on disposable fixtures; no game, launcher, build or window.
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

function New-GpuTree([string]$Dir) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Dir 'patches\imgui'), (Join-Path $Dir 'patches\trg-launcher') | Out-Null
    Set-Content -LiteralPath (Join-Path $Dir 'ffmpeg.txt') -Value 'version=6.0'
    Set-Content -LiteralPath (Join-Path $Dir 'deps.txt') -Value @(
        'third_party/imgui https://example.invalid/imgui.git aaaa',
        'third_party/miniz https://example.invalid/miniz.git bbbb',
        '../third_party/trg-launcher https://example.invalid/trg.git cccc'
    )
    Set-Content -LiteralPath (Join-Path $Dir 'patches\imgui\0001-x.patch') -Value 'patch one'
    Set-Content -LiteralPath (Join-Path $Dir 'patches\trg-launcher\0001-y.patch') -Value 'launcher patch one'
}

function Get-LeftFiles([string]$Dir) {
    @(Get-ChildItem -LiteralPath $Dir -Recurse -File | ForEach-Object { $_.FullName.Substring($Dir.Length + 1) })
}

try {
    $gpu = Join-Path $temp 'gpu-tree'
    New-GpuTree $gpu
    $base = Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu
    $baseToolchain = Get-GpuIdentityPart $base 'toolchain'
    $baseDependencies = Get-GpuIdentityPart $base 'dependencies'
    Check (($baseToolchain -match '^[0-9A-F]{64}$') -and ($baseDependencies -match '^[0-9A-F]{64}$')) 'the GPU runtime identity has a toolchain and a dependency part'
    Check ($base -eq (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu)) 'the same GPU inputs give the same identity'
    $changed = Get-GpuRuntimeIdentity 'Release' 'clang version 20' $gpu
    Check ((Get-GpuIdentityPart $changed 'toolchain') -ne $baseToolchain -and (Get-GpuIdentityPart $changed 'dependencies') -eq $baseDependencies) 'a configuration change invalidates the toolchain part only'
    $changed = Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 21' $gpu
    Check ((Get-GpuIdentityPart $changed 'toolchain') -ne $baseToolchain -and (Get-GpuIdentityPart $changed 'dependencies') -eq $baseDependencies) 'a compiler change invalidates the toolchain part only'

    $ffmpeg = Join-Path $gpu 'ffmpeg.txt'
    Set-Content -LiteralPath $ffmpeg -Value 'version=6.1'
    Check ((Get-GpuIdentityPart (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu) 'toolchain') -ne $baseToolchain) 'an FFmpeg pin change invalidates the toolchain part'
    Set-Content -LiteralPath $ffmpeg -Value 'version=6.0'

    $patch = Join-Path $gpu 'patches\imgui\0001-x.patch'
    Set-Content -LiteralPath $patch -Value 'patch two'
    $changed = Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu
    Check ((Get-GpuIdentityPart $changed 'dependencies') -ne $baseDependencies -and (Get-GpuIdentityPart $changed 'toolchain') -eq $baseToolchain) 'a video-core patch change invalidates the dependency part only'
    Set-Content -LiteralPath $patch -Value 'patch one'
    Check ($base -eq (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu)) 'restoring a patch restores the identity'
    New-Item -ItemType Directory -Force -Path (Join-Path $gpu 'shadps4\video_core') | Out-Null
    Set-Content -LiteralPath (Join-Path $gpu 'shadps4\video_core\vk_example.cpp') -Value 'source'
    Check ($base -eq (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu)) 'a GPU source edit does not reset the artifacts; Ninja recompiles its consumers'
    Set-Content -LiteralPath (Join-Path $temp 'launcher.cpp') -Value 'source'
    Check ($base -eq (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu)) 'a launcher source edit does not reset the video core'

    $deps = Join-Path $gpu 'deps.txt'
    $originalDeps = [IO.File]::ReadAllText($deps)
    [IO.File]::WriteAllText($deps, $originalDeps.Replace('bbbb', 'bbbc'))
    Check ((Get-GpuIdentityPart (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu) 'dependencies') -ne $baseDependencies) 'a video-core pin change invalidates the dependency part'
    [IO.File]::WriteAllText($deps, $originalDeps.Replace('cccc', 'cccd'))
    $launcherPatch = Join-Path $gpu 'patches\trg-launcher\0001-y.patch'
    Set-Content -LiteralPath $launcherPatch -Value 'launcher patch two'
    Check ($base -eq (Get-GpuRuntimeIdentity 'RelWithDebInfo' 'clang version 20' $gpu)) 'launcher-only pins and patches do not touch the video core identity'
    [IO.File]::WriteAllText($deps, $originalDeps)
    Set-Content -LiteralPath $launcherPatch -Value 'launcher patch one'

    $dep = Join-Path $temp 'dep'
    New-Item -ItemType Directory -Force -Path (Join-Path $dep '.git') | Out-Null
    Set-Content -LiteralPath (Join-Path $dep 'source.cpp') -Value 'x'
    $depStamp = Join-Path $dep '.sos_commit'
    Check (-not (Test-PinnedDependency $dep $depStamp 'aaaa')) 'a checkout without a stamp is not current'
    Set-Content -LiteralPath $depStamp -Value 'aaaa'
    Check (Test-PinnedDependency $dep $depStamp 'aaaa') 'a checkout at the pinned commit and patch fingerprint is current'
    Check (-not (Test-PinnedDependency $dep $depStamp 'aaab')) 'a changed commit or patch fingerprint re-fetches an existing checkout'
    Remove-Item -LiteralPath (Join-Path $dep '.git') -Recurse -Force
    Check (-not (Test-PinnedDependency $dep $depStamp 'aaaa')) 'a checkout without git metadata is not current'
    $empty = Join-Path $temp 'empty'
    New-Item -ItemType Directory -Force -Path (Join-Path $empty '.git') | Out-Null
    Set-Content -LiteralPath (Join-Path $empty '.sos_commit') -Value 'aaaa'
    Check (-not (Test-PinnedDependency $empty (Join-Path $empty '.sos_commit') 'aaaa')) 'a checkout without worktree files is not current'

    $compiler = Join-Path $temp 'clang++.exe'
    Set-Content -LiteralPath $compiler -Value 'compiler'
    $compilerTime = [DateTime]::UtcNow.AddDays(-30)
    (Get-Item -LiteralPath $compiler).LastWriteTimeUtc = $compilerTime

    $mock = Join-Path $temp 'build'
    New-MockBuild $mock
    $identity = 'toolchain=' + ('A' * 64) + "`ndependencies=" + ('B' * 64)
    Sync-GpuRuntimeArtifacts $mock $identity $compiler
    Check ((Test-Path -LiteralPath (Join-Path $mock 'gpu\libdcpgpu.a')) -and
           (Test-Path -LiteralPath (Join-Path $mock 'bin\sons_of_sparta.exe'))) 'existing artifacts without a stamp are kept when the compiler predates them'
    Set-GpuRuntimeStamp $mock $identity
    $stampPath = Get-GpuRuntimeStampPath $mock
    $stampTime = (Get-Item -LiteralPath $stampPath).LastWriteTimeUtc
    Start-Sleep -Milliseconds 50
    Set-GpuRuntimeStamp $mock $identity
    Check ((Get-Item -LiteralPath $stampPath).LastWriteTimeUtc -eq $stampTime) 'an unchanged identity does not rewrite the stamp'
    Sync-GpuRuntimeArtifacts $mock $identity $compiler
    Check ((Test-Path -LiteralPath (Join-Path $mock 'gpu\libdcpgpu.a')) -and
           (Test-Path -LiteralPath (Join-Path $mock 'bin\sons_of_sparta.exe'))) 'a matching identity keeps the current artifacts'
    $dependencyOnly = 'toolchain=' + ('A' * 64) + "`ndependencies=" + ('C' * 64)
    Sync-GpuRuntimeArtifacts $mock $dependencyOnly $compiler
    Check ((Test-Path -LiteralPath (Join-Path $mock 'gpu\libdcpgpu.a')) -and
           (Test-Path -LiteralPath (Join-Path $mock 'bin\sons_of_sparta.exe'))) 'a dependency-only change is left to Ninja''s affected consumers'
    $toolchainChange = 'toolchain=' + ('D' * 64) + "`ndependencies=" + ('C' * 64)
    Sync-GpuRuntimeArtifacts $mock $toolchainChange $compiler
    Check (@(Get-ChildItem -LiteralPath (Join-Path $mock 'gpu\CMakeFiles\dcpgpu.dir') -Force).Count -eq 0) 'a toolchain change resets the GPU objects'
    Check (-not (Test-Path -LiteralPath (Join-Path $mock 'gpu\libdcpgpu.a'))) 'a toolchain change resets libdcpgpu.a'
    Check (-not (Test-Path -LiteralPath (Join-Path $mock 'bin\sons_of_sparta.exe'))) 'a toolchain change resets the executable'
    $left = Get-LeftFiles $mock
    $kept = 'bin\dcpgpu_link_smoke.exe', 'bin\assets\launcher\x.png', 'CMakeFiles\sons_of_sparta.dir\src\loader.c.obj',
        'CMakeFiles\dcpgpu_link_smoke.dir\src\gpu_link_smoke.cpp.obj', 'launcher\CMakeFiles\sos_launcher.dir\launcher.cpp.obj',
        'gpu\libother.a', 'generated\sos_version.h', '.ninja_deps', '.ninja_log', 'CMakeCache.txt', 'build.ninja'
    Check (@($kept | Where-Object { $left -notcontains $_ }).Count -eq 0) 'the reset keeps the launcher, smoke target, ninja logs and CMake graph'
    Sync-GpuRuntimeArtifacts $mock $toolchainChange $compiler
    Check $true 'a build tree with no GPU artifacts needs no reset'

    New-MockBuild $mock
    (Get-Item -LiteralPath $compiler).LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(1)
    Sync-GpuRuntimeArtifacts $mock $toolchainChange $compiler
    Check (-not (Test-Path -LiteralPath (Join-Path $mock 'gpu\libdcpgpu.a'))) 'without a stamp, a compiler newer than the artifacts resets them'
    (Get-Item -LiteralPath $compiler).LastWriteTimeUtc = $compilerTime

    $locked = Join-Path $temp 'locked'
    New-MockBuild $locked
    $stream = [IO.File]::Open((Join-Path $locked 'bin\sons_of_sparta.exe'), 'Open', 'ReadWrite', 'None')
    try {
        $threw = $false
        try { Reset-GpuRuntimeArtifacts $locked } catch { $threw = $true }
        Check $threw 'a file that cannot be deleted stops the reset with an error'
    }
    finally { $stream.Dispose() }

    $lockDir = Join-Path $temp 'lockdir'
    $lock = Enter-BuildLock $lockDir
    Exit-BuildLock $lock
    $lock = Enter-BuildLock $lockDir
    Check ($null -ne $lock) 'the build mutex is released on the normal path'
    Exit-BuildLock $lock
    $released = $false
    try { $lock = Enter-BuildLock $lockDir; throw 'failure' }
    catch { }
    finally { if ($lock) { Exit-BuildLock $lock; $released = $true } }
    $lock = Enter-BuildLock $lockDir
    Check ($released -and $null -ne $lock) 'the build mutex is released on a failure path'
    Exit-BuildLock $lock

    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build.ps1'))
    function Position([string]$Pattern) { $m = [regex]::Match($text, $Pattern); if ($m.Success) { $m.Index } else { -1 } }
    $lockPos = Position 'Enter-BuildLock \$buildDir'
    $dependencyPos = Position 'Sync-GpuDependencies'
    $syncPos = Position 'Sync-GpuRuntimeArtifacts \$buildDir \$gpuIdentity \$compiler'
    $configurePos = Position "Write-Step 'Configuring'"
    $ninjaPos = Position "Invoke-Native ninja @\('-C', \`$buildDir, 'sons_of_sparta'\)"
    $stampPos = Position 'Set-GpuRuntimeStamp \$buildDir \$gpuIdentity'
    $unlockPos = Position 'Exit-BuildLock \$buildLock'
    $failedPos = Position "if \(\`$buildResult -ne 0\) \{ Stop-Tool 'Build failed' \}"
    $launchPos = Position '& \$loader @arguments'
    Check (-not $text.Contains('Remove-GpuProductionArtifacts')) 'build.ps1 has no unconditional GPU cleanup'
    Check (-not $text.Contains('Reset-GpuRuntimeArtifacts')) 'build.ps1 does not reset GPU artifacts directly'
    Check ([regex]::Matches($text, 'Sync-GpuRuntimeArtifacts \$buildDir \$gpuIdentity \$compiler').Count -eq 1) 'the GPU identity is validated once'
    Check ($lockPos -ge 0 -and $dependencyPos -gt $lockPos -and $dependencyPos -lt $syncPos) 'dependency fetching and stamping run under the build lock'
    Check ($lockPos -ge 0 -and $syncPos -gt $lockPos -and $syncPos -lt $ninjaPos) 'the GPU validation runs under the build lock before ninja'
    Check ($configurePos -lt 0 -or $syncPos -lt $configurePos) 'the GPU validation runs before a possible configuration'
    Check ($stampPos -gt $ninjaPos -and $unlockPos -gt $stampPos) 'the identity is recorded after a successful build, under the same lock'
    Check (-not [regex]::IsMatch($text, '(?s)if \(\$Run\) \{[^}]*?(Remove-Item|Reset-GpuRuntimeArtifacts)')) '-Run does not delete or reset build artifacts'
    Check (-not [regex]::IsMatch($text, 'Remove-Item[^\r\n]*(libdcpgpu|dcpgpu\.dir|sons_of_sparta\.exe)')) 'build.ps1 does not delete GPU objects, the archive or the executable'
    Check ([regex]::IsMatch($text, '(?s)if \(\$Clean[^\r\n]*\) \{[^}]*Remove-Item -LiteralPath \$buildDir -Recurse -Force')) '-Clean keeps the explicit clean'
    Check ($failedPos -gt $unlockPos -and $launchPos -gt $failedPos) 'a failed build stops before the launch'
    Check ([regex]::IsMatch($text, 'if \(-not \$Run\) \{ return \}') -and $launchPos -gt (Position 'Close-BuildLog 0')) 'the game launches only with -Run, after the build'
    Check ([regex]::Matches($text, 'finally \{ Exit-BuildLock \$buildLock \}').Count -eq 1) 'the build mutex is released in a finally block'
    Check ([regex]::IsMatch($text, "\`$config = 'RelWithDebInfo'")) 'the build configuration remains RelWithDebInfo'
    Check ([regex]::IsMatch($text, '\$compiler = Join-Path \$tc\.Bin ''clang\+\+\.exe''') -and
           [regex]::IsMatch($text, 'Get-GpuRuntimeIdentity \$config \(Get-CompilerIdentity \$compiler\)')) 'the runtime identity uses the configured toolchain compiler and configuration'

    $common = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '_common.ps1'))
    Check ([regex]::IsMatch($common, 'if \(Test-PinnedDependency \$dir \$stamp \$expected\) \{ continue \}')) 'the dependency sync validates an existing checkout against its pin and patch stamp'
    Check (-not [regex]::IsMatch($common, 'Count -gt 0\) \{ continue \}')) 'an existing dependency directory no longer skips pin and patch validation'
    Check ([regex]::IsMatch($common, 'if \(\$patches\.Count -eq 0 -and \(Test-PinnedCheckout \$dir \$commit\)\)')) 'an unpatched clean checkout is accepted at its pin without re-fetching'

    $cmake = [IO.File]::ReadAllText((Join-Path $root 'gpu\CMakeLists.txt'))
    Check ([regex]::IsMatch($cmake, '(?s)\$\{GLSLANG_COMPILER\}\s*\n\s*\$\{SHADER_SRC\}/AddShaderHeaderInclude\.cmake VERBATIM')) 'generated shader headers depend on the shader compiler and header script'
}
finally { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }

if ($failures) { exit 1 }
Write-Host '  -Run and plain builds stay incremental; the runtime stamp resets GPU artifacts only for changed inputs'
exit 0
