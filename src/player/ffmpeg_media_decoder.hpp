#pragma once

#include "player/av_support.hpp"
#include "player/playback_pause.hpp"
#include "player/sdl_media.hpp"

#include <memory>
#include <optional>
#include <string>

namespace webvideoplayback::player {

class FfmpegMediaDecoder final : public IMediaDecoder {
public:
    FfmpegMediaDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause);
    ~FfmpegMediaDecoder() override;

    MediaInfo media_info() const override;
    void start() override;
    void stop() override;
    void seek(double seconds) override;
    std::optional<DecodedVideoFrame> pop_video_frame() override;
    std::optional<DecodedAudioFrame> pop_audio_frame() override;

    const AVCodecContext& video_codec() const;
    bool finished();
    bool has_audio() const;
    AudioOutput* audio_output();
    bool has_audio_clock() const;
    bool audio_preroll_ready() const;
    double audio_playback_seconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webvideoplayback::player
