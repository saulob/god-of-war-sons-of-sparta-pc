# Headless CPU compatibility test (issue #1, processors without AVX2): the processor requirement for synthetic CPUID
# profiles, F16C conversion against the host's instructions, every stub form run natively against the instruction it
# replaces, table validation, mutated copies of the production sources that must fail; then, for each CUSA57767 build
# whose boot image or game files exist, the tables applied to the real boot image and modules (each real site run
# natively and through its stub), the site generator against the built-in tables, the instruction sets of the port's
# own binaries and the compiler flags. No game, launcher, window or audio device is started.
#   -Generate  rewrites src\cpu_compat_sites.inc from the real images of every supported build (needs all of them)
[CmdletBinding()]
param([switch]$Generate)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_evidence.ps1')
. (Join-Path $PSScriptRoot '_mutation.ps1')

$parent = 'E:\ClaudeTemp\opencode'
if (-not (Test-Path -LiteralPath $parent)) { $parent = [IO.Path]::GetTempPath() }
$parent = [IO.Path]::GetFullPath($parent)
$sandboxPrefix = 'sos_cpu_compat_test_'
$temp = Join-Path $parent ($sandboxPrefix + [guid]::NewGuid().ToString('N').Substring(0, 12))

# Every path this script creates or removes lies in its own new folder under $parent: never empty, relative, the
# repository, one of its ancestors, .git or .claude.
function Assert-Sandbox([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathRooted($Path)) { throw "sandbox: refused path '$Path'" }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $box = [IO.Path]::GetFullPath($temp).TrimEnd('\')
    $repo = [IO.Path]::GetFullPath($root).TrimEnd('\')
    if ((Split-Path -Leaf $box) -notlike "$sandboxPrefix*" -or -not $box.StartsWith($parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "sandbox: '$box' is not a test folder under $parent"
    }
    if ($full -ne $box -and -not $full.StartsWith($box + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "sandbox: '$full' is outside $box" }
    if ($full -eq $repo -or $repo.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "sandbox: '$full' touches the repository" }
    if ($full -match '\\\.(git|claude)(\\|$)') { throw "sandbox: '$full' names .git or .claude" }
}
Assert-Sandbox $temp
if (Test-Path -LiteralPath $temp) { throw "sandbox: $temp already exists" }
New-Item -ItemType Directory -Path $temp | Out-Null

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$clang = Join-Path $tc.Bin 'clang.exe'
$lib = Join-Path (Split-Path $tc.Bin) 'lib'
$python = Get-Command py, python -ErrorAction SilentlyContinue | Select-Object -First 1
[string[]]$pythonArguments = if ($python -and $python.Name -like 'py.*') { '-3', '-I' } else { '-I' }
$builds = @('1.00', '1.01')
$modules = @('libfmod.prx', 'libfmodstudio.prx')
$sources = @('src/cpu_compat.c', 'src/cpu_compat.h', 'src/cpu_compat_sites.inc', 'src/isa_emulate.c', 'src/isa_emulate.h',
             'src/redzone_analysis.c', 'src/redzone_analysis.h', 'src/runtime_module.c', 'tools/cpu_compat_module_tests.c',
             'tools/cpu_compat_profiles.h')

# The module loader's own code (runtime_module.c, included by the test) around the production compatibility API.
function Build-ModuleTests([string]$Base, [string]$Exe, [string[]]$Extra = @()) {
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-O2' '-D_WIN32_WINNT=0x0A00' '-DNTDDI_VERSION=0x0A000007' @Extra `
        '-I' (Join-Path $Base 'src') '-I' (Join-Path $root 'src') '-I' (Join-Path $root 'launcher') '-I' (Join-Path $root 'gpu\shim') `
        (Join-Path $Base 'tools\cpu_compat_module_tests.c') (Join-Path $root 'tools\startup_profile_stubs.c') `
        (Join-Path $root 'src\runtime_stop.c') (Join-Path $Base 'src\cpu_compat.c') (Join-Path $Base 'src\isa_emulate.c') '-pthread' '-o' $Exe
}

# The loader and the module loader call the production API where the game needs it: plan before the memory is reserved,
# install before the red-zone pass and before a module is mapped executable, a refusal that stops, the processor check
# before the launcher and the game, and #UD emulation in modules loaded later. Returns what is missing.
function Test-CompatWiring([string]$Loader, [string]$Module) {
    $problems = New-Object System.Collections.Generic.List[string]
    function At([string]$Text, [string]$What) { return $Text.IndexOf($What, [StringComparison]::Ordinal) }
    $plan = At $Loader 'const CpuCompatPlan compat = cpu_compat_plan(CPU_COMPAT_BOOT_IMAGE, compat_key, isa_host_features(), cpu_compat_forced());'
    $area = At $Loader 'const size_t compat_area = round_page(compat.stub_bytes);'
    $allocate = At $Loader 'image = allocate(round_page(size) + stub_area + compat_area);'
    $imports = At $Loader 'const uint64_t stubs = patch_import_stubs(relocs, nr);'
    $install = At $Loader 'install_cpu_compat(&compat, compat_key, image + round_page(size) + stub_area, compat_area);'
    $redzone = At $Loader 'runtime_redzone_install(image, size, triples, ns);'
    if ($plan -lt 0 -or $area -lt $plan -or $allocate -lt $area) { $problems.Add('loader: the boot image plan (host, forced, build) does not size the stub area before the image is allocated') }
    if ($install -lt 0 -or $install -lt $imports -or $redzone -lt 0 -or $install -gt $redzone) { $problems.Add('loader: the boot image is not adapted after relocation and before the red-zone pass') }
    $body = At $Loader 'static void install_cpu_compat('
    $outcome = At $Loader 'const CpuCompatOutcome outcome = cpu_compat_install(plan, image, image_bytes, stubs, stubs_size);'
    $fatal = At $Loader 'if (outcome.fatal) {'
    $stop = if ($fatal -ge 0) { $Loader.IndexOf('fail(message);', $fatal, [Math]::Min(400, $Loader.Length - $fatal), [StringComparison]::Ordinal) } else { -1 }
    $allow = if ($outcome -ge 0) { $Loader.IndexOf('runtime_redzone_allow_branches(', $outcome, [StringComparison]::Ordinal) } else { -1 }
    if ($body -lt 0 -or $outcome -lt $body -or $fatal -lt $outcome -or $stop -lt 0) { $problems.Add('loader: a refused install does not stop the game') }
    if ($allow -lt 0) { $problems.Add('loader: the red-zone pass is not told about the stubs') }
    if ((At $Loader 'if (compat_area) protect(image + round_page(size) + stub_area, compat_area, 5);') -lt 0) { $problems.Add('loader: the stubs are not made executable') }
    if ((At $Loader 'return cpu_compat_processor_usable(isa_host_features(), cause, cause_size, advice, advice_size);') -lt 0) { $problems.Add('loader: the processor check is not the production policy') }
    if ((At $Loader 'skip_launcher, force_launcher, system_usable, game_process,') -lt 0) { $problems.Add('loader: the launcher does not check the processor') }
    $check = At $Loader 'if (!cpu_usable(cause, sizeof(cause), advice, sizeof(advice))) fail(cause);'
    if ($check -lt 0 -or $check -gt (At $Loader 'FILE *f = fopen(boot, "rb");')) { $problems.Add('loader: an unusable processor is not refused before the game loads') }
    foreach ($handler in @(@('EXCEPTION_ILLEGAL_INSTRUCTION', 'Windows'), @('sig == SIGILL', 'Linux'))) {
        $from = At $Loader $handler[0]
        $to = if ($from -ge 0) { $Loader.IndexOf('isa_emulate(', $from, [StringComparison]::Ordinal) } else { -1 }
        if ($to -lt 0 -or $Loader.Substring($from, $to - $from) -notmatch 'runtime_module_in_loaded_code') { $problems.Add("loader: #UD in a module loaded later is not emulated ($($handler[1]))") }
    }
    $modulePlan = At $Module 'const CpuCompatPlan compat=plan_module_compat(m,isa_host_features(),cpu_compat_forced());'
    $moduleAllocate = At $Module 'm->base=(uintptr_t)module_alloc(page_round(m->size)+m->compat_bytes);'
    $relocate = At $Module 'if (!apply_table(m,&elf,rela,relasz)'
    $moduleInstall = At $Module 'if (!install_module_compat(m,&compat)) { free(elf.data); abandon_module(m); return ERRNO(8); }'
    $finalize = At $Module 'if (!finalize_mapping(m,&elf)) { free(elf.data); abandon_module(m); return ERRNO(13); }'
    $protect = At $Module "    protect_cpu_compat(m);`n"
    if ($Module.Contains("`r`n")) { $protect = At $Module "    protect_cpu_compat(m);`r`n" }
    if ($modulePlan -lt 0 -or $moduleAllocate -lt $modulePlan) { $problems.Add('module loader: the plan does not size the stubs before the module is allocated') }
    if ($moduleInstall -lt 0 -or $moduleInstall -lt $relocate -or $finalize -lt $moduleInstall) { $problems.Add('module loader: a module is not adapted after relocation and before it is mapped executable') }
    if ($protect -lt $finalize) { $problems.Add('module loader: the stubs are not made executable') }
    if ((At $Module '(size_t)(((m->size+host_page_size()-1)&~(uint64_t)(host_page_size()-1))+m->compat_bytes)') -lt 0) { $problems.Add('module loader: the stubs are not released with the module') }
    return ,$problems
}

function Build-Tests([string]$Src, [string]$Exe, [string[]]$Extra = @()) {
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-O1' '-D_WIN32_WINNT=0x0A00' @Extra '-I' $Src `
        (Join-Path $PSScriptRoot 'cpu_compat_tests.c') (Join-Path $Src 'cpu_compat.c') (Join-Path $Src 'isa_emulate.c') `
        (Join-Path $Src 'redzone_analysis.c') '-L' $lib '-lZydis' '-lZycore' '-o' $Exe
}
function Build-Generator([string]$Src, [string]$Exe, [string[]]$Extra = @(), [string]$Generator = (Join-Path $PSScriptRoot 'cpu_compat_gen.c')) {
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-O2' '-D_WIN32_WINNT=0x0A00' @Extra '-I' $Src `
        $Generator (Join-Path $Src 'cpu_compat.c') (Join-Path $Src 'isa_emulate.c') (Join-Path $Src 'redzone_analysis.c') `
        '-L' $lib '-lZydis' '-lZycore' '-o' $Exe
}
function New-Workspace([string]$Name) {
    $work = Join-Path $temp $Name
    Assert-Sandbox $work
    foreach ($file in $sources) { Copy-ToWorkspace $work (Join-Path $root $file) $file | Out-Null }
    return $work
}
function Write-Run($Run) { if ($Run.Text) { Write-Host $Run.Text.TrimEnd() } }

# The forbidden compiler and linker options in the build's own files: none may target a processor beyond x86-64.
$cpuFlags = '-march=|-mcpu=|-mtune=native|-mavx|-mfma|-mf16c|-mbmi|-mlzcnt|-mmovbe|-msse4|-mssse3|-msse3|-mpopcnt|-maes|x86-64-v[234]|/arch:'
function Find-CpuFlags([string[]]$Files) {
    $found = @()
    foreach ($file in $Files) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        $number = 0
        foreach ($line in [IO.File]::ReadAllLines($file)) {
            ++$number
            if ($file -like '*CMakeCache.txt' -and $line -notmatch '^CMAKE_(C|CXX|EXE_LINKER|SHARED_LINKER)_FLAGS') { continue }
            if ($line -match $cpuFlags) { $found += "${file}:${number}: $($line.Trim())" }
        }
    }
    return $found
}

$result = 0
try {
    $exe = Join-Path $temp 'cpu_compat_tests.exe'
    Build-Tests (Join-Path $root 'src') $exe @('-Werror')
    if ($LASTEXITCODE -ne 0) { throw 'could not compile tools\cpu_compat_tests.c' }
    $generator = Join-Path $temp 'cpu_compat_gen.exe'
    Build-Generator (Join-Path $root 'src') $generator @('-Werror')
    if ($LASTEXITCODE -ne 0) { throw 'could not compile tools\cpu_compat_gen.c' }
    $hostCheck = Join-Path $temp 'host_isa_check.exe'
    & $clang '--target=x86_64-w64-mingw32' '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-O2' (Join-Path $PSScriptRoot 'host_isa_check.c') `
        '-L' $lib '-lZydis' '-lZycore' '-o' $hostCheck
    if ($LASTEXITCODE -ne 0) { throw 'could not compile tools\host_isa_check.c' }

    $run = Invoke-TimedProgram $exe 300
    Write-Run $run
    if ($run.Code -ne 0) { $result = 1 }

    # The same tests built by gcc for Linux, where the emulation helper is called with the host's own convention.
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    $linuxRoot = if ($wsl) { (& wsl.exe -e wslpath -a ($root.Replace('\', '/')) 2>$null | Select-Object -First 1) } else { $null }
    $ready = if ($linuxRoot) { & wsl.exe -e sh -c 'command -v gcc >/dev/null && test -f /usr/include/Zydis/Zydis.h && echo yes' 2>$null } else { $null }
    if ($ready -eq 'yes') {
        Write-Host 'Linux (WSL, gcc)'
        $script = 'd=$(mktemp -d /tmp/sos_cpu_compat_XXXXXX) || exit 3; case "$d" in /tmp/sos_cpu_compat_?*) ;; *) exit 3 ;; esac; ' +
                  "cd '$linuxRoot' && gcc -std=gnu11 -Wall -Wextra -Werror -O1 -no-pie -Isrc tools/cpu_compat_tests.c src/cpu_compat.c src/isa_emulate.c -lZydis -o " + '"$d/t" && "$d/t"; r=$?; rm -rf -- "$d"; exit $r'
        $output = & wsl.exe -e sh -c $script 2>&1
        $output | Select-Object -Last 4 | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { $output | ForEach-Object { Write-Host "  $_" }; $result = 1 }
    } else {
        Write-Host '  [data] no WSL with gcc and Zydis: the Linux build of the tests was not run'
    }

    # --- the real boot images and modules of each build ------------------------------------------------------------------
    $bootArguments = @()
    $moduleArguments = @()
    $exported = @{}
    $bootResults = @{}
    foreach ($version in $builds) {
        $key = Get-BuildKey $version
        $image = Find-BuildBootImage $version
        if ($image) {
            Write-Evidence 'image' $version $image.State 'boot.bin: compatibility table applied, each site run natively and replaced'
            $run = Invoke-TimedProgram $exe 600 "--image $key `"$($image.Path)`""
            Write-Run $run
            if ($run.Code -ne 0) { $result = 1 }
            $bootResults[$version] = if ($run.Code -eq 0) { 'PASS' } else { 'FAIL' }
            $bootArguments += @('--boot', $key, $image.Path)
        } else {
            Write-Evidence 'image' $version 'NO DATA' 'no boot image in build\image or build\research\image; not read: the compatibility table on the real image'
            Write-Host "  [data] CUSA57767 $version boot image unavailable: its compatibility table was checked only by the contract tests"
        }
        foreach ($module in $modules) {
            $file = Find-BuildGameFile $version "Media/Plugins/$module"
            Write-GameFileEvidence $version $file "Media/Plugins/$module for its compatibility table"
            if (-not $file -or -not $python) { continue }
            $out = Join-Path $temp ('modules_' + $version)
            Assert-Sandbox $out
            $gameRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $file))
            & $python.Source @pythonArguments (Join-Path $PSScriptRoot 'export_code_ranges.py') $gameRoot $out $module | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "could not export $module of CUSA57767 $version" }
            Write-Host "CUSA57767 $version $module"
            $run = Invoke-TimedProgram $exe 300 "--module $module `"$out`""
            Write-Run $run
            if ($run.Code -ne 0) { $result = 1 }
            if (-not $exported.ContainsKey($module)) { $exported[$module] = $out; $moduleArguments += @('--module', $module, $out) }
        }
    }

    # --- the module loader itself on the real modules ------------------------------------------------------------------
    $moduleExe = Join-Path $temp 'cpu_compat_module_tests.exe'
    Build-ModuleTests $root $moduleExe @('-Werror')
    if ($LASTEXITCODE -ne 0) { throw 'could not compile tools\cpu_compat_module_tests.c' }
    $moduleRunArguments = (@($modules | Where-Object { $exported.ContainsKey($_) } | ForEach-Object { "$_ `"$($exported[$_])`"" })) -join ' '
    if ($moduleRunArguments) {
        $run = Invoke-TimedProgram $moduleExe 300 $moduleRunArguments
        $run.Text -split "`r?`n" | Where-Object { $_ -notmatch '^CPU compatibility: module ' } | ForEach-Object { if ($_) { Write-Host $_ } }
        if ($run.Code -ne 0) { $result = 1 }
        $modulePath = if ($run.Code -eq 0) { 'PASS' } else { 'FAIL' }
    } else {
        Write-Host '  [data] no libfmod.prx or libfmodstudio.prx of any build: the module loader was checked only by its plans'
        $modulePath = 'NO DATA'
    }

    # --- the loader's and module loader's wiring -----------------------------------------------------------------------
    $loaderText = [IO.File]::ReadAllText((Join-Path $root 'src\loader.c'))
    $moduleText = [IO.File]::ReadAllText((Join-Path $root 'src\runtime_module.c'))
    $wiring = Test-CompatWiring $loaderText $moduleText
    if ($wiring.Count) { $wiring | ForEach-Object { Write-Host "  [fail] $_" }; $result = 1 }
    else { Write-Host '  wiring: the loader and the module loader plan, install, refuse and protect through the production API; the processor check gates the launcher and the game; #UD in later modules is emulated on Windows and Linux' }

    # --- the generator: the built-in tables are what it makes of the real code -----------------------------------------
    $generatorArguments = @($bootArguments + $moduleArguments)
    if ($generatorArguments.Count) {
        Write-Host 'Site generator against the built-in tables'
        $run = Invoke-TimedProgram $generator 900 ((($generatorArguments | ForEach-Object { "`"$_`"" }) -join ' ') + ' --check')
        Write-Run $run
        if ($run.Code -ne 0) { $result = 1 }
        if ($Generate) {
            if ($bootArguments.Count -ne 3 * $builds.Count -or $moduleArguments.Count -ne 3 * $modules.Count) {
                throw '-Generate needs the boot image and modules of every supported build'
            }
            $target = Join-Path $root 'src\cpu_compat_sites.inc'
            & $generator @generatorArguments '--out' $target
            if ($LASTEXITCODE -ne 0) { throw 'the generator failed' }
        }
    } else {
        Write-Host '  [data] no boot image or module of any build: the generator was not run against real code'
    }

    # --- mutations of the production sources ----------------------------------------------------------------------------
    $mutations = @(
        @{ Name = 'LZCNT result off by one'; File = 'src/cpu_compat.c'; Old = 'put_byte(&nonzero, bits - 1);'; New = 'put_byte(&nonzero, bits);' },
        @{ Name = 'LZCNT/TZCNT of zero is not the width'; File = 'src/cpu_compat.c'; Old = 'else put_u32(&zero, 32);'; New = 'else put_u32(&zero, 31);' },
        @{ Name = 'CF clear for a zero source'; File = 'src/cpu_compat.c'; Old = 'put_byte(&zero, 0xf9);'; New = 'put_byte(&zero, 0xf8);' },
        @{ Name = 'TZCNT result cleared'; File = 'src/cpu_compat.c'; Old = '        put_byte(&nonzero, 0x85);'; New = '        put_byte(&nonzero, 0x31);' },
        @{ Name = 'shielded stub not below the red zone'; File = 'src/cpu_compat.c'; Old = 'if (s->shield) put(c, RSP_DOWN, sizeof RSP_DOWN);'; New = 'if (0) put(c, RSP_DOWN, sizeof RSP_DOWN);' },
        @{ Name = 'memory count not below the red zone'; File = 'src/cpu_compat.c'; Old = 'const int shield = form->memory && !form->rsp_base;'; New = 'const int shield = 0;' },
        @{ Name = 'ymm15 not saved around the emulation'; File = 'src/cpu_compat.c'; Old = 'for (unsigned r = 0; r < 16; ++r) {                                   /* vmovdqu [rsp+32r],ymm_r */'; New = 'for (unsigned r = 0; r < 15; ++r) {' },
        @{ Name = 'flags not restored after the emulation'; File = 'src/cpu_compat.c'; Old = '0x9d, 0xc3,'; New = '0x90, 0xc3,' },
        @{ Name = 'digest ignored'; File = 'src/cpu_compat.c'; Old = '== table->digest ? CPU_COMPAT_OK : CPU_COMPAT_BAD_SITE'; New = '== table->digest ? CPU_COMPAT_OK : CPU_COMPAT_OK' },
        @{ Name = 'overlapping sites accepted'; File = 'src/cpu_compat.c'; Old = 'if (!sorted_and_disjoint(table)) return CPU_COMPAT_BAD_SITE;'; New = '(void)sorted_and_disjoint;' },
        @{ Name = 'stub area reach not checked'; File = 'src/cpu_compat.c'; Old = 'if (needed > stubs_size || !reaches(image, stubs + stubs_size) || !reaches(image + size, stubs))'; New = 'if (needed > stubs_size)' },
        @{ Name = 'LZCNT not replaced without it'; File = 'src/cpu_compat.c'; Old = 'if (!(host.ext1_ecx & bit_LZCNT)) kinds |= CPU_COMPAT_LZCNT;'; New = '' },
        @{ Name = 'TZCNT not replaced without BMI1'; File = 'src/cpu_compat.c'; Old = 'kinds |= CPU_COMPAT_TZCNT | CPU_COMPAT_BMI1;'; New = 'kinds |= CPU_COMPAT_BMI1;' },
        @{ Name = 'half rounding not to even'; File = 'src/isa_emulate.c'; Old = '(dropped == halfway && (kept & 1))'; New = '(dropped == halfway && (kept & 0))' },
        @{ Name = 'DAZ ignored by VCVTPS2PH'; File = 'src/isa_emulate.c'; Old = 'isa_float_to_half(f, rounding, (mxcsr & 0x40) != 0)'; New = 'isa_float_to_half(f, rounding, 0)' },
        @{ Name = 'signalling NaN half not quieted'; File = 'src/isa_emulate.c'; Old = '(mantissa ? 0x400000u : 0)'; New = '0u' },
        @{ Name = 'rip-relative operand before an immediate'; File = 'src/isa_emulate.c'; Old = 'address += rip + c->length + trailing;'; New = 'address += rip + c->length;' },
        @{ Name = 'ymm upper half kept by VCVTPH2PS'; File = 'src/isa_emulate.c'; Old = "        memset(ymm[reg], 0, 32);`n        memcpy(ymm[reg], floats, sizeof floats);"; New = "        memcpy(ymm[reg], floats, sizeof floats);" },
        @{ Name = 'SSE4.1 required again'; File = 'src/isa_emulate.c'; Old = '{"AVX", 0, 0, bit_AVX},'; New = '{"AVX", 0, 0, bit_AVX}, {"SSE4.1", 0, 0, bit_SSE4_1},' },
        @{ Name = 'AVX2 required'; File = 'src/isa_emulate.c'; Old = '{"AVX", 0, 0, bit_AVX},'; New = '{"AVX", 0, 0, bit_AVX}, {"AVX2", 0, 1, bit_AVX2},' },
        @{ Name = 'POPCNT no longer required'; File = 'src/isa_emulate.c'; Old = '{"POPCNT", 0, 0, bit_POPCNT},'; New = '' },
        @{ Name = 'force path removed'; File = 'src/cpu_compat.c'; Old = 'CpuCompatPlan plan = {cpu_compat_needed(host) | forced, 0, 0, forced, NULL, 0};'; New = 'CpuCompatPlan plan = {cpu_compat_needed(host), 0, 0, forced, NULL, 0};' },
        @{ Name = 'CPU profile ignored'; File = 'src/cpu_compat.c'; Old = 'CpuCompatPlan plan = {cpu_compat_needed(host) | forced, 0, 0, forced, NULL, 0};'; New = 'CpuCompatPlan plan = {forced, 0, 0, forced, NULL, 0};' },
        @{ Name = 'signature validation removed'; File = 'src/cpu_compat.c'; Old = '(kind && cpu_compat_instruction(image + s->offset + at, length, kind) != length)'; New = '0' },
        @{ Name = 'unknown build accepted'; File = 'src/cpu_compat.c'; Old = "if (!strcmp(cpu_compat_tables[i].name, name)) return &cpu_compat_tables[i];`n    return NULL;"; New = "if (!strcmp(cpu_compat_tables[i].name, name)) return &cpu_compat_tables[i];`n    return cpu_compat_tables;" },
        @{ Name = 'partial patch accepted'; File = 'src/cpu_compat.c'; Old = "    const CpuCompatStatus status = cpu_compat_check(image, size, table);`n    if (status != CPU_COMPAT_OK) return status;"; New = '' },
        @{ Name = 'refused install not fatal'; File = 'src/cpu_compat.c'; Old = 'out.fatal = plan->essential != 0;'; New = 'out.fatal = 0;' },
        @{ Name = 'CPU without AVX accepted'; File = 'src/cpu_compat.c'; Old = 'if (!isa_missing_sets(host, 0, missing, sizeof missing)) return 1;'; New = 'isa_missing_sets(host, 0, missing, sizeof missing); return 1;' }
    )
    $detected = 0
    $index = 0
    foreach ($m in $mutations) {
        $work = New-Workspace ('mut' + (++$index))
        Edit-WorkspaceText (Join-Path $work $m.File) $m.Old $m.New
        $mutExe = Join-Path $work 'tests.exe'
        $srcDir = Join-Path $work 'src'
        $build = { Build-Tests $srcDir $mutExe @('-w') }.GetNewClosure()
        if (Test-MutationDetected $m.Name $build $mutExe) { $detected++ } else { $result = 1 }
    }
    $total = $mutations.Count

    # The built-in table against a real image: a changed site offset or digest is refused.
    $real = Find-BuildBootImage '1.00'
    if ($real) {
        $table = [IO.File]::ReadAllText((Join-Path $root 'src\cpu_compat_sites.inc'))
        $first = [regex]::Match($table, 'cpu_compat_sites_\d+\[\] = \{\s*\{0x([0-9a-f]+),')
        $digest = [regex]::Match($table, '\{"CUSA57767-1\.00", \d+u, 0x([0-9a-f]{16})ull')
        foreach ($t in @(@{ Name = 'built-in site offset changed'; Old = $first.Value; New = $first.Value -replace '0x([0-9a-f]+),$', { '0x{0:x},' -f ([Convert]::ToUInt32($_.Groups[1].Value, 16) + 1) } },
                         @{ Name = 'built-in digest changed'; Old = $digest.Value; New = $digest.Value.Substring(0, $digest.Value.Length - 4) + $(if ($digest.Value[-4] -eq '0') { '1' } else { '0' }) + 'ull' })) {
            $work = New-Workspace ('mut' + (++$index))
            Edit-WorkspaceText (Join-Path $work 'src\cpu_compat_sites.inc') $t.Old $t.New
            $mutExe = Join-Path $work 'tests.exe'
            Build-Tests (Join-Path $work 'src') $mutExe @('-w')
            $run = Invoke-TimedProgram $mutExe 600 "--image CUSA57767-1.00 `"$($real.Path)`""
            if ($run.Code -ne 0) { Write-Host "  mutation detected: $($t.Name)"; $detected++ } else { Write-Host "  [fail] mutation '$($t.Name)' was not detected"; $result = 1 }
            $total++
        }
        # Without branch targets the generator merges across them: other sites, which --check refuses.
        $work = New-Workspace ('mut' + (++$index))
        $copy = Copy-ToWorkspace $work (Join-Path $PSScriptRoot 'cpu_compat_gen.c') 'tools/cpu_compat_gen.c'
        Edit-WorkspaceText $copy 'if (k != i - back && marked(targets, list[k].at, end)) ok = 0;' ''
        $mutGen = Join-Path $work 'gen.exe'
        Build-Generator (Join-Path $work 'src') $mutGen @('-w') $copy
        $run = Invoke-TimedProgram $mutGen 900 ("--boot CUSA57767-1.00 `"$($real.Path)`" --check")
        if ($run.Code -ne 0) { Write-Host '  mutation detected: generator ignores branch targets'; $detected++ }
        else { Write-Host "  [fail] mutation 'generator ignores branch targets' was not detected"; $result = 1 }
        $total++
    } else {
        Write-Host '  [data] no CUSA57767 1.00 boot image: the built-in table and branch-target mutations were not run'
    }

    # A function whose jump table enters the instruction after a 4-byte LZCNT: the site takes the one before it instead.
    #   lea rcx,[rip+table]; movsxd rax,[rcx+rax*4]; add rax,rcx; jmp rax; case0: mov edx,1; lzcnt eax,ecx;
    #   case1: xor eax,31; ret; table: case0, case1
    $synthetic = Join-Path $temp 'jump_table'
    Assert-Sandbox $synthetic
    New-Item -ItemType Directory -Path $synthetic | Out-Null
    [byte[]]$code = @(0x48, 0x8d, 0x0d, 0x39, 0, 0, 0, 0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0, 0xba, 1, 0, 0, 0,
                      0xf3, 0x0f, 0xbd, 0xc1, 0x83, 0xf0, 0x1f, 0xc3) + @(,0xcc * 35) + @(0xd0, 0xff, 0xff, 0xff, 0xd9, 0xff, 0xff, 0xff)
    [IO.File]::WriteAllBytes((Join-Path $synthetic 'jt.prx.code'), $code)
    [IO.File]::WriteAllText((Join-Path $synthetic 'jt.prx.fn'), "0 1d`n")
    [IO.File]::WriteAllText((Join-Path $synthetic 'jt.prx.size'), "4096`n")
    $want = '{0x10, 9, 0, {0x05, 0x14}}'
    $made = Join-Path $synthetic 'made.inc'
    & $generator --module jt.prx $synthetic --out $made | Out-Null
    if ($LASTEXITCODE -eq 0 -and ([IO.File]::ReadAllText($made)).Contains($want)) { Write-Host '  generator: a jump-table target stays outside the merged site' }
    else { Write-Host '  [fail] generator: the jump-table function did not give the expected site'; $result = 1 }
    $work = New-Workspace ('mut' + (++$index))
    $copy = Copy-ToWorkspace $work (Join-Path $PSScriptRoot 'cpu_compat_gen.c') 'tools/cpu_compat_gen.c'
    Edit-WorkspaceText $copy "                        mark(targets, to, end);`n                        flags[f] |= 2;" "                        flags[f] |= 2;"
    $mutGen = Join-Path $work 'gen.exe'
    Build-Generator (Join-Path $work 'src') $mutGen @('-w') $copy
    $mutMade = Join-Path $work 'made.inc'
    & $mutGen --module jt.prx $synthetic --out $mutMade | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not ([IO.File]::ReadAllText($mutMade)).Contains($want)) { Write-Host '  mutation detected: generator ignores jump tables'; $detected++ }
    else { Write-Host "  [fail] mutation 'generator ignores jump tables' was not detected"; $result = 1 }
    $total++

    # One LZCNT, TZCNT or F16C site dropped from the built-in table of CUSA57767 1.00 (count adjusted).
    $inc = [IO.File]::ReadAllText((Join-Path $root 'src\cpu_compat_sites.inc')).Replace("`r`n", "`n")
    $blockStart = $inc.IndexOf('cpu_compat_sites_0[] = {', [StringComparison]::Ordinal)
    $blockEnd = $inc.IndexOf('};', $blockStart, [StringComparison]::Ordinal)
    foreach ($kind in @(@('LZCNT', '1'), @('TZCNT', '2'), @('F16C', '3'))) {
        $row = [regex]::Match($inc.Substring($blockStart, $blockEnd - $blockStart), '\{0x[0-9a-f]+, \d+, [01], \{0x' + $kind[1] + '[0-9a-f]\}\}, ?')
        $count = [regex]::Match($inc, 'cpu_compat_sites_0, (\d+)\}')
        if (-not $row.Success -or -not $count.Success) { Write-Host "  [fail] no single $($kind[0]) site to drop"; $result = 1; continue }
        $mutated = $inc.Remove($blockStart + $row.Index, $row.Length)
        $mutated = $mutated.Replace($count.Value, "cpu_compat_sites_0, $([int]$count.Groups[1].Value - 1)}")
        $work = New-Workspace ('mut' + (++$index))
        [IO.File]::WriteAllText((Join-Path $work 'src\cpu_compat_sites.inc'), $mutated)
        $mutExe = Join-Path $work 'tests.exe'
        $srcDir = Join-Path $work 'src'
        $build = { Build-Tests $srcDir $mutExe @('-w') }.GetNewClosure()
        if (Test-MutationDetected "one $($kind[0]) site removed from the table" $build $mutExe) { $detected++ } else { $result = 1 }
        $total++
    }

    # The module loader's own code, mutated: the test of runtime_module.c must fail.
    if ($moduleRunArguments) {
        foreach ($m in @(@{ Name = 'FMOD module not processed (looked up by its full path)'; Old = 'cpu_compat_plan(CPU_COMPAT_MODULE,file_name(m->path),host,forced)'; New = 'cpu_compat_plan(CPU_COMPAT_MODULE,m->path,host,forced)' },
                         @{ Name = 'module refusal ignored'; Old = 'return !outcome.fatal;'; New = 'return 1;' },
                         @{ Name = 'module stubs not reserved'; Old = 'm->compat_bytes=page_round(plan.stub_bytes);'; New = 'm->compat_bytes=0;' })) {
            $work = New-Workspace ('mut' + (++$index))
            Edit-WorkspaceText (Join-Path $work 'src\runtime_module.c') $m.Old $m.New
            $mutExe = Join-Path $work 'modules.exe'
            Build-ModuleTests $work $mutExe @('-w')
            $run = if (Test-Path -LiteralPath $mutExe) { Invoke-TimedProgram $mutExe 300 $moduleRunArguments } else { [pscustomobject]@{ Code = 0 } }
            if ($run.Code -ne 0) { Write-Host "  mutation detected: $($m.Name)"; $detected++ } else { Write-Host "  [fail] mutation '$($m.Name)' was not detected"; $result = 1 }
            $total++
        }
    }

    # The wiring, mutated in the loader's and module loader's text: the gate must name each break.
    $eol = if ($loaderText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $install = 'install_cpu_compat(&compat, compat_key, image + round_page(size) + stub_area, compat_area);'
    $redzone = 'runtime_redzone_install(image, size, triples, ns);'
    $wiringMutations = @(
        @{ Name = 'boot image not processed'; Loader = @($install, '') },
        @{ Name = 'boot image adapted after the red-zone pass'; Loader = @($install, ''); Then = @($redzone, ($redzone + $eol + $install)) },
        @{ Name = 'force path not passed by the loader'; Loader = @('isa_host_features(), cpu_compat_forced());', 'isa_host_features(), 0);') },
        @{ Name = 'CPU profile not read by the loader'; Loader = @('compat_key, isa_host_features(), cpu_compat_forced());', 'compat_key, (IsaHostFeatures){0, 0, 0}, cpu_compat_forced());') },
        @{ Name = 'no stub area for the boot image'; Loader = @('image = allocate(round_page(size) + stub_area + compat_area);', 'image = allocate(round_page(size) + stub_area);') },
        @{ Name = 'refused boot image does not stop the game'; Loader = @(('        fail(message);' + $eol + '    }' + $eol + '    if (outcome.message[0])'), ('    }' + $eol + '    if (outcome.message[0])')) },
        @{ Name = 'red-zone pass not told about the stubs'; Loader = @('if (outcome.replaced) runtime_redzone_allow_branches(', 'if (0) (void)(') },
        @{ Name = 'CPU without AVX accepted by the loader'; Loader = @('if (!cpu_usable(cause, sizeof(cause), advice, sizeof(advice))) fail(cause);', '') },
        @{ Name = 'launcher without the processor check'; Loader = @('skip_launcher, force_launcher, system_usable, game_process,', 'skip_launcher, force_launcher, NULL, game_process,') },
        @{ Name = 'module #UD not emulated on Windows'; Loader = @('runtime_module_in_loaded_code((uintptr_t)info->ContextRecord->Rip)', '0') },
        @{ Name = 'module #UD not emulated on Linux'; Loader = @('runtime_module_in_loaded_code((uintptr_t)rip)', '0') },
        @{ Name = 'FMOD module not processed by the module loader'; Module = @('if (!install_module_compat(m,&compat)) { free(elf.data); abandon_module(m); return ERRNO(8); }', '') },
        @{ Name = 'module stubs left writable'; Module = @("    protect_cpu_compat(m);$(if ($moduleText.Contains("`r`n")) { "`r`n" } else { "`n" })", '') },
        @{ Name = 'module allocated without its stubs'; Module = @('module_alloc(page_round(m->size)+m->compat_bytes)', 'module_alloc(page_round(m->size))') }
    )
    foreach ($w in $wiringMutations) {
        $l = $loaderText
        $md = $moduleText
        foreach ($step in @(@('Loader', 'loader'), @('Then', 'loader'), @('Module', 'module'))) {
            $pair = $w[$step[0]]
            if (-not $pair) { continue }
            $text = if ($step[1] -eq 'loader') { $l } else { $md }
            if (-not $text.Contains($pair[0])) { throw "wiring mutation text not found: $($pair[0])" }
            if ($step[1] -eq 'loader') { $l = $text.Replace($pair[0], $pair[1]) } else { $md = $text.Replace($pair[0], $pair[1]) }
        }
        if ((Test-CompatWiring $l $md).Count) { Write-Host "  mutation detected: $($w.Name)"; $detected++ }
        else { Write-Host "  [fail] mutation '$($w.Name)' was not detected"; $result = 1 }
        $total++
    }

    # --- the port's own binaries ----------------------------------------------------------------------------------------
    Write-Host 'Instruction sets of the port''s binaries'
    $binaries = @()
    foreach ($folder in @((Join-Path $root 'build\cmake\clang64-release\bin'), (Join-Path $root 'build\cmake\clang64\bin'))) {
        if (Test-Path -LiteralPath $folder) { $binaries += Get-ChildItem -LiteralPath $folder -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object FullName }
    }
    $package = Get-ChildItem -LiteralPath (Join-Path $root 'dist') -Directory -Filter '*-windows-x64' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($package) { $binaries += Get-ChildItem -LiteralPath $package.FullName -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object FullName }
    if ($binaries.Count) {
        & $hostCheck @binaries
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    } else {
        Write-Host '  [data] no built sons_of_sparta.exe: the binaries were not scanned'
    }
    # The gate fails on a binary that needs more than it allows.
    $probe = Join-Path $temp 'probe.c'
    Set-Content -LiteralPath $probe -Value 'int f(int *a, int n) { int s = 0; for (int i = 0; i < n; ++i) s += a[i] * a[i]; return s; } int main(void) { return 0; }' -Encoding ascii
    foreach ($p in @(@{ Name = 'own executable with POPCNT/SSE4.2'; Out = 'own_sse42\sons_of_sparta.exe'; Flags = @('-msse4.2', '-mpopcnt'); Fail = $true },
                     @{ Name = 'own executable with AVX2'; Out = 'own_avx2\sons_of_sparta.exe'; Flags = @('-mavx2'); Fail = $true },
                     @{ Name = 'library with AVX2 and no CPUID choice'; Out = 'lib_avx2\libprobe.dll'; Flags = @('-mavx2', '-shared'); Fail = $true },
                     @{ Name = 'own executable for x86-64'; Out = 'own_plain\sons_of_sparta.exe'; Flags = @(); Fail = $false })) {
        $out = Join-Path $temp $p.Out
        Assert-Sandbox $out
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
        & $clang '--target=x86_64-w64-mingw32' '-O3' @($p.Flags) $probe '-o' $out
        if ($LASTEXITCODE -ne 0) { throw "could not compile the $($p.Name) probe" }
        $run = Invoke-TimedProgram $hostCheck 60 "`"$out`""
        $failedGate = $run.Code -ne 0
        if ($failedGate -eq $p.Fail) { Write-Host "  gate $(if ($p.Fail) { 'rejects' } else { 'accepts' }): $($p.Name)"; if ($p.Fail) { $detected++ } }
        else { Write-Host "  [fail] gate wrong on: $($p.Name)"; $result = 1 }
        if ($p.Fail) { $total++ }
    }

    # --- compiler flags ------------------------------------------------------------------------------------------------
    $flagFiles = @((Join-Path $root 'CMakeLists.txt'), (Join-Path $root 'tools\build.ps1'), (Join-Path $root 'tools\build.sh'),
                   (Join-Path $root 'tools\_gpu_build.ps1'), (Join-Path $root 'tools\build_game.ps1'))
    foreach ($folder in @('gpu', 'launcher')) {
        $flagFiles += Get-ChildItem -LiteralPath (Join-Path $root $folder) -Recurse -File -Include 'CMakeLists.txt', '*.cmake' -ErrorAction SilentlyContinue | ForEach-Object FullName
    }
    $flagFiles += Get-ChildItem -LiteralPath (Join-Path $root 'build\cmake') -Recurse -File -Filter 'CMakeCache.txt' -Depth 1 -ErrorAction SilentlyContinue | ForEach-Object FullName
    $flags = @(Find-CpuFlags $flagFiles)
    if ($flags.Count) { $flags | ForEach-Object { Write-Host "  [fail] processor-specific compiler option: $_" }; $result = 1 }
    else { Write-Host "  compiler options: no -march, -mavx*, -mfma, -mbmi*, -mlzcnt, -mmovbe, -mf16c, -msse4*, x86-64-v2/3/4 or /arch in $($flagFiles.Count) build files" }
    $copy = Join-Path $temp 'CMakeLists.txt'
    Assert-Sandbox $copy
    Copy-Item -LiteralPath (Join-Path $root 'CMakeLists.txt') -Destination $copy
    Add-Content -LiteralPath $copy -Value 'add_compile_options(-march=x86-64-v3)'
    if (@(Find-CpuFlags @($copy)).Count) { Write-Host '  mutation detected: -march=x86-64-v3 added to CMakeLists.txt'; $detected++ }
    else { Write-Host '  [fail] mutation -march=x86-64-v3 was not detected'; $result = 1 }
    $total++

    Write-Host 'CPU compatibility summary'
    $profileNames = [regex]::Matches([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'cpu_compat_profiles.h')), '\{"([^"]+)", \{') | ForEach-Object { $_.Groups[1].Value }
    Write-Host "  profiles ($($profileNames.Count)): $($profileNames -join '; ')"
    foreach ($t in [regex]::Matches([IO.File]::ReadAllText((Join-Path $root 'src\cpu_compat_sites.inc')), '\{"([^"]+)", \d+u, 0x[0-9a-f]+ull, cpu_compat_sites_\d+, (\d+)\}')) {
        $name = $t.Groups[1].Value
        $state = if ($name -like 'CUSA*') { $version = $name.Split('-')[1]; if ($bootResults.ContainsKey($version)) { "real boot image: normal path $($bootResults[$version]), forced compatibility path $($bootResults[$version])" } else { 'no real boot image (contract and plans only)' } }
                 elseif ($exported.ContainsKey($name)) { "real module through runtime_module.c: normal path $modulePath, forced compatibility path $modulePath" } else { 'no real module (plans only)' }
        Write-Host "  $($name.Replace('CUSA57767-', 'CUSA57767 ')): $($t.Groups[2].Value) sites; $state"
    }
    Write-Host "  wiring of the loader and the module loader: $(if ($wiring.Count) { 'FAIL' } else { 'PASS' })"
    Write-Host "CPU compatibility mutations: $detected of $total detected"
} finally {
    Assert-Sandbox $temp
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $result
