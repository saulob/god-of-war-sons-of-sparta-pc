<#
.SYNOPSIS
  Deletes the CMake/Ninja build tree (build\cmake). The prepared game, boot image and logs are kept.
#>
[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')
$buildDir = Join-Path $script:RepoRoot 'build\cmake'
if (Test-Path -LiteralPath $buildDir) {
    Write-Step 'Removing build\cmake'
    Remove-Item -LiteralPath $buildDir -Recurse -Force
    Write-Ok 'Removed'
} else {
    Write-Ok 'Nothing to clean (build\cmake does not exist)'
}
