# Code Review Report — webvideoplayback

**Scope:** Full codebase — player + server
**Focus:** C++20 compliance, memory/resource leaks, correctness, performance
**Last updated:** 2026-07-13

---

## Changes since previous review

New since 2026-07-12:
- `sdl_media.hpp` decomposed into `audio_output.{hpp,cpp}`, `render_sink.{hpp,cpp}`, `sdl_platform.{hpp,cpp}`
- `MediaFoundationDecoder` native Windows backend added
- `decoder_backend.{hpp,cpp}` selection layer added (`--decoder-backend auto|ffmpeg|native`)
- `string_utils.cpp` — `utf8_to_wide` added

**Resolved findings:** none — all previous findings remain open.

**New findings:** #9 (MF render-thread blocking I/O), #10 (dead `MediaFoundationAudioWorker` class).

**Updated file references:** findings #2 and #4 moved from `sdl_media.hpp` to `audio_output.cpp` and `render_sink.cpp` respectively.

---

## Finding 1 — Detached thread has no try/catch; uncaught exception calls `std::terminate`

**File:** `src/server/server.cpp:776`
**Severity:** Critical

```cpp
std::thread([client = std::move(client), config, stats] {
    handle_client(client.get(), config, stats);  // no try/catch
}).detach();
```

`handle_client` is not wrapped. `send_file` calls `std::filesystem::file_size(path)` at line 509 — which throws `filesystem_error` if the file is deleted between the `is_regular_file` check and the actual open. That exception propagates into the detached thread and calls `std::terminate`. On terminate, `Socket`'s destructor does not run (no stack unwind), `ActiveRequest`'s destructor does not run (`active_requests` never decremented), and the SOCKET handle leaks.

**Fix:** Wrap `handle_client(...)` in `try/catch(...)` inside the lambda.

---

## Finding 2 — `swr_get_out_samples` negative return used as `buffer_.resize()` argument

**File:** `src/player/audio_output.cpp:109` *(moved from `sdl_media.hpp`)*
**Severity:** High (crash in audio thread)

```cpp
const int output_samples = swr_get_out_samples(impl_->resampler.get(), frame.nb_samples);
const int buffer_size = output_samples * Impl::output_channels * bytes_per_sample;
impl_->buffer.resize(static_cast<std::size_t>(buffer_size));  // negative int → huge size_t
```

`swr_get_out_samples` returns a negative error code on failure. No sign check is performed. Casting a negative `int` to `std::size_t` wraps to ~`2^64`, causing `std::bad_alloc`. This exception is unhandled inside `AudioDecoderWorker::run()`, which has no try/catch, causing `std::terminate`.

**Fix:** `if (output_samples < 0) throw std::runtime_error(...)` before the multiply.

---

## Finding 3 — `weakly_canonical` does not resolve symlinks; path-traversal via in-root symlinks

**File:** `src/server/server.cpp:392`
**Severity:** High (security)

```cpp
const std::filesystem::path requested = std::filesystem::weakly_canonical(root_path / path(target));
// prefix comparison follows
```

`weakly_canonical` resolves `..` lexically but **does not follow symlinks**. A symlink placed inside `root` pointing outside it produces a `requested` path still prefixed by `root`, so the prefix check passes and the symlink target is served. NTFS junctions have identical behavior.

**Fix:** Use `std::filesystem::canonical` (which follows symlinks) or explicitly check that the resolved path is not a symlink before serving. Note: `canonical` throws if the path does not exist; handle that.

---

## Finding 4 — `SDL_GetRendererOutputSize` called 4× per rendered frame

**File:** `src/player/render_sink.cpp:394, 434, 441, 449` *(moved from `sdl_media.hpp`)*
**Severity:** Medium (performance)

`present()` calls it once internally (line 394). Then the render loop in `player.cpp` calls `current_video_rect()` (line 434), `window_width()` (line 441), and `window_height()` (line 449) in sequence — each independently calling `SDL_GetRendererOutputSize`. That is 4 SDL/driver round-trips per frame to read values that cannot change mid-frame. At 60fps this is 240 redundant calls/sec.

**Fix:** Cache the output size inside `present()` and expose it as a member — or compute all three values from a single call in the render loop.

---

## Finding 5 — `url_decode` allocates `std::istringstream` per `%XX` byte

**File:** `src/common/string_utils.cpp:60`
**Severity:** Medium (performance)

```cpp
std::istringstream stream{std::string(hex)};  // heap alloc per encoded byte
stream >> std::hex >> byte;
```

`<charconv>` is already `#include`d and `std::from_chars` is used elsewhere in the same file (line 45). Each `%XX` escape allocates a `std::string` and `std::istringstream`; `from_chars` with base 16 parses the same two bytes with zero allocation.

**Fix:**
```cpp
unsigned int byte = 0;
auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + 2, byte, 16);
if (ec == std::errc()) { ... }
```

---

## Finding 6 — `VideoDecoderWorker` destructor signals condition variables without holding `mutex_`

**File:** `src/player/ffmpeg_media_decoder.cpp:173`
**Severity:** Low (latent correctness hazard)

```cpp
// VideoDecoderWorker dtor — no lock:
stop_requested_.store(true);
frame_available_.notify_all();

// AudioDecoderWorker dtor — acquires lock:
{ std::lock_guard lock(mutex_); stop_requested_.store(true); }
queue_changed_.notify_all();
```

