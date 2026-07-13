#pragma once

#include "player/decoder_backend.hpp"
#include "player/playback_pause.hpp"
#include "player/sdl_media.hpp"

#include <memory>
#include <optional>
#include <string>

namespace webvideoplayback::player {

class MediaFoundationDecoder final : public IPlaybackDecoder {
public:
    MediaFoundationDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause);
    ~MediaFoundationDecoder() override;

    MediaInfo media_info() const override;
    void start() override;
    void stop() override;
    void seek(double seconds) override;
    std::optional<DecodedVideoFrame> pop_video_frame() override;
    std::optional<DecodedAudioFrame> pop_audio_frame() override;

    bool finished() override;
    bool has_audio() const override;
    AudioOutput* audio_output() override;
    bool has_audio_clock() const override;
    bool audio_preroll_ready() const override;
    double audio_playback_seconds() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webvideoplayback::player
