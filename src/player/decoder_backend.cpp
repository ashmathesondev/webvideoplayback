#include "player/decoder_backend.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace webvideoplayback::player {
namespace {

std::string lower_ascii(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

bool native_backend_available()
{
    return false;
}

} // namespace

DecoderBackendPreference parse_decoder_backend_preference(const std::string& value)
{
    const std::string normalized = lower_ascii(value);
    if (normalized == "auto") {
        return DecoderBackendPreference::Auto;
    }
    if (normalized == "ffmpeg") {
        return DecoderBackendPreference::Ffmpeg;
    }
    if (normalized == "native") {
        return DecoderBackendPreference::Native;
    }

    throw std::runtime_error("decoder backend must be auto, ffmpeg, or native");
}

std::string decoder_backend_preference_name(DecoderBackendPreference preference)
{
    switch (preference) {
    case DecoderBackendPreference::Auto:
        return "auto";
    case DecoderBackendPreference::Ffmpeg:
        return "ffmpeg";
    case DecoderBackendPreference::Native:
        return "native";
    }

    return "auto";
}

DecoderBackendPreference configured_decoder_backend_preference()
{
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, "WEBVIDEOPLAYBACK_DECODER_BACKEND") != 0 || raw_value == nullptr) {
        return DecoderBackendPreference::Auto;
    }

    const std::unique_ptr<char, decltype(&std::free)> value_guard(raw_value, &std::free);
#else
    const char* raw_value = std::getenv("WEBVIDEOPLAYBACK_DECODER_BACKEND");
    if (raw_value == nullptr || raw_value[0] == '\0') {
        return DecoderBackendPreference::Auto;
    }
#endif

    return parse_decoder_backend_preference(raw_value);
}

DecoderBackendSelection create_decoder_backend(
    const std::string& path,
    double audio_target_ms,
    const PlaybackPause& playback_pause,
    DecoderBackendPreference preference)
{
    if (preference == DecoderBackendPreference::Native) {
        if (!native_backend_available()) {
            throw std::runtime_error("native decoder backend is not implemented yet");
        }
    }

    return {
        std::make_unique<FfmpegMediaDecoder>(path, audio_target_ms, playback_pause),
        "ffmpeg",
    };
}

} // namespace webvideoplayback::player
