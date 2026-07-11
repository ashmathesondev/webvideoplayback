# Web Video Playback

C++ and SDL2 application scaffold for local web media playback.

## Dependencies

This project uses CMake and vcpkg manifest mode.

- SDL2 handles the window, rendering, and audio output.
- FFmpeg handles container demuxing and audio/video decoding.

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

Playback shows a debug overlay at the bottom of the window.

The first implementation targets local files. Network URLs, seeking,
subtitles, playlists, and hardware decoding are natural next steps.
