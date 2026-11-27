# SOS-BXG9UT: the production shader recompiler on each GPU family's interpolation route (Steam Deck AMD explicit, NVIDIA
# manual barycentrics, Intel Xe2 native barycentrics, Intel ARL driver-interpolation fallback), every module checked by
# spirv-val; the fallback module must carry no barycentric or AMD explicit-vertex capability, extension, built-in or
# decoration, keep its flat input Flat and its integer fix; a P10/P20 V_INTERP_MOV_F32 the fallback cannot express stops
# with status 23 naming the shader after draining the log. Linux (WSL: clang++, libfmt, libZydis, Boost, SPIRV-Tools);
# without them the group reports not applicable. Mutated copies of the recompiler sources must fail it.
# No window, GPU, launcher or game; the repository is only read.
[CmdletBinding()]
param([switch]$BaseOnly)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$probe = if ($wsl) {
    (& wsl.exe -e sh -c 'command -v clang++ >/dev/null && command -v spirv-val >/dev/null && command -v spirv-dis >/dev/null && test -d /usr/include/fmt && test -f /usr/include/Zydis/Zydis.h && test -d /usr/include/boost && echo ready' 2>$null) -join ''
} else { '' }
if ($probe.Trim() -ne 'ready') {
    Write-Host '  [n/a]  no WSL with clang++, libfmt, libZydis, Boost and SPIRV-Tools (spirv-val): not applicable, not a failure'
    exit 0
}

function ConvertTo-WslPath([string]$Path) {
    return ((& wsl.exe -e wslpath -a ($Path.Replace('\', '/'))) | Select-Object -First 1).Trim()
}

$repo = ConvertTo-WslPath $root
# Content-keyed objects: safe to keep between runs, so later runs only compile what changed.
$cache = '/tmp/sos-interpolation-spirv-cache'
$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_interp_spirv_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

function Invoke-SpirvTest([string]$Overlay, [string]$Label) {
    $overlayWsl = if ($Overlay) { ConvertTo-WslPath $Overlay } else { '' }
    $exe = "$cache/$Label/interpolation_fallback_spirv_tests"
    $script = "tr -d '\r' < '$repo/tools/interpolation_spirv_build.sh' > '/tmp/sos_interp_build_$Label.sh' && " +
        "mkdir -p '$cache/$Label' && ln -sfn '$cache/obj' '$cache/$Label/obj' 2>/dev/null; mkdir -p '$cache/obj'; " +
        "SOS_OVERLAY='$overlayWsl' sh '/tmp/sos_interp_build_$Label.sh' '$repo' '$cache/$Label' || exit 97; " +
        "cd /tmp && '$exe'; code=`$?; rm -f '$exe' '/tmp/sos_interp_build_$Label.sh'; exit `$code"
    # Windows PowerShell 5.1 turns native stderr into terminating errors under Stop.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & wsl.exe -e sh -c $script 2>&1 | ForEach-Object { "$_" } }
    finally { $ErrorActionPreference = $previous }
    return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($output) }
}

$failures = 0
try {
    $run = Invoke-SpirvTest '' 'base'
    $run.Lines | Where-Object { $_ -notmatch '^GPU \[' } | ForEach-Object { Write-Host "  $_" }
    if ($run.Code -eq 97) { throw 'the interpolation SPIR-V test did not build under WSL' }
    if ($run.Code -ne 0) { $failures++ }
    if ($BaseOnly) { exit $failures }

    $interp ='gpu/shadps4/shader_recompiler/frontend/translate/vector_interpolation.cpp'
    $emit = 'gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv.cpp'
    $context = 'gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp'
    $integer = 'gpu/shadps4/shader_recompiler/ir/passes/interpolated_integer_pass.cpp'
    $assert = 'gpu/shim/common/assert.cpp'
    $mutations = @(
        @{ Name = 'the fallback reads a flat P0 as a per-vertex attribute'; File = $interp; Old = "        interp.primary = Qualifier::Flat;`n        SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));"; New = "        interp.primary = Qualifier::PerVertex;`n        SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));" },
        @{ Name = 'the barycentric capability is declared without the extension'; File = $emit; Old = "        } else if (profile.supports_fragment_shader_barycentric) {`n            ctx.AddExtension(`"SPV_KHR_fragment_shader_barycentric`");"; New = "        } else {`n            ctx.AddExtension(`"SPV_KHR_fragment_shader_barycentric`");" },
        @{ Name = 'a flat input loses its Flat decoration'; File = $context; Old = 'Decorate(attr_id, primary == Qualifier::Flat ? spv::Decoration::Flat'; New = 'Decorate(attr_id, primary == Qualifier::Flat ? spv::Decoration::NoPerspective' },
        @{ Name = 'the interpolated-integer fix is not applied'; File = $integer; Old = 'inst.SetArg(0, ir.FPMul(value, ir.Imm32(InterpolatedIntegerScale)));'; New = 'inst.SetArg(0, value);' },
        @{ Name = 'the fallback stop does not name the shader'; File = $interp; Old = '"V_INTERP_MOV_F32 P{} of smooth attribute {} in shader {:#018x} needs per-vertex attributes "'; New = '"V_INTERP_MOV_F32 P{} of smooth attribute {} needs per-vertex attributes ({:#x}) "' },
        @{ Name = 'a GPU assertion ends without draining the log'; File = $assert; Old = "    if (auto* flush = Common::DeviceLost::Global().ExitFlush()) {`n        flush();`n    }`n"; New = '' }
    )
    $detected = 0
    $index = 0
    foreach ($mutation in $mutations) {
        $overlay = Join-Path $temp ('mut' + $index)
        Copy-ToWorkspace $overlay (Join-Path $root $mutation.File) $mutation.File | Out-Null
        Edit-WorkspaceText (Join-Path $overlay $mutation.File) $mutation.Old $mutation.New
        $mutated = Invoke-SpirvTest $overlay ('mut' + $index)
        if ($mutated.Code -eq 97) { Write-Host "  [fail] mutation did not build: $($mutation.Name)"; $failures++ }
        elseif ($mutated.Code -ne 0) { Write-Host "  mutation detected: $($mutation.Name)"; $detected++ }
        else { Write-Host "  [fail] mutation was not detected: $($mutation.Name)"; $failures++ }
        $index++
    }
    Write-Host "interpolation_spirv mutations: $detected of $($mutations.Count) detected"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $failures
