# Headless proof of Bug Report HTTP client, resumable chunked minidump upload (with mid-transfer drop & resume),
# Worker SQLite storage, retention/cleanup, and admin dashboard download + mutations. Local loopback only.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_crash_report_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$serverProcess = $null
try {
    $result = 0

    # 1. Worker + Dashboard Node unit tests on production server/bug-report sources.
    $workerDir = Join-Path $root 'server\bug-report'
    $nodeOut = & node --test (Join-Path $workerDir 'test\reports.test.js') (Join-Path $workerDir 'test\dashboard.test.js') 2>&1
    $nodeCode = $LASTEXITCODE
    $nodeOut | ForEach-Object { Write-Host $_ }
    if ($nodeCode -ne 0) { $result = 1 }

    # 2. End-to-end C++ HTTP resumable upload & dashboard download against the real local Node server.
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $target = '--target=x86_64-w64-mingw32'
    $inc = @('-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $prefix 'include'))
    $cxxFlags = @($target, '-std=c++20', '-Wall', '-Wextra', '-O1')
    $sources = @(
        (Join-Path $root 'tests\bug_report_upload_tests.cpp'),
        (Join-Path $root 'launcher\bug_report.cpp'),
        (Join-Path $root 'launcher\bug_report_http.cpp'),
        (Join-Path $root 'launcher\log_scrubber.cpp'),
        (Join-Path $root 'launcher\game_profile.cpp'),
        (Join-Path $root 'launcher\localization\ui_text.cpp'),
        (Join-Path $root 'launcher\updater\json.cpp')
    )
    $objs = New-Object System.Collections.Generic.List[string]
    foreach ($src in $sources) {
        $obj = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
        & $clangxx @cxxFlags @inc '-c' $src '-o' $obj
        if ($LASTEXITCODE -ne 0) { throw "could not compile $src" }
        $objs.Add($obj)
    }
    $uploadExe = Join-Path $temp 'bug_report_upload_tests.exe'
    & $clangxx $target '-o' $uploadExe @($objs) '-lwinhttp'
    if ($LASTEXITCODE -ne 0) { throw 'could not link bug_report_upload_tests' }

    $token = 'local-test-dashboard-token-01234567'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'node'
    $psi.Arguments = '"' + (Join-Path $workerDir 'local\server.js') + '"'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['PORT'] = '0'
    $psi.EnvironmentVariables['SOS_DASHBOARD_TOKEN'] = $token
    $psi.EnvironmentVariables['SOS_DROP_CHUNK_OFFSET'] = '65536'
    $serverProcess = [System.Diagnostics.Process]::Start($psi)

    $listening = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        $line = $serverProcess.StandardOutput.ReadLine()
        if ($null -eq $line) { break }
        if ($line -match '^LISTENING (http://127\.0\.0\.1:\d+)$') {
            $listening = $Matches[1]
            break
        }
    }
    if (-not $listening) {
        throw "local bug-report server did not start: $($serverProcess.StandardError.ReadToEnd())"
    }

    $uploadOut = & $uploadExe $listening $token 2>&1
    $uploadCode = $LASTEXITCODE
    $uploadOut | ForEach-Object { Write-Host $_ }
    if ($uploadCode -ne 0) { $result = 1 }

    if ($serverProcess -and -not $serverProcess.HasExited) {
        try { $serverProcess.Kill() } catch { $null = $_ }
        $serverProcess.WaitForExit(3000) | Out-Null
    }
    $serverProcess = $null

    # 3. Worker & Dashboard mutations on temporary copies.
    $mutations = @(
        @{ Name = 'resume accepts wrong offset gap'; File = 'src/reports.js'; Old = "if (offset > stored) return { error: 'wrong_offset', status: 409, detail: 'gap', stored };"; New = 'if (false) return {};' },
        @{ Name = 'chunk exceeding declared dump size accepted'; File = 'src/reports.js'; Old = "if (offset + bytes.length > dump.bytes) {"; New = 'if (false) {' },
        @{ Name = 'server-side MDMP magic validation removed'; File = 'src/reports.js'; Old = "if (offset === 0 && !isMinidump(bytes)) {"; New = 'if (false) {' },
        @{ Name = 'dashboard download returns wrong report dump'; File = 'src/admin.js'; Old = 'const dump = await store.reportDump(api.ticket);'; New = 'const list = await store.reports(); const other = list.reports.find((r) => r.ticket !== api.ticket && r.dump_status === "stored"); const dump = await store.reportDump(other ? other.ticket : api.ticket);' },
        @{ Name = 'retention delete leaves orphan dump chunks'; File = 'src/reports.js'; Old = "this.sql.run('DELETE FROM report_dump_chunks WHERE ticket = ?', ticket);"; New = '// orphan chunks kept' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        Copy-Item -LiteralPath $workerDir -Destination $work -Recurse -Force
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutOut = & node --test (Join-Path $work 'test\reports.test.js') (Join-Path $work 'test\dashboard.test.js') 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  [fail] mutation '$($mutation.Name)' was not detected"
            $result = 1
        } else {
            Write-Host "  mutation detected: $($mutation.Name)"
            $detected++
        }
    }
    Write-Host "crash-report mutations: $detected of $($mutations.Count) detected"
    exit $result
} finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        try { $serverProcess.Kill() } catch { $null = $_ }
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
