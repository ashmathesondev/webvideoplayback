# Web Video Playback

C++ and SDL2 application scaffold for local web media playback.

## Dependencies

This project uses CMake and vcpkg manifest mode.

- SDL2 handles the window, rendering, and audio output.
- FFmpeg handles container demuxing and audio/video decoding.
- FTXUI renders the test server console dashboard.
- nlohmann-json parses the test server config file.

## Build

```powershell
cmake --preset windows-msvc-vcpkg
cmake --build --preset windows-msvc-vcpkg-debug
```

## Run

```powershell
.\build\windows-msvc-vcpkg\Debug\webvideoplayback.exe
```

Launch the executable. The app opens a native file picker.
Pass a URL or file path to skip the file picker.

```powershell
.\build\windows-msvc-vcpkg\Debug\webvideoplayback.exe http://127.0.0.1:8080/sample.mp4
```

Press `F1` to toggle the debug overlay.
Performance CSV files are written to `reports/`.
CSV reporting is off by default.
Enable it with `--performance-report`.
`--perf-report` also works.
Set `WEBVIDEOPLAYBACK_AUDIO_LATENCY_MS` to tune queued audio.
The default target is `150`.

Release build output:

```powershell
.\build\windows-msvc-vcpkg\Release\webvideoplayback.exe
```

## Test Video Server

The build also creates a loopback HTTP server for playback tests.
It supports `GET`, `HEAD`, `OPTIONS`, byte ranges, and CORS.
It uses an FTXUI console dashboard with active stream progress,
server totals, and recent output events.

```powershell
.\build\windows-msvc-vcpkg\Debug\webvideoplayback_test_server.exe --root E:\path\to\media --port 8080
```

Or serve an explicit file list:

```powershell
.\build\windows-msvc-vcpkg\Debug\webvideoplayback_test_server.exe --config .\server-config.example.json
```

Open media through URLs like:

```text
http://127.0.0.1:8080/sample.mp4
```

Press `Q` in the server terminal to stop it.

For configured local videos, run:

```powershell
.\scripts\serve-videos.ps1
```

Then play it from the test server:

```powershell
.\scripts\play-real-genius-stream.ps1
```

Or play The Last Starfighter:

```powershell
.\scripts\play-starfighter.ps1
```

The Real Genius route is:

```text
http://127.0.0.1:8080/real-genius.mp4
```

The Last Starfighter route is:

```text
http://127.0.0.1:8080/the-last-starfighter.mp4
```

Seeking, subtitles, playlists, and hardware decoding are natural next steps.

## Note about assets

I have local copies of the Real Genius and The Last StarFighter videos.
They are not included in this repository as they are copyrighted.
