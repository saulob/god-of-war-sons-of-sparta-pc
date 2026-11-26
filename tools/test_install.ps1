# SOS-6Y8NVY installer regression (tests/install_tests.cpp), synthetic data only: the current installer, the same tests
# with the production manifest, the v0.1.0 installer reproducing the report, mutated installers, and Linux under WSL.
# Every run works in a new folder outside the repository; the repository is only read.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

function Test-RepositoryIntact {
    (Test-Path -LiteralPath (Join-Path $root '.git\HEAD') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $root '.git\objects') -PathType Container)
}

function Get-RepositoryFixtures {
    @('game', 'game\prepared', 'game\image', 'game\extracted') | ForEach-Object {
        $dir = Join-Path $root $_
        if (Test-Path -LiteralPath $dir) { Get-ChildItem -LiteralPath $dir -Force -Filter '*CUSA99999*' }
    }
}

if (-not (Test-RepositoryIntact)) { Write-Host '  [fail] the repository has no .git: stopped before any test'; exit 70 }
if (@(Get-RepositoryFixtures).Count) { Write-Host '  [fail] a CUSA99999 folder is already in game: stopped'; exit 70 }

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$parent = [IO.Path]::GetFullPath($parent).TrimEnd('\')
$temp = Join-Path $parent ('sosi_' + [guid]::NewGuid().ToString('N').Substring(0, 8))

function Test-Inside([string]$Child, [string]$Folder) {
    $Child.StartsWith($Folder.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

# Refuses anything but this run's own folder or what is in it.
function Assert-RemovableTemp([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathRooted($Path)) { throw "refusing to remove '$Path'" }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $repo = [IO.Path]::GetFullPath($root).TrimEnd('\')
    $own = [IO.Path]::GetFullPath($temp).TrimEnd('\')
    if ($full -match '(^|\\)\.(git|claude)(\\|$)' -or $full -eq $repo -or (Test-Inside $repo $full) -or (Test-Inside $full $repo)) {
        throw "refusing to remove '$Path': the repository, inside it or an ancestor"
    }
    if (-not ($full -eq $own -or (Test-Inside $full $own)) -or -not (Test-Inside $own $parent) -or
        (Split-Path $own -Leaf) -notmatch '^sosi_[0-9a-f]{8}$') {
        throw "refusing to remove '$Path': not this run's test folder"
    }
    $item = Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "refusing to remove the link '$Path'" }
}

function Remove-TempItem([string]$Path) {
    Assert-RemovableTemp $Path
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

if ((Test-Inside $temp $root) -or (Test-Inside $root $temp)) { throw 'the test folder must be outside the repository' }
New-Item -ItemType Directory -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$prefix = Split-Path $tc.Bin -Parent
$clangxx = Join-Path $tc.Bin 'clang++.exe'
$target = '--target=x86_64-w64-mingw32'
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw),
    'project\(sons_of_sparta_pc VERSION (\d+\.\d+\.\d+)').Groups[1].Value
$cxxFlags = @($target, '-std=c++23', '-O1', (Get-StringDefine 'SOS_PORT_VERSION' $version))
$strict = @('-Wall', '-Wextra', '-Werror')
$trgInclude = Join-Path $root 'third_party\trg-launcher\include'
$sources = @('install_fs', 'pkg_extract', 'game_install', 'game_files', 'game_profile', 'boot_image', 'build_diagnostics',
             'crypto_win', 'log_scrubber', 'localization\ui_text')
$libs = @('-L', (Join-Path $prefix 'lib'), '-lz', '-lbcrypt', '-lshell32', '-lole32', '-lcomdlg32')
$failures = 0

function Build-Objects([string]$Tree, [string]$ObjDir, [string[]]$Names, [string[]]$Extra) {
    New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null
    $inc = @('-I', (Join-Path $Tree 'launcher'), '-I', (Join-Path $prefix 'include'))
    $objects = @()
    foreach ($name in $Names) {
        $object = Join-Path $ObjDir (($name -replace '[\\/]', '_') + '.o')
        & $clangxx @cxxFlags @Extra @inc '-c' (Join-Path $Tree "launcher\$name.cpp") '-o' $object
        if ($LASTEXITCODE -ne 0) { return $null }
        $objects += $object
    }
    return $objects
}

function Build-Test([string]$Tree, [string[]]$Objects, [string]$Exe, [string[]]$Extra) {
    $inc = @('-I', (Join-Path $Tree 'launcher'), '-I', $trgInclude, '-I', (Join-Path $prefix 'include'))
    $object = Join-Path (Split-Path $Exe -Parent) 'install_tests.o'
    & $clangxx @cxxFlags @Extra @inc '-c' (Join-Path $root 'tests\install_tests.cpp') '-o' $object
    if ($LASTEXITCODE -ne 0) { return }
    & $clangxx $target @Objects $object @libs '-o' $Exe
}

function New-Workspace([string]$Dir) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Dir 'launcher\localization') | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $root 'launcher') -File | Where-Object { $_.Extension -in '.h', '.cpp', '.c' } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Dir 'launcher') }
    Get-ChildItem -LiteralPath (Join-Path $root 'launcher\localization') -File |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Dir 'launcher\localization') }
}

