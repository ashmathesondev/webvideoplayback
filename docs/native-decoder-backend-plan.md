# Native Decoder Backend Plan

## Summary

Move playback behind native decoder and render backends.
Keep FFmpeg as the portable fallback path.
Keep SDL as the cross-platform window and event layer.
Use native hardware decode where each platform supports it well.
Render decoded frames through native surfaces when possible.

The goal is lower CPU use and fewer frame copies.
The player loop should not own backend-specific details.

## Target Backends

Windows:

- Use Media Foundation for native demux and decode.
- Use Direct3D 11 textures for video surfaces.
- Keep SDL for the application window and input.

macOS:

- Use AVFoundation for native media reading.
- Use VideoToolbox for hardware decode.
- Use Metal textures for video surfaces.
- Keep SDL for the application window and input.

Linux:

- Keep FFmpeg as the primary decoder path.
- Add VA-API or Vulkan Video only after support is proven.
- Keep SDL for the application window and input.

Portable fallback:

- Keep the current FFmpeg software decode path.
- Keep SDL texture upload as the fallback render path.
- Preserve URL and local file playback coverage.

## Interfaces

Introduce a decoder interface around stream discovery, decode, timing,
and frame delivery.

```cpp
class IMediaDecoder {
public:
    virtual ~IMediaDecoder() = default;

    virtual MediaInfo media_info() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void seek(double seconds) = 0;
    virtual std::optional<DecodedVideoFrame> pop_video_frame() = 0;
    virtual std::optional<DecodedAudioFrame> pop_audio_frame() = 0;
};
```

Introduce a render sink interface around frame presentation.
The sink receives either CPU frames or native surface handles.

```cpp
class IRenderSink {
public:
    virtual ~IRenderSink() = default;

    virtual void resize(int width, int height) = 0;
    virtual VideoFrameTiming present(const RenderFrame& frame) = 0;
    virtual SDL_Rect current_video_rect() const = 0;
};
```

Keep concrete types narrow:

- `FfmpegMediaDecoder`
- `MediaFoundationDecoder`
- `AvFoundationDecoder`
- `SdlTextureRenderSink`
- `D3d11RenderSink`
- `MetalRenderSink`

The existing playback loop should depend on interfaces.
Backend selection should happen during startup.

## Native Surface Rendering

Prefer zero-copy presentation for native decoder paths.
Decoded frames should stay in GPU-backed surfaces.
The render sink should map those surfaces to the active renderer.

Windows should pass Direct3D 11 texture handles.
macOS should pass Metal-compatible texture objects.
The portable fallback should pass CPU pixel planes.

`RenderFrame` should carry one frame payload variant.
It should also carry presentation timestamp metadata.
The playback loop should not inspect backend surface internals.

## Implementation Steps

1. Extract current FFmpeg decode logic behind `IMediaDecoder`.
2. Extract current SDL video output behind `IRenderSink`.
3. Preserve the current FFmpeg plus SDL path first.
4. Add backend selection by platform and feature flag.
5. Add Media Foundation decode on Windows.
6. Add Direct3D 11 render sink on Windows.
7. Add AVFoundation or VideoToolbox decode on macOS.
8. Add Metal render sink on macOS.
9. Revisit Linux hardware decode after Windows and macOS.
10. Keep FFmpeg fallback available on every platform.

Each step should keep playback runnable.
Avoid mixing native backend work into playback policy.

## Test Plan

Documentation:

- Verify this Markdown renders cleanly.
- Verify the README link targets this file.

Existing playback:

- Play a local MP4 file.
- Play an HTTP MP4 stream from the test server.
- Toggle the debug overlay with `F1`.
- Confirm audio remains the master clock.
- Confirm resize behavior keeps aspect ratio.
- Confirm CSV reporting still writes when enabled.

Backend behavior:

- Confirm backend selection is visible in diagnostics.
- Confirm fallback works when native decode fails.
- Compare CPU use against the current FFmpeg path.
- Confirm frame timing and drop counters remain useful.

Platform coverage:

- Windows native backend smoke test.
- macOS native backend smoke test.
- Linux FFmpeg fallback smoke test.

## Assumptions

- SDL remains the window and input layer.
- FFmpeg remains the portable fallback backend.
- Native APIs need official documentation checks before coding.
- Hardware decode should never be required for playback.
- Backend failures should fall back when format support allows.
- The first implementation should prioritize Windows.
