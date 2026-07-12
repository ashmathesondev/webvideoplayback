$ErrorActionPreference = 'Stop'

$player = Join-Path $PSScriptRoot '..\build\windows-msvc-vcpkg\Debug\webvideoplayback.exe'
$url = 'http://127.0.0.1:8080/real-genius.mp4'

if (-not (Test-Path -LiteralPath $player)) {
    throw "Player executable not found: $player"
}

& $player $url
