#pragma once

#include "player/ffmpeg_media_decoder.hpp"
#include "player/playback_pause.hpp"

#include <memory>
#include <string>

namespace webvideoplayback::player {

enum class DecoderBackendPreference {
    Auto,
    Ffmpeg,
    Native,
};

struct DecoderBackendSelection {
    std::unique_ptr<FfmpegMediaDecoder> decoder;
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
