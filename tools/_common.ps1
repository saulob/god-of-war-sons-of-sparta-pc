# Shared helpers, dot-sourced by the other tools. Works in Windows PowerShell 5.1 and PowerShell 7.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:ProjectSlug = 'sons_of_sparta'
$script:BuildLog = $null

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan; Write-BuildLogLine "==> $Text" }
function Write-Ok([string]$Text)   { Write-Host "  [ok]   $Text" -ForegroundColor Green; Write-BuildLogLine "  [ok]   $Text" }
function Write-Warn([string]$Text) { Write-Host "  [warn] $Text" -ForegroundColor Yellow; Write-BuildLogLine "  [warn] $Text" }
function Write-Bad([string]$Text)  { Write-Host "  [miss] $Text" -ForegroundColor Red; Write-BuildLogLine "  [miss] $Text" }

function Stop-Tool([string]$Text) {
    Write-Host "error: $Text" -ForegroundColor Red
    Write-BuildLogLine "error: $Text"
    Close-BuildLog 1
    exit 1
}

# Where a build logs. A build alone writes build\logs\build\build_NNN.log; a build that runs writes the session log,
# build\logs\sons_of_sparta_NNN.log, which the loader then continues, so one invocation has one log.
function Get-BuildLogPlan([switch]$Run) {
    if ($Run) { return @{ Directory = (Join-Path $script:RepoRoot 'build\logs'); Name = $script:ProjectSlug; Session = $true } }
    return @{ Directory = (Join-Path $script:RepoRoot 'build\logs\build'); Name = 'build'; Session = $false }
}

# The loader continues the session log given with --log; arguments that already choose the log are kept as they are.
function Get-SessionLoaderArgs([string]$SessionLog, [string[]]$Arguments = @()) {
    if ($Arguments -contains '--log' -or $Arguments -contains '--logs') { return $Arguments }
    return @('--log', $SessionLog) + $Arguments
}

# After the run: the runtime deletes its log after a clean exit when detailed logging is off, so the build section and
# the exit code are written back before the session ends.
function Complete-SessionLog([string]$Path, [string]$BuildSection, [int]$ExitCode) {
    if (-not (Test-Path -LiteralPath $Path)) {
        [IO.File]::WriteAllText($Path, $BuildSection, (New-Object Text.UTF8Encoding($false)))
    }
    Add-Content -LiteralPath $Path -Value "[run] sons_of_sparta.exe exit code $ExitCode"
}

# Reserves <name>_NNN.log with an exclusive create, so concurrent builds never share a number and an existing log is
# never reopened. A session log is numbered from the greater of the folder and build\.sons_of_sparta_log_sequence, so
# deleting build\logs never reuses a number; -Session starts the log with a [build] line.
function Start-BuildLog([string]$Mode, [string]$Configuration,
                        [string]$Directory = (Join-Path $script:RepoRoot 'build\logs\build'),
                        [string]$Name = 'build', [switch]$Session) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $pattern = '^' + [regex]::Escape($Name) + '_(\d+)\.log$'
    $sequenceFile = Join-Path $script:RepoRoot 'build\.sons_of_sparta_log_sequence'
    $stream = $null
    $number = 0
    for ($attempt = 0; $attempt -lt 1000 -and -not $stream; $attempt++) {
        $last = 0
        foreach ($file in [IO.Directory]::GetFiles($Directory)) {
            if ([IO.Path]::GetFileName($file) -match $pattern) {
                $found = [int64]$Matches[1]
                if ($found -gt $last) { $last = $found }
            }
        }
        if ($Session -and (Test-Path -LiteralPath $sequenceFile)) {
            $stored = [int64]0
            if ([int64]::TryParse((Get-Content -LiteralPath $sequenceFile -Raw).Trim(), [ref]$stored) -and $stored -gt $last) { $last = $stored }
        }
        $number = $last + 1
        $path = Join-Path $Directory ('{0}_{1:D3}.log' -f $Name, $number)
        try { $stream = New-Object IO.FileStream($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read) }
        catch [IO.IOException] { $stream = $null }
    }
    if (-not $stream) { throw "Could not reserve a build log in $Directory" }
    if ($Session) {
        New-Item -ItemType Directory -Force -Path (Split-Path $sequenceFile -Parent) | Out-Null
        [IO.File]::WriteAllText($sequenceFile, "$number", (New-Object Text.UTF8Encoding($false)))
    }
    $writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    $script:BuildLog = @{ Path = $path; Writer = $writer; Started = Get-Date; Warnings = 0; Errors = 0 }
    if ($Session) { $writer.WriteLine('[build]') }
    $writer.WriteLine("Project: $(if ($Session) { $script:ProjectSlug } else { 'build' })")
    $writer.WriteLine("Started: $($script:BuildLog.Started.ToString('yyyy-MM-dd HH:mm:ss zzz'))")
    $writer.WriteLine("Configuration: $Configuration")
    $writer.WriteLine("Mode: $Mode")
    $writer.WriteLine('')
    Write-Host "$(if ($Session) { 'Session log' } else { 'Build log' }): $path"
    return $path
}

