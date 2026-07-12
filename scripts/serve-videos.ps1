$ErrorActionPreference = 'Stop'

$server = Join-Path $PSScriptRoot '..\build\windows-msvc-vcpkg\Debug\webvideoplayback_test_server.exe'
$config = Join-Path $PSScriptRoot '..\real-genius-server.json'

if (-not (Test-Path -LiteralPath $server)) {
    throw "Server executable not found: $server"
}

if (-not (Test-Path -LiteralPath $config)) {
    throw "Server config not found: $config"
}

& $server --config $config
