# Headless proof of the Bug Report system: the Worker protocol, crash dump resumability, admin authentication and
# downloads, the launcher's WebSocket client and My reports history, plus mutated copies that must fail. Loopback only.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$temp = Join-Path $parent ('sos_bug_report_test_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$serverProcess = $null
try {
    $result = 0

    # 1. Worker, protocol and admin tests on the production server/bug-report sources.
    $workerDir = Join-Path $root 'server\bug-report'
    $nodeOut = & node --test (Join-Path $workerDir 'test\incidents.test.js') (Join-Path $workerDir 'test\admin.test.js') (Join-Path $workerDir 'test\worker.test.js') 2>&1
    $nodeCode = $LASTEXITCODE
    $nodeOut | ForEach-Object { Write-Host $_ }
    if ($nodeCode -ne 0) { $result = 1 }

    # 1b. The manual Worker script and the single client endpoint stay as documented.
    $sourceChecks = @(
        @{ Name = 'tools/worker.ps1 exists'; File = 'tools\worker.ps1'; Pattern = 'server\\bug-report' },
        @{ Name = 'the Worker script supports a dry run'; File = 'tools\worker.ps1'; Pattern = '--dry-run' },
        @{ Name = 'the Worker script never installs Wrangler'; File = 'tools\worker.ps1'; Pattern = '--no-install' },
        @{ Name = 'the Worker script names the Sons dashboard secret'; File = 'tools\worker.ps1'; Pattern = 'SOS_DASHBOARD_TOKEN' },
        @{ Name = 'the client has one endpoint resolver'; File = 'launcher\bug_report_link.cpp'; Pattern = 'ResolveBugReportConfig' },
        @{ Name = 'the compiled production endpoint is the confirmed Worker'; File = 'launcher\bug_report_link.cpp'; Pattern = 'kDefaultBugReportUrl\[\] = "wss://sons-of-sparta-bug-report\.saulobenigno\.workers\.dev/ws"' },
        @{ Name = 'the environment override remains available'; File = 'launcher\bug_report_link.cpp'; Pattern = 'SOS_BUG_REPORT_URL' },
        @{ Name = 'the file override remains available'; File = 'launcher\bug_report_link.cpp'; Pattern = 'bug_report_url.txt' },
        @{ Name = 'the client keeps a missing endpoint distinct'; File = 'launcher\bug_report_link.h'; Pattern = 'kNotConfigured' },
        @{ Name = 'the client keeps a malformed endpoint distinct'; File = 'launcher\bug_report_link.h'; Pattern = 'kInvalidEndpoint' }
    )
    foreach ($check in $sourceChecks) {
        $path = Join-Path $root $check.File
        if ((Test-Path -LiteralPath $path -PathType Leaf) -and ((Get-Content -Raw -LiteralPath $path) -match $check.Pattern)) {
            Write-Host "  [pass] $($check.Name)"
        } else {
            Write-Host "  [fail] $($check.Name)"
            $result = 1
        }
    }
    $endpointHits = @(Get-ChildItem -LiteralPath (Join-Path $root 'launcher'), (Join-Path $root 'src') -Recurse -File -Include '*.cpp', '*.h' |
        Select-String -SimpleMatch 'sons-of-sparta-bug-report.saulobenigno.workers.dev')
    if ($endpointHits.Count -eq 1 -and $endpointHits[0].Path -match 'bug_report_link\.cpp$') {
        Write-Host '  [pass] the production endpoint is defined in exactly one client source'
    } else {
        Write-Host '  [fail] the production endpoint must be defined only in launcher/bug_report_link.cpp'
        $result = 1
    }

    # 2. Launcher model, receipts, statuses, feedback and saved history with no network.
    $tc = Resolve-Toolchain 'auto'
    $env:PATH = "$($tc.Bin);$env:PATH"
    $prefix = Split-Path $tc.Bin -Parent
    $clangxx = Join-Path $tc.Bin 'clang++.exe'
    $target = '--target=x86_64-w64-mingw32'
    $inc = @('-I', (Join-Path $root 'launcher'), '-I', (Join-Path $root 'src'), '-I', (Join-Path $prefix 'include'))
    $cxxFlags = @($target, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1')
    $sources = @(
        (Join-Path $root 'launcher\bug_report.cpp'),
        (Join-Path $root 'launcher\bug_report_link.cpp'),
        (Join-Path $root 'launcher\report_history.cpp'),
        (Join-Path $root 'launcher\log_scrubber.cpp'),
        (Join-Path $root 'launcher\game_profile.cpp'),
        (Join-Path $root 'launcher\localization\ui_text.cpp'),
        (Join-Path $root 'src\net\json.cpp'),
        (Join-Path $root 'src\net\websocket.cpp'),
        (Join-Path $root 'src\net\websocket_win.cpp'),
        (Join-Path $root 'src\net\report_link.cpp')
    )
    $objects = New-Object System.Collections.Generic.List[string]
    foreach ($src in $sources) {
        $obj = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
        & $clangxx @cxxFlags @inc '-c' $src '-o' $obj
        if ($LASTEXITCODE -ne 0) { throw "could not compile $src" }
        $objects.Add($obj)
    }

    function Link-Test([string]$name) {
        $object = Join-Path $temp ($name + '.o')
        & $clangxx @cxxFlags @inc '-c' (Join-Path $root "tests\$name.cpp") '-o' $object
        if ($LASTEXITCODE -ne 0) { throw "could not compile $name.cpp" }
        $exe = Join-Path $temp ($name + '.exe')
        & $clangxx $target '-o' $exe @($object) @($objects) '-lwinhttp'
        if ($LASTEXITCODE -ne 0) { throw "could not link $name" }
        return $exe
    }

    $unitExe = Link-Test 'bug_report_tests'
    $unitOut = & $unitExe 2>&1
    $unitCode = $LASTEXITCODE
    $unitOut | ForEach-Object { Write-Host $_ }
    if ($unitCode -ne 0) { $result = 1 }

    # 3. End-to-end WebSocket submission and resumable dump upload against the real local Node service.
    $uploadExe = Link-Test 'bug_report_upload_tests'
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
        if ($line -match '^LISTENING (ws://127\.0\.0\.1:\d+)$') {
            $listening = $Matches[1]
            break
        }
    }
    if (-not $listening) {
        throw "local bug-report server did not start: $($serverProcess.StandardError.ReadToEnd())"
    }

    $uploadOut = & $uploadExe ($listening + '/ws') 2>&1
    $uploadCode = $LASTEXITCODE
    $uploadOut | ForEach-Object { Write-Host $_ }
    if ($uploadCode -ne 0) { $result = 1 }

    if ($serverProcess -and -not $serverProcess.HasExited) {
        try { $serverProcess.Kill() } catch { $null = $_ }
        $serverProcess.WaitForExit(3000) | Out-Null
    }
    $serverProcess = $null

    # 4. Worker and admin mutations on temporary copies.
    $mutations = @(
        @{ Name = 'a dump frame beyond the announced size is accepted'; File = 'src/service.js'; Old = '&& payload <= INCIDENT.dumpChunkBytes && up.at + payload <= up.total;'; New = '&& payload <= INCIDENT.dumpChunkBytes;' },
        @{ Name = 'the first chunk need not be a minidump'; File = 'src/service.js'; Old = 'if (up.at === 0 && !isMinidump(data)) {'; New = 'if (false) {' },
        @{ Name = 'a wrong receipt still opens the report'; File = 'src/incidents.js'; Old = 'return row && digestsMatch(row.receipt_hash, hash) ? row : null;'; New = 'return row ? row : null;' },
        @{ Name = 'a dump name need not match the log'; File = 'src/incidents.js'; Old = "if (msg.dump_file !== dumpNameOf(logFile)) throw new Refusal('bad_request', 'dump_file');"; New = "if (false) throw new Refusal('bad_request', 'dump_file');" },
        @{ Name = 'the admin serves the dump of another report'; File = 'src/admin.js'; Old = 'const dump = await store.incidentDump(api.ticket);'; New = 'const list = await store.incidents(); const other = list.incidents.find((r) => r.ticket !== api.ticket && r.dump_status === "stored"); const dump = await store.incidentDump(other ? other.ticket : api.ticket);' },
        @{ Name = 'deleting a report leaves its status'; File = 'src/incidents.js'; Old = "this.sql.run('DELETE FROM incident_tracking WHERE ticket = ?', ticket);"; New = '// status kept' },
        @{ Name = 'the admin token comparison ignores the token'; File = 'src/admin.js'; Old = 'return diff === 0;'; New = 'return true;' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        Copy-Item -LiteralPath $workerDir -Destination $work -Recurse -Force
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutOut = & node --test (Join-Path $work 'test\incidents.test.js') (Join-Path $work 'test\admin.test.js') (Join-Path $work 'test\worker.test.js') 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  [fail] mutation '$($mutation.Name)' was not detected"
            $result = 1
        } else {
            Write-Host "  mutation detected: $($mutation.Name)"
            $detected++
        }
    }
    Write-Host "bug-report mutations: $detected of $($mutations.Count) detected"
    exit $result
} finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        try { $serverProcess.Kill() } catch { $null = $_ }
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
