#include "player/media_foundation_decoder.hpp"

#ifdef _WIN32

#include "common/string_utils.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

namespace webvideoplayback::player {
namespace {

void throw_if_failed(HRESULT result, const std::string& context)
{
    if (FAILED(result)) {
        throw std::runtime_error(context + ": HRESULT 0x" + std::to_string(static_cast<unsigned long>(result)));
    }
}

std::string subtype_name(const GUID& subtype)
{
    if (subtype == MFVideoFormat_RGB32) {
        return "RGB32";
    }
    if (subtype == MFVideoFormat_ARGB32) {
        return "ARGB32";
    }
    if (subtype == MFAudioFormat_Float) {
        return "float";
    }
    if (subtype == MFAudioFormat_PCM) {
        return "pcm";
    }
    return "unknown";
}

class ComGuard {
public:
    ComGuard()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE) {
            initialized_ = false;
            return;
        }
        throw_if_failed(result, "initialize COM");
        initialized_ = true;
    }

    ~ComGuard()
    {
        if (initialized_) {
            CoUninitialize();
        }
    }

private:
    bool initialized_ = false;
};

class MediaFoundationGuard {
public:
    MediaFoundationGuard()
    {
        throw_if_failed(MFStartup(MF_VERSION, MFSTARTUP_FULL), "start Media Foundation");
    }

    ~MediaFoundationGuard()
    {
        MFShutdown();
    }
};

} // namespace

struct MediaFoundationDecoder::Impl {
    Impl(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
        : audio_target_ms(audio_target_ms),
          playback_pause(playback_pause)
    {
        static_cast<void>(this->audio_target_ms);
        static_cast<void>(this->playback_pause);

        const std::wstring url = webvideoplayback::utils::utf8_to_wide(path);
        throw_if_failed(MFCreateAttributes(attributes.GetAddressOf(), 1), "create source reader attributes");
        throw_if_failed(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "enable hardware transforms");
        throw_if_failed(MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), reader.GetAddressOf()), "create Media Foundation source reader");

        configure_video();
        detect_audio();

        if (!info.has_video && !info.has_audio) {
            throw std::runtime_error("input has no Media Foundation audio or video streams");
        }
    }

    void configure_video()
    {
        Microsoft::WRL::ComPtr<IMFMediaType> output_type;
        const HRESULT create_type = MFCreateMediaType(output_type.GetAddressOf());
        if (FAILED(create_type)) {
            return;
        }

        if (FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
            || FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))
            || FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output_type.Get()))) {
            return;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> current_type;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), current_type.GetAddressOf()))) {
            return;
        }

        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &width, &height))) {
            return;
        }

        GUID subtype = {};
        current_type->GetGUID(MF_MT_SUBTYPE, &subtype);

        info.has_video = true;
        info.video.codec_name = "media-foundation";
        info.video.pixel_format = subtype_name(subtype);
        info.video_render.width = static_cast<int>(width);
        info.video_render.height = static_cast<int>(height);
        info.video_render.pixel_format = AV_PIX_FMT_BGRA;
    }

    void detect_audio()
    {
        Microsoft::WRL::ComPtr<IMFMediaType> audio_type;
        if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), audio_type.GetAddressOf()))) {
            info.has_audio = true;
        }
    }

    ComGuard com;
    MediaFoundationGuard media_foundation;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    MediaInfo info;
    double audio_target_ms = 150.0;
    const PlaybackPause& playback_pause;
};

MediaFoundationDecoder::MediaFoundationDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
    : impl_(std::make_unique<Impl>(path, audio_target_ms, playback_pause))
{
}

MediaFoundationDecoder::~MediaFoundationDecoder() = default;

MediaInfo MediaFoundationDecoder::media_info() const
{
    return impl_->info;
}

void MediaFoundationDecoder::start()
{
    throw std::runtime_error("Media Foundation frame delivery is not implemented yet");
}

void MediaFoundationDecoder::stop()
{
}

void MediaFoundationDecoder::seek(double seconds)
{
    static_cast<void>(seconds);
    throw std::runtime_error("Media Foundation seeking is not implemented yet");
}

std::optional<DecodedVideoFrame> MediaFoundationDecoder::pop_video_frame()
{
    return std::nullopt;
}

std::optional<DecodedAudioFrame> MediaFoundationDecoder::pop_audio_frame()
{
    return std::nullopt;
}

bool MediaFoundationDecoder::finished() const
{
    return true;
}

bool MediaFoundationDecoder::has_audio() const
{
    return impl_->info.has_audio;
}

AudioOutput* MediaFoundationDecoder::audio_output()
{
    return nullptr;
}

bool MediaFoundationDecoder::has_audio_clock() const
{
    return false;
}

bool MediaFoundationDecoder::audio_preroll_ready() const
{
    return false;
}

double MediaFoundationDecoder::audio_playback_seconds() const
{
    return 0.0;
}

} // namespace webvideoplayback::player

#else

namespace webvideoplayback::player {

MediaFoundationDecoder::MediaFoundationDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
{
    static_cast<void>(path);
    static_cast<void>(audio_target_ms);
    static_cast<void>(playback_pause);
    throw std::runtime_error("Media Foundation is only available on Windows");
}

MediaFoundationDecoder::~MediaFoundationDecoder() = default;
MediaInfo MediaFoundationDecoder::media_info() const { return {}; }
void MediaFoundationDecoder::start() {}
void MediaFoundationDecoder::stop() {}
void MediaFoundationDecoder::seek(double seconds) { static_cast<void>(seconds); }
std::optional<DecodedVideoFrame> MediaFoundationDecoder::pop_video_frame() { return std::nullopt; }
std::optional<DecodedAudioFrame> MediaFoundationDecoder::pop_audio_frame() { return std::nullopt; }
bool MediaFoundationDecoder::finished() const { return true; }
bool MediaFoundationDecoder::has_audio() const { return false; }
AudioOutput* MediaFoundationDecoder::audio_output() { return nullptr; }
bool MediaFoundationDecoder::has_audio_clock() const { return false; }
bool MediaFoundationDecoder::audio_preroll_ready() const { return false; }
double MediaFoundationDecoder::audio_playback_seconds() const { return 0.0; }

} // namespace webvideoplayback::player

#endif