function Invoke-InstallTests([string]$Exe, [string]$Source) {
    $runTemp = Join-Path $temp ('r' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    return Invoke-TimedProgram $Exe 300 ('"' + $runTemp + '" "' + $Source + '" "' + $root + '"')
}

function Write-Run($Run) { Write-Host $Run.Text.TrimEnd() }

try {
    Write-Step 'Current installer (Windows, real file system, no long paths in this process)'
    $base = Join-Path $temp 'base'
    New-Workspace $base
    $objects = Build-Objects $base (Join-Path $base 'obj') $sources $strict
    if (-not $objects) { throw 'could not compile the installer sources' }
    $trgObject = Join-Path $base 'obj\trg_platform.o'
    & $clangxx $target '-std=c++17' '-O1' '-D_CRT_SECURE_NO_WARNINGS' '-I' $trgInclude '-c' `
        (Join-Path $root 'third_party\trg-launcher\src\platform.cpp') '-o' $trgObject
    if ($LASTEXITCODE -ne 0) { throw 'could not compile the TRG task' }
    $objects += $trgObject
    $exe = Join-Path $base 'install_tests.exe'
    Build-Test $base $objects $exe $strict
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) { throw 'could not compile tests\install_tests.cpp' }
    $sourceFile = Join-Path $base 'launcher\pkg_extract.cpp'
    $run = Invoke-InstallTests $exe $sourceFile
    Write-Run $run
    if ($run.Code -ne 0) { $failures++; Write-Host "  [fail] install tests exited with $($run.Code)" }

    Write-Step 'The same tests with the production manifest (src\sons_of_sparta.rc)'
    $resDir = Join-Path $temp 'res'
    New-Item -ItemType Directory -Force -Path $resDir | Out-Null
    $numbers = $version.Replace('.', ',')
    [IO.File]::WriteAllText((Join-Path $resDir 'sos_version.h'),
        "#define SOS_VERSION_NUMBERS $numbers,0`n#define SOS_FILE_VERSION_STRING `"$version.0`"`n#define SOS_VERSION_STRING `"$version`"`n")
    $resource = Join-Path $resDir 'sons_of_sparta_res.o'
    & (Join-Path $tc.Bin 'windres.exe') '-I' $resDir '-I' (Join-Path $root 'src') (Join-Path $root 'src\sons_of_sparta.rc') '-O' 'coff' '-o' $resource
    if ($LASTEXITCODE -ne 0) { throw 'could not compile src\sons_of_sparta.rc' }
    $manifestExe = Join-Path $base 'install_tests_manifest.exe'
    & $clangxx $target @objects (Join-Path $base 'install_tests.o') $resource @libs '-o' $manifestExe
    if ($LASTEXITCODE -ne 0) { throw 'could not link the manifest build of the tests' }
    $manifest = Invoke-TimedProgram $manifestExe 30 '--manifest'
    Write-Run $manifest
    if ($manifest.Code -ne 0) { $failures++; Write-Host '  [fail] the production manifest lacks longPathAware' }
    $withManifest = Invoke-InstallTests $manifestExe $sourceFile
    Write-Run $withManifest
    if ($withManifest.Code -ne 0) { $failures++; Write-Host "  [fail] install tests with the manifest exited with $($withManifest.Code)" }

    Write-Step 'The v0.1.0 installer against the same scenario (the release of SOS-6Y8NVY)'
    $baseline = Join-Path $temp 'v010'
    New-Item -ItemType Directory -Force -Path $baseline | Out-Null
    $archive = Join-Path $temp 'v010.tar'
    & git -C $root archive --format=tar -o $archive v0.1.0 launcher 2>$null
    if ($LASTEXITCODE -ne 0) {
        $failures++
        Write-Host '  [fail] the v0.1.0 tag is not in this clone: the legacy reproduction cannot run'
    } else {
        & tar -xf $archive -C $baseline
        $oldSources = @($sources | Where-Object { $_ -notin 'install_fs', 'log_scrubber' })
        $oldObjects = Build-Objects $baseline (Join-Path $baseline 'obj') $oldSources @('-w')
        $oldExe = Join-Path $baseline 'install_tests_v010.exe'
        if ($oldObjects) { Build-Test $baseline $oldObjects $oldExe @('-w', '-DSOS_INSTALL_BASELINE') }
        if (-not $oldObjects -or -not (Test-Path -LiteralPath $oldExe)) {
            $failures++
            Write-Host '  [fail] the v0.1.0 installer did not compile with the tests'
        } else {
            $old = Invoke-InstallTests $oldExe $sourceFile
            Write-Run $old
            if ($old.Code -ne 0) {
                $failures++
                Write-Host "  [fail] the v0.1.0 installer did not fail the scenario the way the report shows ($($old.Code))"
            } else {
                Write-Host '  [ok]   reproduced: the v0.1.0 installer fails SOS-6Y8NVY, the current one installs it'
            }
        }
    }

    Write-Step 'Mutated installers must fail'
    $mutations = @(
        @{ Name = 'Windows gets the plain path again (no extended-length form)'; File = 'launcher\install_fs.cpp'; Old = '        return fs::path(L"\\\\?\\" + full);'; New = '        return path;' },
        @{ Name = 'the extractor creates the file with std::ofstream again'; File = 'launcher\pkg_extract.cpp'; Old = '        installfs::OutputFile file;
        if (!file.Open(target, error)) {'; New = '        installfs::OutputFile file;
        if (std::ofstream probe(target, std::ios::binary); !probe || !file.Open(target, error)) {' },
        @{ Name = 'a failed file loses its system error'; File = 'launcher\pkg_extract.cpp'; Old = '    return {code, std::move(detail), std::move(error)};'; New = '    return {code, std::move(detail), {}};' },
        @{ Name = 'a failed installation keeps its staging'; File = 'launcher\game_install.cpp'; Old = '        if (FsError error; !installfs::RemoveAll(dir_, error)) {
            leftovers_.push_back(error.Describe());
        }'; New = '' },
        @{ Name = 'the bar is left at the failed step'; File = 'launcher\game_install.cpp'; Old = '        if (hooks_.phase) {
            hooks_.phase(kCleanupPhase);
        }'; New = '' },
        @{ Name = 'a failure does not say where it stopped'; File = 'launcher\game_install.cpp'; Old = '        result.stage_done = track->done;'; New = '' },
        @{ Name = 'a failed activation does not restore the installed game'; File = 'launcher\game_install.cpp'; Old = '        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
        return FsFailure(InstallStatus::kWriteFailed, "cannot activate", std::move(error));'; New = '        return FsFailure(InstallStatus::kWriteFailed, "cannot activate", std::move(error));' },
        @{ Name = 'a backup left by a failed restore is not put back'; File = 'launcher\game_install.cpp'; Old = '        if (!installfs::Exists(*own) && installfs::Exists(*backup) && !RenameWithRetry(*backup, *own, error)) {'; New = '        if (false) {' },
        @{ Name = 'the installed game''s path limit is not checked'; File = 'launcher\game_install.cpp'; Old = 'if (!paths.long_paths && paths.longest_final >= installfs::kMaxPath) {'; New = 'if (false) {' },
        @{ Name = 'a full disk is reported as a write failure'; File = 'launcher\game_install.cpp'; Old = 'if (status == InstallStatus::kWriteFailed && error.problem() == FsProblem::kDiskFull) {'; New = 'if (false) {' },
        @{ Name = 'access denied is not named'; File = 'launcher\install_fs.cpp'; Old = '    case 5: case 19: return FsProblem::kAccessDenied;   // ERROR_ACCESS_DENIED, ERROR_WRITE_PROTECT'; New = '' }
    )
    $detected = 0
    foreach ($mutation in $mutations) {
        $work = Join-Path $temp ('mut_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Workspace $work
        Edit-WorkspaceText (Join-Path $work $mutation.File) $mutation.Old $mutation.New
        $mutated = [IO.Path]::GetFileNameWithoutExtension($mutation.File)
        $mutExe = Join-Path $work 'install_tests.exe'
        $mutObject = Build-Objects $work (Join-Path $work 'obj') @($mutated) @('-w')
        if ($mutObject) {
            $linked = @($objects | Where-Object { (Split-Path $_ -Leaf) -ne ($mutated + '.o') }) + @($mutObject)
            Build-Test $work $linked $mutExe @('-w')
        }
        if (-not $mutObject -or $LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $mutExe)) {
            Write-Host "  [fail] mutation '$($mutation.Name)' did not compile"
            $failures++
            continue
        }
        $mutRun = Invoke-InstallTests $mutExe $sourceFile
        if ($mutRun.Code -eq 0) {
            Write-Host "  [fail] mutation '$($mutation.Name)' was not detected"
            $failures++
        } elseif ($mutRun.Code -eq 70) {
            Write-Host "  [fail] mutation '$($mutation.Name)': the run was stopped by the test folder guard"
            $failures++
        } else {
            Write-Host "  mutation detected: $($mutation.Name)"
            $detected++
        }
        Remove-TempItem $work
    }
    Write-Host "install mutations: $detected of $($mutations.Count) detected"

    Write-Step 'Linux (WSL): the same tests on a Linux file system'
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    $ready = $false
    if ($wsl) {
        $probe = & wsl.exe -e sh -c 'command -v clang++ >/dev/null 2>&1 && test -f /usr/include/openssl/evp.h && test -f /usr/include/zlib.h && echo ready' 2>$null
        $ready = ($probe -join '').Trim() -eq 'ready'
    }
    if (-not $ready) {
        Write-Host '  [n/a] no WSL with clang++, OpenSSL and zlib headers: the Linux run is skipped'
    } else {
        $wslBase = (& wsl.exe -e wslpath -a ($base.Replace('\', '/')) | Select-Object -First 1).Trim()
        $wslRoot = (& wsl.exe -e wslpath -a ($root.Replace('\', '/')) | Select-Object -First 1).Trim()
        $linuxSources = @('install_fs', 'pkg_extract', 'game_install', 'game_files', 'game_profile', 'boot_image',
                          'build_diagnostics', 'crypto_openssl', 'log_scrubber', 'localization/ui_text') |
            ForEach-Object { "'$wslBase/launcher/$_.cpp'" }
        $script = @"
set -e
out=`$(mktemp -d /tmp/sos_install_build.XXXXXX)
case "`$out" in /tmp/sos_install_build.??????) ;; *) echo "unexpected build folder '`$out'"; exit 70 ;; esac
trap 'case "`$out" in /tmp/sos_install_build.??????) rm -rf -- "`$out" ;; esac' EXIT
clang++ -std=c++17 -O1 -I '$wslRoot/third_party/trg-launcher/include' -c '$wslRoot/third_party/trg-launcher/src/platform.cpp' -o "`$out/trg_platform.o"
clang++ -std=c++23 -O1 -Wall -Wextra -Werror -DSOS_PORT_VERSION='"$version"' -I '$wslBase/launcher' \
  -I '$wslRoot/third_party/trg-launcher/include' '$wslRoot/tests/install_tests.cpp' $($linuxSources -join ' ') \
  "`$out/trg_platform.o" -pthread -lssl -lcrypto -lz -o "`$out/install_tests"
"`$out/install_tests" "`$out/run" '$wslBase/launcher/pkg_extract.cpp' '$wslRoot'
"@
        $linux = & wsl.exe -e bash -c ($script.Replace("`r`n", "`n")) 2>&1
        $linuxCode = $LASTEXITCODE
        Write-Host (($linux | ForEach-Object { "$_" }) -join "`n")
        if ($linuxCode -ne 0) { $failures++; Write-Host "  [fail] the Linux run exited with $linuxCode" }
    }
} finally {
    Remove-TempItem $temp
}

Write-Step 'The repository after the run'
if (-not (Test-RepositoryIntact)) { $failures++; Write-Host '  [fail] the repository lost its .git: stop and check it by hand' }
else { Write-Host '  [ok]   .git is still there' }
$fixtures = @(Get-RepositoryFixtures)
if ($fixtures.Count) { $failures++; Write-Host "  [fail] CUSA99999 folders in game (not removed): $($fixtures.FullName -join ', ')" }
else { Write-Host '  [ok]   no CUSA99999 folder in the repository''s game tree' }
if (Test-Path -LiteralPath $temp) { $failures++; Write-Host "  [fail] the test folder was left: $temp" }
if ($failures -eq 0) { Write-Host 'install: all checks passed' } else { Write-Host "install: $failures failure(s)" }
exit $failures
