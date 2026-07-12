<#
.SYNOPSIS
    Builds webvideoplayback targets using CMake.
.DESCRIPTION
    Configures (if needed) and builds the player and/or web server for the
    specified configuration using the windows-msvc-vcpkg preset.
.PARAMETER Configuration
    Build configuration: Debug, Release, or RelWithDebInfo. Default: Debug.
.PARAMETER Target
    Which target(s) to build: All, Player, or Server. Default: All.
.PARAMETER h
    Show this help message.
.EXAMPLE
    .\build.ps1
    .\build.ps1 -Configuration Release
    .\build.ps1 -Configuration RelWithDebInfo -Target Server
#>

param(
    [Alias('?')]
    [switch]$h,

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Debug',

    [ValidateSet('All', 'Player', 'Server')]
    [string]$Target = 'All'
)

$ErrorActionPreference = 'Stop'

if ($h) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$root      = Resolve-Path (Join-Path $PSScriptRoot '..')
$buildDir  = Join-Path $root 'build\windows-msvc-vcpkg'
$preset    = "windows-msvc-vcpkg-$($Configuration.ToLower())"

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    Write-Host "Configuring with preset 'windows-msvc-vcpkg'..."
    cmake --preset windows-msvc-vcpkg -S $root
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }
}

$targetArgs = switch ($Target) {
    'Player' { @('--target', 'webvideoplayback') }
    'Server' { @('--target', 'webvideoplayback_test_server') }
    default  { @() }
}

Write-Host "Building '$Target' [$Configuration]..."
cmake --build --preset $preset @targetArgs
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

Write-Host "Done. Output: $buildDir\$Configuration\"