Currently safe because predicates check `stop_requested_.load()`. If `stop_requested_` is ever changed from `atomic_bool` to a plain bool protected by the mutex (a natural refactor), `VideoDecoderWorker` gets a data race. The inconsistency creates a trap for future maintainers.

**Fix:** Adopt the same pattern as `AudioDecoderWorker` — acquire the lock before storing `stop_requested_`.

---

## Finding 7 — `eof_` set unconditionally after `catch` block; exception masked for audio-only files

**File:** `src/player/ffmpeg_media_decoder.cpp:303`
**Severity:** Low (silent error drop)

```cpp
} catch (...) {
    std::lock_guard lock(mutex_);
    error_ = std::current_exception();
}
{
    std::lock_guard lock(mutex_);
    eof_ = true;  // always set, even after exception
}
```

On the first call to `finished()` after an exception, the method re-throws via `error_`. However, any future caller that calls `finished()` once, observes `true`, and breaks without a second call will silently swallow the stored exception.

---

## Finding 8 — Latent: `av_read_frame` called on shared `AVFormatContext` from two thread classes

**File:** `src/player/ffmpeg_media_decoder.cpp:273, 372`
**Severity:** Low (latent — currently masked)

`VideoDecoderWorker::run()` and `AudioDemuxWorker::run()` both hold a reference to the same `AVFormatContext` and call `av_read_frame` on it. Currently the `else if` at line 484 ensures only one of them is instantiated at a time. If a seek path, stream switching, or combined audio+video audio-only demux is ever added, concurrent `av_read_frame` calls on a non-thread-safe demuxer will cause heap corruption inside libavformat.

**Fix:** Enforce the mutual-exclusion invariant with a comment at the construction site, or restructure to a single demux thread that distributes packets to decoder workers via queues (the canonical FFmpeg pipeline pattern).

---

## Finding 9 — `MediaFoundationDecoder` blocks the render thread on synchronous `ReadSample` I/O

**File:** `src/player/media_foundation_decoder.cpp:458`
**Severity:** High (new)

```cpp
// Called from pop_video_frame() → read_video_frame() → read_next_sample() — on the main/render thread:
throw_if_failed(
    reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &stream_index, &flags, &timestamp, ...),
    "read Media Foundation video sample");
```

`IMFSourceReader::ReadSample` in synchronous mode blocks until the pipeline delivers a sample or EOF. The FFmpeg backend isolates this I/O on a dedicated `VideoDecoderWorker` thread. The MF backend calls it directly from `pop_video_frame()`, which the render loop calls on the main thread. Any pipeline stall (network, disk seek, codec decompression) freezes the event loop, skips SDL event processing, and causes the window to become unresponsive.

The `pump_audio_buffer()` loop (line 441–449) compounds this — it calls `read_next_sample()` in a `while` loop until the audio buffer is filled, potentially making dozens of blocking calls before returning control.

**Fix:** Move `ReadSample` onto a dedicated decode thread (matching the FFmpeg pattern). Feed decoded frames into a bounded queue; `pop_video_frame()` dequeues non-blocking.

---

## Finding 10 — `MediaFoundationAudioWorker` is dead code; `Impl::audio_worker` is never assigned

**File:** `src/player/media_foundation_decoder.cpp:95`
**Severity:** Low (dead code / maintenance)

`MediaFoundationAudioWorker` (~190 lines) is defined in the anonymous namespace and has a corresponding `std::unique_ptr<MediaFoundationAudioWorker> audio_worker` member in `Impl` (line 602). However `audio_worker` is never assigned anywhere — not in the constructor, not in `start()`, not in any other method. `stop()` calls `audio_worker.reset()` which is always a no-op.

Audio is instead handled inline by `read_next_sample()` / `queue_audio_sample()` on the main thread. The worker class and its 190 lines of code, its own `IMFSourceReader`, its own COM/MF guards, and its mutex are entirely unused.

**Fix:** Delete `MediaFoundationAudioWorker` and the `audio_worker` member from `Impl`.

---

## Summary

| # | File | Line | Severity | Category | Status |
|---|------|------|----------|----------|--------|
| 1 | `src/server/server.cpp` | 776 | Critical | Crash / resource leak | Open |
| 2 | `src/player/audio_output.cpp` | 109 | High | Crash in audio thread | Open |
| 3 | `src/server/server.cpp` | 392 | High | Path traversal (security) | Open |
| 9 | `src/player/media_foundation_decoder.cpp` | 458 | High | Render thread blocked on I/O | **New** |
| 4 | `src/player/render_sink.cpp` | 394+ | Medium | Performance | Open |
| 5 | `src/common/string_utils.cpp` | 60 | Medium | Performance | Open |
| 6 | `src/player/ffmpeg_media_decoder.cpp` | 173 | Low | Latent correctness | Open |
| 7 | `src/player/ffmpeg_media_decoder.cpp` | 303 | Low | Silent error drop | Open |
| 8 | `src/player/ffmpeg_media_decoder.cpp` | 273 | Low | Latent thread safety | Open |
| 10 | `src/player/media_foundation_decoder.cpp` | 95 | Low | Dead code | **New** |

No C++20 compliance issues found. The codebase uses C++20 features correctly (`std::from_chars`, structured bindings, `std::atomic<double>`, `std::optional`). The `FF_THREAD_FRAME | FF_THREAD_SLICE` combination is valid per the FFmpeg API.