function Write-BuildLogLine([string]$Line) {
    if (-not $script:BuildLog) { return }
    if ($Line -match '(?i)\bwarning\b:') { $script:BuildLog.Warnings++ }
    if ($Line -match '(?i)(\berror\b:|^FAILED:)') { $script:BuildLog.Errors++ }
    $script:BuildLog.Writer.WriteLine($Line)
}

# Idempotent; the exit code is recorded as given and never changed.
function Close-BuildLog([int]$ExitCode) {
    $log = $script:BuildLog
    if (-not $log) { return }
    $script:BuildLog = $null
    $finished = Get-Date
    $log.Writer.WriteLine('')
    $log.Writer.WriteLine("Warnings: $($log.Warnings)")
    $log.Writer.WriteLine("Errors: $($log.Errors)")
    $log.Writer.WriteLine("Result: $(if ($ExitCode -eq 0) { 'SUCCESS' } else { 'FAILED' })")
    $log.Writer.WriteLine("Exit code: $ExitCode")
    $log.Writer.WriteLine("Finished: $($finished.ToString('yyyy-MM-dd HH:mm:ss zzz')) ($([int]($finished - $log.Started).TotalSeconds) s)")
    $log.Writer.Dispose()
}

# Console and build log, line by line.
filter Out-BuildLog {
    $line = "$_"
    Write-Host $line
    Write-BuildLogLine $line
}

# Serialize Ninja builds that share build\cmake\clang64 (tools\build.ps1 and test helpers).
function Enter-BuildLock([string]$BuildDir) {
    $key = [BitConverter]::ToString([Security.Cryptography.SHA1]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($BuildDir.ToLowerInvariant()))).Replace('-', '')
    $mutex = New-Object System.Threading.Mutex($false, "Global\sos_build_$key")
    try { $acquired = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) {
        Write-Host '  Another build of this directory is running; waiting for it to finish...'
        try { $acquired = $mutex.WaitOne([TimeSpan]::FromMinutes(30)) } catch [System.Threading.AbandonedMutexException] { $acquired = $true }
    }
    if (-not $acquired) { Stop-Tool 'Another build of this directory did not finish within 30 minutes.' }
    return $mutex
}

function Exit-BuildLock($Mutex) {
    if ($Mutex) { $Mutex.ReleaseMutex(); $Mutex.Dispose() }
}

# A pinned dependency is current only when the checkout is present and its stamp records the expected commit and patch
# fingerprint. The directory alone is never enough: a changed pin or patch must fetch it again.
function Test-PinnedDependency([string]$Directory, [string]$Stamp, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Directory)) { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $Directory '.git'))) { return $false }
    if (@(Get-ChildItem -LiteralPath $Directory -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notin '.git', '.sos_commit' }).Count -eq 0) { return $false }
    return (Test-Path -LiteralPath $Stamp) -and
        (((Get-Content -LiteralPath $Stamp -Raw).Trim() -replace "`r`n", "`n") -eq $Expected)
}

