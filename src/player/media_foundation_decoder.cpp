#include "player/media_foundation_decoder.hpp"

#ifdef _WIN32

#include "common/string_utils.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace webvideoplayback::player {
namespace {

void throw_if_failed(HRESULT result, const std::string& context)
{
    if (FAILED(result)) {
        std::ostringstream stream;
        stream << context << ": HRESULT 0x" << std::hex << static_cast<unsigned long>(result);
        throw std::runtime_error(stream.str());
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
        throw_if_failed(MFCreateAttributes(attributes.GetAddressOf(), 2), "create source reader attributes");
        throw_if_failed(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "enable hardware transforms");
        throw_if_failed(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "enable video processing");
        throw_if_failed(MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), reader.GetAddressOf()), "create Media Foundation source reader");

        configure_video();
        detect_audio();

        if (!info.has_video && !info.has_audio) {
            throw std::runtime_error("input has no Media Foundation audio or video streams");
        }
    }

    void configure_video()
    {
        const std::optional<DWORD> stream_index = find_first_stream(MFMediaType_Video);
        if (!stream_index) {
            return;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> output_type;
        const HRESULT create_type = MFCreateMediaType(output_type.GetAddressOf());
        if (FAILED(create_type)) {
            return;
        }

        throw_if_failed(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "set Media Foundation video major type");
        throw_if_failed(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "set Media Foundation RGB32 output type");
        throw_if_failed(
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE),
            "deselect Media Foundation streams");
        throw_if_failed(reader->SetStreamSelection(*stream_index, TRUE), "select Media Foundation video stream");
        throw_if_failed(reader->SetCurrentMediaType(*stream_index, nullptr, output_type.Get()), "set Media Foundation video output type");

        Microsoft::WRL::ComPtr<IMFMediaType> current_type;
        throw_if_failed(reader->GetCurrentMediaType(*stream_index, current_type.GetAddressOf()), "read Media Foundation video output type");

        UINT32 width = 0;
        UINT32 height = 0;
        throw_if_failed(MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &width, &height), "read Media Foundation video frame size");

        GUID subtype = {};
        current_type->GetGUID(MF_MT_SUBTYPE, &subtype);

        info.has_video = true;
        video_stream_index = *stream_index;
        info.video.codec_name = "media-foundation";
        info.video.pixel_format = subtype_name(subtype);
        info.video_render.width = static_cast<int>(width);
        info.video_render.height = static_cast<int>(height);
        info.video_render.pixel_format = AV_PIX_FMT_BGRA;
    }

    void detect_audio()
    {
        if (find_first_stream(MFMediaType_Audio)) {
            info.has_audio = false;
        }
    }

    std::optional<DWORD> find_first_stream(const GUID& major_type)
    {
        for (DWORD stream_index = 0;; ++stream_index) {
            Microsoft::WRL::ComPtr<IMFMediaType> native_type;
            const HRESULT result = reader->GetNativeMediaType(stream_index, 0, native_type.GetAddressOf());
            if (result == MF_E_INVALIDSTREAMNUMBER) {
                return std::nullopt;
            }
            if (FAILED(result)) {
                continue;
            }

            GUID stream_major_type = {};
            if (FAILED(native_type->GetGUID(MF_MT_MAJOR_TYPE, &stream_major_type))) {
                continue;
            }
            if (stream_major_type == major_type) {
                return stream_index;
            }
        }
    }

    std::optional<DecodedVideoFrame> read_video_frame()
    {
        if (!info.has_video || eof) {
            return std::nullopt;
        }

        if (playback_pause.paused()) {
            return std::nullopt;
        }

        const auto start = std::chrono::steady_clock::now();
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        throw_if_failed(
            reader->ReadSample(
                video_stream_index,
                0,
                &stream_index,
                &flags,
                &timestamp,
                sample.GetAddressOf()),
            "read Media Foundation video sample");
        const auto read_done = std::chrono::steady_clock::now();

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            eof = true;
            return std::nullopt;
        }

        if (sample == nullptr) {
            return std::nullopt;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        throw_if_failed(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()), "copy Media Foundation video sample");

        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        throw_if_failed(buffer->Lock(&data, &max_length, &current_length), "lock Media Foundation video buffer");
        static_cast<void>(max_length);
        bool locked = true;

        try {
            FramePtr frame(av_frame_alloc());
            if (frame == nullptr) {
                throw std::runtime_error("allocate Media Foundation video frame");
            }

            frame->format = info.video_render.pixel_format;
            frame->width = info.video_render.width;
            frame->height = info.video_render.height;
            throw_if_error(av_frame_get_buffer(frame.get(), 32), "allocate Media Foundation video frame buffer");

            const int source_stride = info.video_render.width * 4;
            const int required_length = source_stride * info.video_render.height;
            if (current_length < static_cast<DWORD>(required_length)) {
                throw std::runtime_error("Media Foundation video buffer is smaller than expected");
            }

            for (int row = 0; row < info.video_render.height; ++row) {
                std::memcpy(
                    frame->data[0] + row * frame->linesize[0],
                    data + row * source_stride,
                    static_cast<std::size_t>(source_stride));
            }

            throw_if_failed(buffer->Unlock(), "unlock Media Foundation video buffer");
            locked = false;

            const auto copy_done = std::chrono::steady_clock::now();
            DecodedVideoFrame decoded;
            decoded.frame = std::move(frame);
            decoded.media_time_s = static_cast<double>(timestamp) / 10000000.0;
            decoded.packet_timing.demux_ms = elapsed_ms(start, read_done);
            decoded.decode_ms = elapsed_ms(read_done, copy_done);
            return decoded;
        } catch (...) {
            if (locked) {
                buffer->Unlock();
            }
            throw;
        }
    }

    static double elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    ComGuard com;
    MediaFoundationGuard media_foundation;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    MediaInfo info;
    DWORD video_stream_index = 0;
    double audio_target_ms = 150.0;
    const PlaybackPause& playback_pause;
    bool eof = false;
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
    return impl_->read_video_frame();
}

std::optional<DecodedAudioFrame> MediaFoundationDecoder::pop_audio_frame()
{
    return std::nullopt;
}

bool MediaFoundationDecoder::finished()
{
    return impl_->eof;
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
bool MediaFoundationDecoder::finished() { return true; }
bool MediaFoundationDecoder::has_audio() const { return false; }
AudioOutput* MediaFoundationDecoder::audio_output() { return nullptr; }
bool MediaFoundationDecoder::has_audio_clock() const { return false; }
bool MediaFoundationDecoder::audio_preroll_ready() const { return false; }
double MediaFoundationDecoder::audio_playback_seconds() const { return 0.0; }

} // namespace webvideoplayback::player

#endif
