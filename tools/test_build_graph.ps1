# Dependency correctness of the production build graph (target sons_of_sparta in build\cmake\clang64): a source or header
# change must recompile exactly its consumers, update libdcpgpu.a and relink sons_of_sparta.exe, and an unchanged build must do
# nothing. A touched file keeps its content (only its modification time moves, and is restored), so the rebuilt objects equal
# the current ones. Headless: no game, launcher, window or audio. Needs the product built once; builds only what is stale.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_gpu_build.ps1')

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$buildDir = $script:GpuBuildDir
$library = $script:GpuLibrary
$loader = Join-Path $buildDir 'bin\sons_of_sparta.exe'
$failures = 0

function Fail([string]$Text) { Write-Host "  [fail] $Text"; $script:failures++ }
function Pass([string]$Text) { Write-Host "  ok    $Text" }

function Invoke-Ninja([string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = (& ninja -C $buildDir @Arguments 2>&1 | Out-String) } finally { $ErrorActionPreference = $previous }
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
}

function Get-CompiledObjects([string]$Text) {
    @([regex]::Matches($Text, '(?m)^\[\d+/\d+\] Building (?:C|CXX) object (\S+)') | ForEach-Object { $_.Groups[1].Value -replace '\\', '/' } | Sort-Object)
}

