# Shared runner of the small headless host tests that read production headers and sources: the test is compiled against
# a workspace copy of the files it reads, then once more per mutation, and every mutated build must be rejected.
# Nothing production is written; the workspace lives in the run's temp folder. No window, GPU, launcher or game.

function Invoke-HostMutationTest {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Test,
        [string[]]$Sources = @(),
        [Parameter(Mandatory)][string[]]$Copy,
        [object[]]$Mutations = @(),
        [string]$AssertStub = '',
        [string[]]$IncludeDirs = @()
    )
    . (Join-Path $PSScriptRoot '_common.ps1')
    . (Join-Path $PSScriptRoot '_mutation.ps1')

    $parent = 'E:\ClaudeTemp\opencode'
    if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
    $temp = Join-Path $parent ("sos_${Name}_test_" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    try {
        $tc = Resolve-Toolchain 'auto'
        $env:PATH = "$($tc.Bin);$env:PATH"
        $prefix = Split-Path $tc.Bin -Parent
        $gpu = Join-Path $Root 'gpu'
        $clang = Join-Path $tc.Bin 'clang++.exe'
        $flags = @('--target=x86_64-w64-mingw32', '-std=gnu++23', '-Wall', '-Wextra', '-O1', '-DNOMINMAX', '-Wno-deprecated-declarations',
            '-Wno-missing-field-initializers', '-include', (Join-Path $gpu 'shim/dcp_win_compat.h'))

        $newWorkspace = {
            param([string]$Dir)
            foreach ($relative in $Copy) {
                Copy-ToWorkspace $Dir (Join-Path $Root $relative) $relative | Out-Null
            }
            if ($AssertStub) {
                $stub = Join-Path $Dir 'stub/common/assert.h'
                New-Item -ItemType Directory -Force -Path (Split-Path $stub -Parent) | Out-Null
                [IO.File]::WriteAllText($stub, $AssertStub)
            }
        }
        $compile = {
            param([string]$Dir, [string]$Exe, [string[]]$Extra)
            $includes = @()
            if ($AssertStub) { $includes += @('-I', (Join-Path $Dir 'stub')) }
            foreach ($include in $IncludeDirs) { $includes += @('-I', (Join-Path $Dir $include)) }
            $includes += @('-I', (Join-Path $Dir 'gpu/shim'), '-I', (Join-Path $Dir 'gpu/shadps4'), '-I', (Join-Path $prefix 'include'))
            $rootText = $Dir.Replace('\', '/')
            $inputs = @((Join-Path $Root $Test)) + @($Sources | ForEach-Object { Join-Path $Dir $_ })
            & $clang @flags @Extra "-DSOS_SOURCE_DIR=$rootText" @includes @inputs '-o' $Exe
        }

        $baseline = Join-Path $temp 'base'
        & $newWorkspace $baseline
        $exe = Join-Path $baseline 'test.exe'
        & $compile $baseline $exe @()
        if ($LASTEXITCODE -ne 0) { throw "could not compile the $Name test" }
        $run = Invoke-TimedProgram $exe
        Write-Host $run.Text.TrimEnd()
        $result = $run.Code

        $detected = 0
        foreach ($mutation in $Mutations) {
            $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
            & $newWorkspace $work
            Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
            $mutatedExe = Join-Path $work 'test.exe'
            $build = { & $compile $work $mutatedExe @('-w') }.GetNewClosure()
            if (Test-MutationDetected $mutation.Name $build $mutatedExe) { $detected++ } else { $result = 1 }
        }
        Write-Host "$Name mutations: $detected of $($Mutations.Count) detected"
        return $result
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