# An unpatched checkout is current when it is clean at the pinned commit. This also accepts a checkout fetched before
# the stamp existed without re-fetching it, which would rebuild all of its consumers for no source change.
function Test-PinnedCheckout([string]$Directory, [string]$Commit) {
    if (-not (Test-Path -LiteralPath (Join-Path $Directory '.git'))) { return $false }
    if (Test-Path -LiteralPath (Join-Path $Directory '.gitmodules')) { return $false }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $head = (& git -C $Directory rev-parse HEAD 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $head -ne $Commit) { return $false }
        $status = (& git -C $Directory status --porcelain --untracked-files=no 2>$null | Out-String).Trim()
        return ($LASTEXITCODE -eq 0 -and -not $status)
    }
    finally { $ErrorActionPreference = $previous }
}

function Get-ContentSha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($Path))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-StringSha256([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-', '') }
    finally { $sha.Dispose() }
}

# The GPU inputs Ninja does not track, split by what they invalidate: the toolchain (compiler, configuration, FFmpeg)
# needs every GPU object recompiled; a dependency pin or patch only needs Ninja's affected consumers recompiled from
# the re-fetched sources. Sources, headers, shaders and flags stay in Ninja's dependency graph.
function Get-GpuRuntimeIdentity([string]$Configuration, [string]$CompilerIdentity, [string]$GpuRoot) {
    $toolchain = "config=$Configuration`ncompiler=$CompilerIdentity`n" +
        'ffmpeg=' + (Get-ContentSha256 (Join-Path $GpuRoot 'ffmpeg.txt'))
    $dependencies = New-Object Text.StringBuilder
    foreach ($line in Get-Content -LiteralPath (Join-Path $GpuRoot 'deps.txt')) {
        if ($line -match '^\s*(#|$)') { continue }
        $path, $url, $commit = -split $line
        # Dependencies outside gpu\ (LibAtrac9, trg-launcher) are not part of the video core.
        if ($path -match '^\.\.') { continue }
        $patchDir = Join-Path $GpuRoot ('patches\' + (Split-Path $path -Leaf))
        $patches = @(Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
        [void]$dependencies.Append("dep=$path@$commit patches=").Append((Get-PatchFingerprint $patches)).Append("`n")
    }
    return "toolchain=$(Get-StringSha256 $toolchain)`ndependencies=$(Get-StringSha256 $dependencies.ToString())"
}

function Get-GpuIdentityPart([string]$Identity, [string]$Name) {
    $match = [regex]::Match($Identity, "(?m)^$Name=([0-9A-F]{64})$")
    if ($match.Success) { return $match.Groups[1].Value }
    return ''
}

function Get-CompilerIdentity([string]$CompilerPath) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = (& $CompilerPath --version 2>&1 | Out-String).Trim() }
    finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0 -or -not $text) { Stop-Tool "clang++ --version failed ($CompilerPath)" }
    return $text
}

function Get-GpuRuntimeStampPath([string]$BuildDir) { return (Join-Path $BuildDir 'gpu\.sos_runtime_identity') }

function Read-GpuRuntimeStamp([string]$BuildDir) {
    $path = Get-GpuRuntimeStampPath $BuildDir
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    return ((Get-Content -LiteralPath $path -Raw).Trim() -replace "`r`n", "`n")
}

# Written only when the identity changes, so an unchanged build leaves the artifact times alone.
function Set-GpuRuntimeStamp([string]$BuildDir, [string]$Identity) {
    if ((Read-GpuRuntimeStamp $BuildDir) -eq $Identity) { return }
    $path = Get-GpuRuntimeStampPath $BuildDir
    New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllText($path, $Identity, [Text.Encoding]::ASCII)
}

# The GPU objects, archive and executable built from other inputs. A file that cannot be deleted (a running game)
# throws; the build never goes on to launch old artifacts.
function Reset-GpuRuntimeArtifacts([string]$BuildDir) {
    $objects = Join-Path $BuildDir 'gpu\CMakeFiles\dcpgpu.dir'
    if (Test-Path -LiteralPath $objects) {
        Get-ChildItem -LiteralPath $objects -Force | Remove-Item -Recurse -Force -ErrorAction Stop
    }
    foreach ($file in (Join-Path $BuildDir 'gpu\libdcpgpu.a'), (Join-Path $BuildDir 'bin\sons_of_sparta.exe')) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force -ErrorAction Stop }
    }
}

