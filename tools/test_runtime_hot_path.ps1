# Headless proof of the recursive lock helper the draw path takes several times per draw, on the production source with a
# stub for the assert macros: nesting locks once, free release order, and no allocation after a thread's first lock, plus
# mutated copies the same test must reject.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot '_host_test.ps1')

$lock = 'gpu/shadps4/common/recursive_lock.cpp'
$stub = @'
#pragma once
#include <cstdlib>
#define ASSERT(a) do { if (!(a)) std::abort(); } while (0)
#define ASSERT_MSG(a, ...) do { if (!(a)) std::abort(); } while (0)
'@
$mutations = @(
    @{ Name = 'a nested lock is never taken by the outermost scope'; File = $lock; Old = "g_recursive_locks.push_back({mutex, type, 1});`n    return true;"; New = "g_recursive_locks.push_back({mutex, type, 1});`n    return false;" },
    @{ Name = 'releasing the last lock frees the storage again'; File = $lock; Old = "g_recursive_locks.pop_back();`n            return true;"; New = "g_recursive_locks.pop_back();`n            g_recursive_locks.shrink_to_fit();`n            return true;" },
    @{ Name = 'a held mutex is forgotten on the outermost release'; File = $lock; Old = 'if (--state.count == 0) {'; New = 'if (--state.count == 1) {' }
)
$code = Invoke-HostMutationTest -Name 'runtime_hot_path' -Root $root -Test 'tests/recursive_lock_tests.cpp' `
    -Sources @($lock) -Copy @($lock, 'gpu/shadps4/common/recursive_lock.h') -Mutations $mutations -AssertStub $stub
exit $code
