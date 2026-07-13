#pragma once

#include "player/audio_output.hpp"
#include "player/av_support.hpp"
#include "player/playback_pause.hpp"

#include <memory>
#include <optional>
#include <string>

namespace webvideoplayback::player {

enum class DecoderBackendPreference {
    Auto,
    Ffmpeg,
    Native,
};

class IPlaybackDecoder : public IMediaDecoder {
public:
    ~IPlaybackDecoder() override = default;

    virtual bool finished() = 0;
    virtual bool has_audio() const = 0;
    virtual AudioOutput* audio_output() = 0;
    virtual bool has_audio_clock() const = 0;
    virtual bool audio_preroll_ready() const = 0;
    virtual double audio_playback_seconds() const = 0;
};

struct DecoderBackendSelection {
    std::unique_ptr<IPlaybackDecoder> decoder;
    std::string backend_name;
};

DecoderBackendPreference parse_decoder_backend_preference(const std::string& value);
std::string decoder_backend_preference_name(DecoderBackendPreference preference);
DecoderBackendPreference configured_decoder_backend_preference();

DecoderBackendSelection create_decoder_backend(
    const std::string& path,
    double audio_target_ms,
    const PlaybackPause& playback_pause,
    DecoderBackendPreference preference);

} // namespace webvideoplayback::player