# A changed toolchain makes every GPU object unusable; a dependency-only change is rebuilt by Ninja from the sources
# that Sync-GpuDependencies re-fetched. Without a stamp, existing artifacts are accepted only when the compiler is not
# newer than all of them.
function Sync-GpuRuntimeArtifacts([string]$BuildDir, [string]$Identity, [string]$CompilerPath) {
    $stamp = Read-GpuRuntimeStamp $BuildDir
    if ($stamp -eq $Identity) { return }
    $objects = Join-Path $BuildDir 'gpu\CMakeFiles\dcpgpu.dir'
    $artifacts = @()
    if (Test-Path -LiteralPath $objects) { $artifacts += @(Get-ChildItem -LiteralPath $objects -Recurse -File) }
    foreach ($file in (Join-Path $BuildDir 'gpu\libdcpgpu.a'), (Join-Path $BuildDir 'bin\sons_of_sparta.exe')) {
        if (Test-Path -LiteralPath $file) { $artifacts += Get-Item -LiteralPath $file }
    }
    if ($artifacts.Count -eq 0) { return }
    if ($stamp) {
        $stampToolchain = Get-GpuIdentityPart $stamp 'toolchain'
        if ($stampToolchain -and $stampToolchain -eq (Get-GpuIdentityPart $Identity 'toolchain')) { return }
    }
    else {
        $compiler = Get-Item -LiteralPath $CompilerPath -ErrorAction SilentlyContinue
        $newest = @($artifacts | ForEach-Object { $_.LastWriteTimeUtc } | Sort-Object -Descending)[0]
        if ($compiler -and $newest -and $compiler.LastWriteTimeUtc -le $newest) { return }
    }
    Write-Step 'GPU runtime inputs changed: rebuilding the GPU artifacts'
    Reset-GpuRuntimeArtifacts $BuildDir
}

# Runs a native tool with stdout and stderr merged into the console and the build log. The tool's exit code stays in
# $LASTEXITCODE for the caller; nothing here changes it.
function Invoke-Native([string]$File, [string[]]$Arguments = @()) {
    Write-BuildLogLine ("$ $File " + ($Arguments -join ' '))
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $File @Arguments 2>&1 | Out-BuildLog }
    finally { $ErrorActionPreference = $previous }
}

# A -D define whose value is a C string, in the form the compiler receives it. Windows PowerShell 5.1 and the legacy
# argument mode drop embedded quotes unless they are escaped; PowerShell 7.3 and later escape them itself.
function Get-StringDefine([string]$Name, [string]$Value) {
    $mode = Get-Variable -Name PSNativeCommandArgumentPassing -ValueOnly -ErrorAction SilentlyContinue
    $escape = $PSVersionTable.PSVersion -lt [version]'7.3' -or "$mode" -eq 'Legacy'
    $quote = if ($escape) { '\"' } else { '"' }
    return "-D$Name=$quote$Value$quote"
}

function Get-VenvPython {
    $path = Join-Path $script:RepoRoot '.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $path) { return $path }
    return $null
}

# Interpreter for the scripts: the project venv, else the py launcher, else the first python on PATH.
function Get-Python {
    $venv = Get-VenvPython
    if ($venv) { return @{ Exe = $venv; Args = @() } }
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) { return @{ Exe = $launcher.Source; Args = @('-3') } }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { return @{ Exe = $python.Source; Args = @() } }
    return $null
}

# Runs scripts/<name> with isolated-mode Python so nothing next to the data is imported.
function Invoke-Script([string]$Name, [string[]]$Arguments = @()) {
    $python = Get-Python
    if (-not $python) { Stop-Tool 'Python 3 not found. Run tools\setup.ps1.' }
    $script = Join-Path $script:RepoRoot "scripts\$Name"
    & $python.Exe @($python.Args) '-I' $script @Arguments | Out-Host
    return $LASTEXITCODE
}

function Test-PythonModule([string]$Module) {
    $python = Get-Python
    if (-not $python) { return $false }
    & $python.Exe @($python.Args) '-I' '-c' "import $Module" 2>$null
    return ($LASTEXITCODE -eq 0)
}

function Get-Msys2Root {
    $candidates = @($env:MSYS2_ROOT, 'C:\msys64', 'D:\msys64') | Where-Object { $_ }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'usr\bin\pacman.exe')) { return $candidate }
    }
    return $null
}

