# Headless import preflight for the builds whose modules are on this machine (prepared folder or extracted layers): every PRX
# import table and PLT/GOT call site is checked against the real NID mappings and resolver tables. SaveData.prx may not leave a
# called import without a resolver. A build without modules is reported as NOT INSTALLED. No game, launcher, GPU or window is started.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$python = Get-Python
if (-not $python) { Stop-Tool 'Python 3 not found. Run tools\setup.ps1.' }
$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_import_preflight_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$script = Join-Path $root 'scripts\preflight_imports.py'
$result = 0

function Run-Preflight([string[]]$Arguments) {
    $command = @($python.Args) + @($script) + $Arguments
    $text = & $python.Exe @command 2>&1
    return [pscustomobject]@{ Text = ($text -join "`n"); Code = $LASTEXITCODE }
}

try {
    $saveReport = Join-Path $temp 'savedata.txt'
    $save = Run-Preflight @('--check', '--report', $saveReport)
    ($save.Text -split "`r?`n") | Where-Object { $_ -match 'CALLED IMPORTS|KNOWN OPTIONAL|SaveData\.prx|REQUIRED NID|Import preflight|^EVIDENCE\|' } |
        ForEach-Object { Write-Host $_ }
    if ($save.Code -eq 0 -and $save.Text -match 'CALLED IMPORTS WITH NO RESOLVER: none') {
        Write-Host '  [ok]   SaveData.prx has no called import without a resolver in the builds whose modules are here'
    } else {
        Write-Host "  [fail] SaveData.prx has a called import without a resolver (exit $($save.Code))"
        $result = 1
    }
    $namesText = Get-Content -LiteralPath (Join-Path $root 'src\import_names.inc') -Raw
    if ($namesText.Contains('{"VwadwBBBJ80#libSceSaveData","sceSaveDataUmountWithBackup"}')) {
        Write-Host '  [ok]   the VwadwBBBJ80 mapping is present'
    } else {
        Write-Host '  [fail] the VwadwBBBJ80 mapping is missing'
        $result = 1
    }

    $allReport = Join-Path $temp 'all.txt'
    $all = Run-Preflight @('--all', '--check', '--report', $allReport)
    if ($all.Code -eq 0 -and (Test-Path $allReport)) {
        $rows = @(Get-Content -LiteralPath $allReport | Where-Object { $_ -match 'CUSA57767-1\.0[01]\s+.*total=' })
        $scanned = @($all.Text -split "`r?`n" | Where-Object { $_ -match '^EVIDENCE\|content\|' -and $_ -notmatch '\|NOT INSTALLED\|' }).Count
        Write-Host ("  [ok]   full inventory scanned ({0} module rows, {1} build(s) with modules)" -f $rows.Count, $scanned)
        if ($rows.Count -lt 20 * $scanned) {
            Write-Host '  [fail] the full inventory did not cover the modules of every build that has them'
            $result = 1
        }
        $all.Text -split "`r?`n" | Where-Object { $_ -match '^EVIDENCE\|' } | ForEach-Object { Write-Host $_ }
    } else {
        Write-Host "  [fail] the full import inventory failed (exit $($all.Code))"
        $result = 1
    }

    $missingExport = Join-Path $temp 'missing_export'
    New-Item -ItemType Directory -Force -Path $missingExport | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'src\runtime_savedata.c') -Destination (Join-Path $missingExport 'runtime_savedata.c')
    Copy-Item -LiteralPath (Join-Path $root 'src\import_names.inc') -Destination (Join-Path $missingExport 'import_names.inc')
    $savedata = Get-Content -LiteralPath (Join-Path $missingExport 'runtime_savedata.c') -Raw
    $savedata = $savedata.Replace('    {"sceSaveDataUmountWithBackup",save_umount_with_backup}, ', '')
    Set-Content -LiteralPath (Join-Path $missingExport 'runtime_savedata.c') -Value $savedata -NoNewline
    $mutated = Run-Preflight @('--src', $missingExport, '--check')
    if ($mutated.Code -ne 0 -and $mutated.Text -match 'VwadwBBBJ80' -and $mutated.Text -match 'sceSaveDataUmountWithBackup') {
        Write-Host '  [ok]   removing the SaveData export is detected'
    } else {
        Write-Host "  [fail] removing the SaveData export was not detected (exit $($mutated.Code))"
        $result = 1
    }

    $missingNid = Join-Path $temp 'missing_nid'
    New-Item -ItemType Directory -Force -Path $missingNid | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'src\runtime_savedata.c') -Destination (Join-Path $missingNid 'runtime_savedata.c')
    $names = Get-Content -LiteralPath (Join-Path $root 'src\import_names.inc') -Raw
    $names = $names -replace '\{"VwadwBBBJ80#libSceSaveData","sceSaveDataUmountWithBackup"\},\r?\n', ''
    Set-Content -LiteralPath (Join-Path $missingNid 'import_names.inc') -Value $names -NoNewline
    $mutated = Run-Preflight @('--src', $missingNid, '--check')
    if ($mutated.Code -ne 0 -and $mutated.Text -match 'REQUIRED NID MAPPINGS MISSING') {
        Write-Host '  [ok]   removing the SaveData NID mapping is detected'
    } else {
        Write-Host "  [fail] removing the SaveData NID mapping was not detected (exit $($mutated.Code))"
        $result = 1
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
