# Helpers of the headless test scripts that prove a gate can fail: a mutated copy of production sources is compiled with
# the same test, and the test must reject it. Nothing production is ever written; copies live in the run's temp folder.

# Copies a production file to <Workspace>\<Destination> (the path the test includes it by).
function Copy-ToWorkspace([string]$Workspace, [string]$Source, [string]$Destination) {
    $target = Join-Path $Workspace $Destination
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target -Force
    return $target
}

# Replaces the first occurrence of $Old in a workspace file; a mutation that matches nothing is a broken mutation.
function Edit-WorkspaceText([string]$Path, [string]$Old, [string]$New) {
    $text = [IO.File]::ReadAllText($Path)
    $old = $Old.Replace("`r`n", "`n")
    $new = $New.Replace("`r`n", "`n")
    if ($text.Contains("`r`n")) { $text = $text.Replace("`r`n", "`n") }
    $at = $text.IndexOf($old, [StringComparison]::Ordinal)
    if ($at -lt 0) { throw "mutation text not found in ${Path}: $($old.Trim())" }
    [IO.File]::WriteAllText($Path, $text.Substring(0, $at) + $new + $text.Substring($at + $old.Length), (New-Object Text.UTF8Encoding($false)))
}

# Runs a program with a time limit; a hang or a crash is a rejection like a failed check.
function Invoke-TimedProgram([string]$File, [int]$Seconds = 60, [string]$Arguments = '') {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($Seconds * 1000)) {
        try { $process.Kill() } catch { $null = $_ }
        return [pscustomobject]@{ Code = 124; Text = 'timed out' }
    }
    $process.WaitForExit()
    return [pscustomobject]@{ Code = $process.ExitCode; Text = ($out.Result + $err.Result) }
}

# One mutation: build the test against the mutated workspace, run it, and require it to fail. Returns $true when the
# mutation was detected. A mutated build that does not compile counts as a broken mutation, not as a detection.
function Test-MutationDetected([string]$Name, [scriptblock]$Build, [string]$Exe) {
    if (Test-Path -LiteralPath $Exe) { Remove-Item -LiteralPath $Exe -Force }
    & $Build
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Exe)) {
        Write-Host "  [fail] mutation '$Name' did not compile"
        return $false
    }
    $run = Invoke-TimedProgram $Exe
    if ($run.Code -eq 0) {
        Write-Host "  [fail] mutation '$Name' was not detected"
        return $false
    }
    Write-Host "  mutation detected: $Name"
    return $true
}