# The only supported toolchain: MSYS2 CLANG64, as in the reference ports.
$script:Toolchains = [ordered]@{
    clang64 = @{ Dir = 'clang64'; Compiler = 'clang.exe'; Package = 'mingw-w64-clang-x86_64' }
}
# The milestone loader/runtime needs only the compiler, CMake and Ninja (no SDL3/Vulkan yet).
$script:BuildTools = @('clang.exe', 'clang++.exe', 'cmake.exe', 'ninja.exe', 'pkgconf.exe', 'SDL3.dll', 'glslang.exe', 'vulkan-1.dll', 'libfreetype-6.dll')

function Get-ToolchainStatus([string]$Name) {
    $info = $script:Toolchains[$Name]
    $root = Get-Msys2Root
    $status = @{ Name = $Name; Ready = $false; Bin = $null; Missing = @() }
    if (-not $root) { $status.Missing = @('MSYS2'); return $status }
    $bin = Join-Path $root "$($info.Dir)\bin"
    $status.Bin = $bin
    foreach ($tool in $script:BuildTools) {
        if (-not (Test-Path -LiteralPath (Join-Path $bin $tool))) { $status.Missing += $tool }
    }
    $status.Ready = ($status.Missing.Count -eq 0)
    return $status
}

function Resolve-Toolchain([string]$Requested) {
    if ($Requested -and $Requested -ne 'auto') {
        $status = Get-ToolchainStatus $Requested
        if (-not $status.Ready) { Stop-Tool "Toolchain '$Requested' is incomplete (missing: $($status.Missing -join ', ')). Run tools\setup.ps1." }
        return $status
    }
    foreach ($name in $script:Toolchains.Keys) {
        $status = Get-ToolchainStatus $name
        if ($status.Ready) { return $status }
    }
    Stop-Tool 'MSYS2 CLANG64 (clang, cmake, ninja) not found. Run tools\setup.ps1 for what to install.'
}

# Content fingerprint of a dependency's patch set: empty when it has none.
function Get-PatchFingerprint($files) {
    if (-not $files -or $files.Count -eq 0) { return '' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $text = New-Object System.Text.StringBuilder
        foreach ($file in $files) {
            $hex = [System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($file.FullName))).Replace('-', '')
            [void]$text.Append($file.Name).Append(':').Append($hex).Append("`n")
        }
        $hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text.ToString()))
        return [System.BitConverter]::ToString($hash).Replace('-', '')
    } finally { $sha.Dispose() }
}

# Fetches the pinned sources in gpu\deps.txt missing, at another commit or with another patch set. The stamp holds the
# commit and, for a patched dependency, its patch fingerprint; an unpatched clean checkout at the pin is stamped without
# re-fetching, so its consumers keep their objects.
function Sync-GpuDependencies {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { Stop-Tool 'git is needed to fetch the video core dependencies (gpu\deps.txt).' }
    $gpu = Join-Path $script:RepoRoot 'gpu'
    foreach ($line in Get-Content -LiteralPath (Join-Path $gpu 'deps.txt')) {
        if ($line -match '^\s*(#|$)') { continue }
        $path, $url, $commit = -split $line
        $dir = Join-Path $gpu $path
        $stamp = Join-Path $dir '.sos_commit'
        $patch_dir = Join-Path $gpu ('patches\' + (Split-Path $path -Leaf))
        $patches = @(Get-ChildItem -LiteralPath $patch_dir -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
        $fingerprint = Get-PatchFingerprint $patches
        $expected = if ($fingerprint) { "$commit`n$fingerprint" } else { $commit }
        if (Test-PinnedDependency $dir $stamp $expected) { continue }
        if ($patches.Count -eq 0 -and (Test-PinnedCheckout $dir $commit)) {
            [System.IO.File]::WriteAllText($stamp, $expected, [System.Text.Encoding]::ASCII)
            continue
        }
        Write-Step "Fetching $path ($($commit.Substring(0, 7)))"
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        & git -C $dir init --quiet
        & git -C $dir fetch --quiet --depth 1 $url $commit
        if ($LASTEXITCODE -ne 0) { Stop-Tool "git fetch failed for $url" }
        & git -C $dir -c advice.detachedHead=false checkout --quiet FETCH_HEAD
        if (Test-Path -LiteralPath (Join-Path $dir '.gitmodules')) {
            & git -C $dir submodule update --quiet --init --recursive --depth 1
            if ($LASTEXITCODE -ne 0) { Stop-Tool "submodules of $path failed" }
        }
        foreach ($patch in $patches) {
            & git -C $dir apply $patch.FullName
            if ($LASTEXITCODE -ne 0) { Stop-Tool "patch $($patch.Name) does not apply to $path" }
        }
        [System.IO.File]::WriteAllText($stamp, $expected, [System.Text.Encoding]::ASCII)
    }
}

# FFmpeg for AvPlayer is built from gpu\ffmpeg.txt by tools\build-ffmpeg.sh (LGPL, the game's codecs only), never taken
# from MSYS2, whose GPL-3.0-or-later build cannot ship with this GPL-2.0-only program.
function Read-FFmpegPin {
    $pin = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $script:RepoRoot 'gpu\ffmpeg.txt')) {
        if ($line -match '^\s*(#|$)') { continue }
        $key, $value = $line -split '\s+', 2
        $pin[$key] = $value
    }
    foreach ($key in 'version', 'url', 'sha256', 'configure') { if (-not $pin[$key]) { throw "gpu\ffmpeg.txt has no $key" } }
    return $pin
}

