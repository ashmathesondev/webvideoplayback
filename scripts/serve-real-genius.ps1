$ErrorActionPreference = 'Stop'

$server = Join-Path $PSScriptRoot '..\build\windows-msvc-vcpkg\Debug\webvideoplayback_test_server.exe'
$root = 'I:\Movies\Real Genius (1985) [BluRay] [1080p] [YTS.AM]'
$port = '8080'

if (-not (Test-Path -LiteralPath $server)) {
    throw "Server executable not found: $server"
}

if (-not (Test-Path -LiteralPath $root)) {
    throw "Media folder not found: $root"
}

& $server --root $root --port $port