# Objects of production sources whose include closure (quoted includes, by file name) contains the file.
function Get-ExpectedObjects([string]$Changed) {
    $scanRoots = @('gpu\shim', 'gpu\shadps4', 'src') | ForEach-Object { Join-Path $root $_ }
    $files = @(Get-ChildItem -LiteralPath $scanRoots -Recurse -File -Include *.h, *.hpp, *.inl, *.cpp, *.c)
    $includers = @{}
    foreach ($file in $files) {
        foreach ($match in [regex]::Matches([IO.File]::ReadAllText($file.FullName), '(?m)^\s*#\s*include\s*"([^"]+)"')) {
            $name = [IO.Path]::GetFileName($match.Groups[1].Value)
            if (-not $includers.ContainsKey($name)) { $includers[$name] = New-Object System.Collections.Generic.List[object] }
            $includers[$name].Add($file)
        }
    }
    $reached = New-Object System.Collections.Generic.HashSet[string]
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue([IO.Path]::GetFileName($Changed))
    $sources = New-Object System.Collections.Generic.HashSet[string]
    while ($queue.Count) {
        $name = $queue.Dequeue()
        if (-not $reached.Add($name) -or -not $includers.ContainsKey($name)) { continue }
        foreach ($file in $includers[$name]) {
            if ($file.Extension -in '.cpp', '.c') { [void]$sources.Add($file.FullName) } else { $queue.Enqueue($file.Name) }
        }
    }
    if ($Changed -match '\.(cpp|c)$') { [void]$sources.Add($Changed) }
    $gpuRoot = (Join-Path $root 'gpu') + '\'
    $objects = foreach ($source in $sources) {
        if ($source.StartsWith($gpuRoot)) { 'gpu/CMakeFiles/dcpgpu.dir/' + $source.Substring($gpuRoot.Length).Replace('\', '/') + '.obj' }
    }
    $known = @(Invoke-Ninja @('-t', 'targets', 'all')).Text -split "`n" | ForEach-Object { ($_ -split ':')[0] }
    return @($objects | Where-Object { $known -contains $_ } | Sort-Object)
}

function Test-SameSet([string[]]$Actual, [string[]]$Expected) {
    return ((@($Actual) -join '|') -eq (@($Expected) -join '|'))
}

$running = @(Get-Process sons_of_sparta -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $loader })
if ($running.Count) { Write-Host '  [fail] sons_of_sparta.exe is running; close it first'; exit 1 }

$lock = Enter-BuildLock $buildDir
try {
    if (-not (Initialize-GpuBuildDirectory)) { Write-Host '  [fail] CMake configuration failed'; exit 1 }
    $first = Invoke-Ninja @('sons_of_sparta')
    if ($first.Code -ne 0) { Write-Host $first.Text; Write-Host '  [fail] sons_of_sparta does not build'; exit 1 }

    $again = Invoke-Ninja @('sons_of_sparta')
    if ($again.Code -eq 0 -and $again.Text -match 'no work to do' -and -not @(Get-CompiledObjects $again.Text).Count) { Pass 'an unchanged build recompiles nothing' }
    else { Fail "an unchanged build did work:`n$($again.Text)" }

    $inputs = (Invoke-Ninja @('-t', 'inputs', 'bin/sons_of_sparta.exe')).Text -replace '\\', '/'
    $archive = (Invoke-Ninja @('-t', 'inputs', 'gpu/libdcpgpu.a')).Text -replace '\\', '/'
    if ($inputs -match 'gpu/libdcpgpu\.a' -and $inputs -match 'dcpgpu\.dir/shim/window\.cpp\.obj') { Pass 'sons_of_sparta.exe depends on libdcpgpu.a and, through it, on every dcpgpu object' }
    else { Fail 'sons_of_sparta.exe does not depend on libdcpgpu.a and its objects' }
    if ($archive -match 'vk_frame_capture\.cpp\.obj' -and $archive -match 'vk_rasterizer\.cpp\.obj') { Pass 'libdcpgpu.a depends on the video core objects' }
    else { Fail 'libdcpgpu.a does not depend on the video core objects' }
    $smoke = (Invoke-Ninja @('-t', 'inputs', 'bin/dcpgpu_link_smoke.exe')).Text -replace '\\', '/'
    $shared = @(($inputs -split "`n") | Where-Object { $_ -match '\.obj$' -and $smoke.Contains($_) -and $_ -notmatch '^(gpu/CMakeFiles/dcpgpu\.dir|launcher/)' })
    if ($shared.Count -eq 0) { Pass 'the link smoke target shares only the dcpgpu and launcher objects with production' }
    else { Fail "the link smoke target shares objects with production: $($shared -join ', ')" }

    $gpu = Join-Path $root 'gpu'
    $cases = @(
        @{ File = Join-Path $gpu 'shadps4\video_core\renderer_vulkan\vk_frame_capture.cpp'; What = 'vk_frame_capture.cpp' },
        @{ File = Join-Path $gpu 'shadps4\video_core\renderer_vulkan\vk_frame_capture.h'; What = 'vk_frame_capture.h' },
        @{ File = Join-Path $gpu 'shim\dcp_capture_window.h'; What = 'dcp_capture_window.h' },
        @{ File = Join-Path $gpu 'shim\dcp_f5_capture.h'; What = 'dcp_f5_capture.h' }
    )
    foreach ($case in $cases) {
        $item = Get-Item -LiteralPath $case.File
        $original = $item.LastWriteTimeUtc
        $expected = @(Get-ExpectedObjects $case.File)
        $libBefore = (Get-Item -LiteralPath $library).LastWriteTimeUtc
        $exeBefore = (Get-Item -LiteralPath $loader).LastWriteTimeUtc
        try {
            Start-Sleep -Milliseconds 50
            $item.LastWriteTimeUtc = [DateTime]::UtcNow
            $build = Invoke-Ninja @('sons_of_sparta')
        }
        finally { $item.LastWriteTimeUtc = $original }
        if ($build.Code -ne 0) { Write-Host $build.Text; Fail "$($case.What): the rebuild failed"; continue }
        $compiled = @(Get-CompiledObjects $build.Text | Where-Object { $_ -like 'gpu/CMakeFiles/dcpgpu.dir/*' })
        if ($expected.Count -and (Test-SameSet $compiled $expected)) { Pass "$($case.What): recompiled its $($expected.Count) consumer(s): $((($expected | ForEach-Object { [IO.Path]::GetFileName($_) }) -join ', '))" }
        else { Fail "$($case.What): recompiled [$($compiled -join ', ')], expected [$($expected -join ', ')]" }
        if ((Get-Item -LiteralPath $library).LastWriteTimeUtc -gt $libBefore) { Pass "$($case.What): libdcpgpu.a was rebuilt" } else { Fail "$($case.What): libdcpgpu.a was not rebuilt" }
        if ((Get-Item -LiteralPath $loader).LastWriteTimeUtc -gt $exeBefore) { Pass "$($case.What): sons_of_sparta.exe was relinked" } else { Fail "$($case.What): sons_of_sparta.exe was not relinked" }
        $after = Invoke-Ninja @('sons_of_sparta')
        if ($after.Code -eq 0 -and $after.Text -match 'no work to do') { Pass "$($case.What): a second build does no work" }
        else { Fail "$($case.What): a second build did work:`n$($after.Text)" }
    }
}
finally { Exit-BuildLock $lock }

if ($failures) { exit 1 }
Write-Host '  production build graph: consumers recompile, library and executable relink, unchanged builds are no-ops'
exit 0