function Get-FFmpegPrefix { Join-Path $script:RepoRoot "cache\deps\ffmpeg\$((Read-FFmpegPin).version)\clang64" }

# Builds FFmpeg in MSYS2 CLANG64 when the prefix does not hold the current pin; returns the prefix.
function Sync-FFmpeg {
    $prefix = Get-FFmpegPrefix
    $work = Join-Path (Split-Path $prefix -Parent) 'work'
    $bash = Join-Path (Get-Msys2Root) 'usr\bin\bash.exe'
    if (-not (Test-Path -LiteralPath $bash)) { Stop-Tool 'MSYS2 bash is needed to build FFmpeg (tools\setup.ps1)' }
    $saved = @{ MSYSTEM = $env:MSYSTEM; CHERE_INVOKING = $env:CHERE_INVOKING }
    $env:MSYSTEM = 'CLANG64'
    $env:CHERE_INVOKING = '1'
    Push-Location $script:RepoRoot
    try {
        $command = "bash tools/build-ffmpeg.sh '$($prefix.Replace('\', '/'))' '$($work.Replace('\', '/'))'"
        & $bash -lc $command 2>&1 | ForEach-Object { Write-Host "  $_"; Write-BuildLogLine "  $_" }
        if ($LASTEXITCODE -ne 0) { Stop-Tool 'FFmpeg could not be built (tools\build-ffmpeg.sh, gpu\ffmpeg.txt)' }
    } finally {
        Pop-Location
        foreach ($name in $saved.Keys) {
            if ($null -eq $saved[$name]) { Remove-Item "env:$name" -ErrorAction SilentlyContinue } else { Set-Item "env:$name" $saved[$name] }
        }
    }
    return $prefix
}

# An FFmpeg DLL is the LGPL build: its licence string says so and its configuration has no --enable-gpl.
function Test-LgplFFmpegDll([string]$Path) {
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($Path))
    return $text.Contains('LGPL version 2.1 or later') -and -not $text.Contains('--enable-gpl')
}

# The FFmpeg DLLs sit beside the loader, as in a release; the build installs them from the pinned prefix.
function Install-FFmpegRuntime([string]$Prefix, [string]$Directory) {
    foreach ($dll in Get-ChildItem -LiteralPath (Join-Path $Prefix 'bin') -Filter '*.dll' -File) {
        if (-not (Test-LgplFFmpegDll $dll.FullName)) { Stop-Tool "$($dll.Name) is not the LGPL FFmpeg of gpu\ffmpeg.txt" }
        $target = Join-Path $Directory $dll.Name
        if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $dll.FullName).Hash) {
            Copy-Item -LiteralPath $dll.FullName -Destination $target -Force
        }
    }
}
