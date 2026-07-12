$ErrorActionPreference = 'Stop'

$player = Join-Path $PSScriptRoot '..\build\windows-msvc-vcpkg\Debug\webvideoplayback.exe'
$url = 'http://127.0.0.1:8080/Real.Genius.1985.1080p.BluRay.x264-%5BYTS.AM%5D.mp4'

if (-not (Test-Path -LiteralPath $player)) {
    throw "Player executable not found: $player"
}

& $player $url
