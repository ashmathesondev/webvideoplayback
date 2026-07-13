#pragma once

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <atomic>
#include <cstdint>
#include <memory>

namespace webvideoplayback::player {

class AudioOutput {
public:
    AudioOutput(const AVCodecContext& codec, double target_latency_ms);
    ~AudioOutput();

    void queue(const AVFrame& frame);
    void mark_media_end(double media_end_seconds);
    double playback_seconds() const;
    double clock_remaining_seconds() const;
    Uint32 queued_size() const;
    double queued_milliseconds() const;
    int sample_rate() const;
    int channels() const;
    void pause();
    void resume();
    void wait_until_below(double queued_ms, const std::atomic_bool& stop_requested);
    double target_latency_ms() const;
    std::uint64_t underruns() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webvideoplayback::player
