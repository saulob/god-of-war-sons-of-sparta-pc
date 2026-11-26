# The AppImage's library policy (tools/package-linux.sh is_host_lib), run as written under WSL bash: the host's glibc,
# Vulkan loader, GL/EGL, display server and driver stacks (Mesa RADV on a Steam Deck, NVIDIA, VA-API, VDPAU, OpenCL) are
# never bundled, so the system's graphics drivers are always the ones used; ordinary libraries are bundled, including
# libselinux, libpcre2 and libcap, which SteamOS (Arch) may lack. Mutated copies of the function must fail. Reports as not
# applicable without WSL bash. Nothing is packaged.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$failures = 0
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$probe = if ($wsl) { (& wsl.exe -e sh -c 'command -v bash >/dev/null 2>&1 && echo bash' 2>$null) -join '' } else { '' }
if ($probe.Trim() -ne 'bash') {
    Write-Host '  [n/a]  AppImage library policy: this host has no WSL bash (not applicable, not a failure)'
    exit 0
}

$cases = [ordered]@{
    'libc.so.6' = 'host'; 'libm.so.6' = 'host'; 'libmvec.so.1' = 'host'; 'libnss_files.so.2' = 'host'
    'libvulkan.so.1' = 'host'; 'libGL.so.1' = 'host'; 'libEGL.so.1' = 'host'; 'libOpenGL.so.0' = 'host'; 'libdrm.so.2' = 'host'
    'libgbm.so.1' = 'host'; 'libwayland-client.so.0' = 'host'; 'libX11.so.6' = 'host'; 'libxcb.so.1' = 'host'
    'libXrender.so.1' = 'host'; 'libva.so.2' = 'host'; 'libva-drm.so.2' = 'host'; 'libvdpau.so.1' = 'host'
    'libnvidia-glcore.so.550.54' = 'host'; 'libcuda.so.1' = 'host'; 'libOpenCL.so.1' = 'host'; 'libasound.so.2' = 'host'
    'libpipewire-0.3.so.0' = 'host'; 'libudev.so.1' = 'host'
    'libSDL3.so.0' = 'bundle'; 'libavcodec.so.61' = 'bundle'; 'libfreetype.so.6' = 'bundle'; 'libstdc++.so.6' = 'bundle'
    'libselinux.so.1' = 'bundle'; 'libpcre2-8.so.0' = 'bundle'; 'libcap.so.2' = 'bundle'; 'libssl.so.3' = 'bundle'
}

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_appimage_libs_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

function Convert-ToWslPath([string]$Path) {
    return ((& wsl.exe -e wslpath -a ($Path.Replace('\', '/')) 2>$null) | Select-Object -First 1).Trim()
}

function Invoke-Policy([string]$Script) {
    $names = ($cases.Keys | ForEach-Object { "'$_'" }) -join ' '
    $runner = Join-Path $temp ('run_' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.sh')
    $body = @"
set -e
eval "`$(sed -n '/^is_host_lib() {/,/^}/p' '$(Convert-ToWslPath $Script)')"
for name in $names; do
    if is_host_lib "`$name"; then echo "`$name host"; else echo "`$name bundle"; fi
done
"@
    [IO.File]::WriteAllText($runner, $body.Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
    $output = & wsl.exe --exec bash (Convert-ToWslPath $runner) 2>&1
    $answers = @{}
    foreach ($line in $output) {
        $parts = "$line".Trim() -split ' '
        if ($parts.Count -eq 2) { $answers[$parts[0]] = $parts[1] }
    }
    return $answers
}

function Test-Policy([hashtable]$Answers, [switch]$Quiet) {
    $bad = 0
    foreach ($name in $cases.Keys) {
        $ok = $Answers[$name] -eq $cases[$name]
        if (-not $ok) { $bad++ }
        if (-not $Quiet) {
            $state = if ($ok) { 'ok' } else { 'fail' }
            Write-Host ("  [{0,-4}] {1} is {2} (expected {3})" -f $state, $name, $Answers[$name], $cases[$name])
        }
    }
    return $bad
}

try {
    $script = Join-Path $root 'tools\package-linux.sh'
    $failures += Test-Policy (Invoke-Policy $script)
    Write-Host "AppImage library policy: $($cases.Count) libraries checked"

    $mutations = @(
        @{ Name = 'libselinux never bundled again (breaks SteamOS)'; Old = 'libmvec.so.*|libnss_*.so.*)'; New = 'libmvec.so.*|libnss_*.so.*|libselinux.so.*)' },
        @{ Name = 'VA-API bundled (would shadow the host driver)'; Old = '|libva*.so.*|'; New = '|' },
        @{ Name = 'the Vulkan loader bundled'; Old = 'libEGL.so.*|libGLESv2.so.*|libvulkan.so.*|'; New = 'libEGL.so.*|libGLESv2.so.*|' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        $copy = Copy-ToWorkspace $work $script 'package-linux.sh'
        Edit-WorkspaceText $copy $mutation.Old $mutation.New
        if ((Test-Policy (Invoke-Policy $copy) -Quiet) -gt 0) { Write-Host "  mutation detected: $($mutation.Name)"; $detected++ }
        else { Write-Host "  [fail] mutation was not detected: $($mutation.Name)"; $failures++ }
    }
    Write-Host "appimage-libs mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $failures
