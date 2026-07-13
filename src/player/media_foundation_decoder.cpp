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
#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

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

class MediaFoundationAudioWorker {
public:
    MediaFoundationAudioWorker(
        std::string path,
        DWORD audio_stream_index,
        double audio_target_ms,
        const PlaybackPause& playback_pause)
        : path_(std::move(path)),
          audio_stream_index_(audio_stream_index),
          audio_target_ms_(audio_target_ms),
          playback_pause_(playback_pause)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~MediaFoundationAudioWorker()
    {
        stop_requested_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    AudioOutput* output()
    {
        rethrow_if_failed();
        std::lock_guard lock(mutex_);
        return output_.get();
    }

    bool has_clock() const
    {
        rethrow_if_failed();
        return last_audio_end_seconds_.load() > 0.0;
    }

    bool preroll_ready() const
    {
        rethrow_if_failed();
        std::lock_guard lock(mutex_);
        return output_ != nullptr
            && last_audio_end_seconds_.load() > 0.0
            && output_->queued_milliseconds() >= output_->target_latency_ms();
    }

    double audio_playback_seconds() const
    {
        rethrow_if_failed();
        std::lock_guard lock(mutex_);
        return output_ != nullptr ? output_->playback_seconds() : 0.0;
    }

    bool finished() const
    {
        rethrow_if_failed();
        return eof_.load();
    }

private:
    void run()
    {
        try {
            ComGuard com;
            MediaFoundationGuard media_foundation;
            Microsoft::WRL::ComPtr<IMFAttributes> attributes;
            Microsoft::WRL::ComPtr<IMFSourceReader> reader;
            const std::wstring url = webvideoplayback::utils::utf8_to_wide(path_);

            throw_if_failed(MFCreateAttributes(attributes.GetAddressOf(), 1), "create Media Foundation audio attributes");
            throw_if_failed(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "enable audio hardware transforms");
            throw_if_failed(MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), reader.GetAddressOf()), "create Media Foundation audio reader");
            configure_audio_reader(*reader.Get());

            while (!stop_requested_.load()) {
                playback_pause_.wait(stop_requested_);

                DWORD stream_index = 0;
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                Microsoft::WRL::ComPtr<IMFSample> sample;
                throw_if_failed(
                    reader->ReadSample(audio_stream_index_, 0, &stream_index, &flags, &timestamp, sample.GetAddressOf()),
                    "read Media Foundation audio sample");

                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    break;
                }
                if (sample == nullptr) {
                    continue;
                }

                queue_sample(*sample.Get(), timestamp);
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            error_ = std::current_exception();
        }

        eof_.store(true);
    }

    void configure_audio_reader(IMFSourceReader& reader)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> output_type;
        throw_if_failed(MFCreateMediaType(output_type.GetAddressOf()), "create Media Foundation audio output type");
        throw_if_failed(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "set Media Foundation audio major type");
        throw_if_failed(output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float), "set Media Foundation float audio type");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, output_channels_), "set Media Foundation audio channels");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, output_sample_rate_), "set Media Foundation audio sample rate");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32), "set Media Foundation audio bits");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, output_channels_ * sizeof(float)), "set Media Foundation audio alignment");
        throw_if_failed(
            output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, output_sample_rate_ * output_channels_ * sizeof(float)),
            "set Media Foundation audio byte rate");

        throw_if_failed(
            reader.SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE),
            "deselect Media Foundation audio reader streams");
        throw_if_failed(reader.SetStreamSelection(audio_stream_index_, TRUE), "select Media Foundation audio stream");
        throw_if_failed(reader.SetCurrentMediaType(audio_stream_index_, nullptr, output_type.Get()), "set Media Foundation audio output type");

        std::lock_guard lock(mutex_);
        output_ = std::make_unique<AudioOutput>(
            static_cast<int>(output_sample_rate_),
            static_cast<int>(output_channels_),
            audio_target_ms_);
    }

    void queue_sample(IMFSample& sample, LONGLONG timestamp)
    {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        throw_if_failed(sample.ConvertToContiguousBuffer(buffer.GetAddressOf()), "copy Media Foundation audio sample");

        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        throw_if_failed(buffer->Lock(&data, &max_length, &current_length), "lock Media Foundation audio buffer");
        static_cast<void>(max_length);
        bool locked = true;

        try {
            LONGLONG duration = 0;
            if (FAILED(sample.GetSampleDuration(&duration))) {
                const double bytes_per_second = static_cast<double>(output_sample_rate_ * output_channels_ * sizeof(float));
                duration = static_cast<LONGLONG>((static_cast<double>(current_length) / bytes_per_second) * 10000000.0);
            }

            std::lock_guard lock(mutex_);
            if (output_ != nullptr) {
                output_->queue_float_pcm(data, current_length);
                const double audio_end_seconds = static_cast<double>(timestamp + duration) / 10000000.0;
                output_->mark_media_end(audio_end_seconds);
                last_audio_end_seconds_.store(audio_end_seconds);
            }

            throw_if_failed(buffer->Unlock(), "unlock Media Foundation audio buffer");
            locked = false;
        } catch (...) {
            if (locked) {
                buffer->Unlock();
            }
            throw;
        }
    }

    void rethrow_if_failed() const
    {
        std::lock_guard lock(mutex_);
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

    static constexpr UINT32 output_sample_rate_ = 48000;
    static constexpr UINT32 output_channels_ = 2;

    std::string path_;
    DWORD audio_stream_index_ = 0;
    double audio_target_ms_ = 150.0;
    const PlaybackPause& playback_pause_;
    mutable std::mutex mutex_;
    std::unique_ptr<AudioOutput> output_;
    std::exception_ptr error_;
    std::atomic_bool stop_requested_ = false;
    std::atomic_bool eof_ = false;
    std::atomic<double> last_audio_end_seconds_ = 0.0;
    std::thread worker_;
};

} // namespace

struct MediaFoundationDecoder::Impl {
    Impl(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
        : path(path),
          audio_target_ms(audio_target_ms),
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
        const std::optional<DWORD> stream_index = find_first_stream(MFMediaType_Audio);
        if (stream_index) {
            configure_audio(*stream_index);
            info.has_audio = true;
            audio_stream_index = *stream_index;
        }
    }

    void configure_audio(DWORD stream_index)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> output_type;
        throw_if_failed(MFCreateMediaType(output_type.GetAddressOf()), "create Media Foundation audio output type");
        throw_if_failed(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "set Media Foundation audio major type");
        throw_if_failed(output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float), "set Media Foundation float audio type");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, output_channels), "set Media Foundation audio channels");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, output_sample_rate), "set Media Foundation audio sample rate");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32), "set Media Foundation audio bits");
        throw_if_failed(output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, output_channels * sizeof(float)), "set Media Foundation audio alignment");
        throw_if_failed(
            output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, output_sample_rate * output_channels * sizeof(float)),
            "set Media Foundation audio byte rate");
        throw_if_failed(reader->SetStreamSelection(stream_index, TRUE), "select Media Foundation audio stream");
        throw_if_failed(reader->SetCurrentMediaType(stream_index, nullptr, output_type.Get()), "set Media Foundation audio output type");
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
        if (!queued_video_frames.empty()) {
            DecodedVideoFrame frame = std::move(queued_video_frames.front());
            queued_video_frames.pop_front();
            return frame;
        }

        if (!info.has_video || eof) {
            return std::nullopt;
        }

        if (playback_pause.paused()) {
            return std::nullopt;
        }

        while (!eof) {
            std::optional<DecodedVideoFrame> frame = read_next_sample();
            if (frame) {
                return frame;
            }
        }

        return std::nullopt;
    }

    void pump_audio_preroll()
    {
        if (!info.has_audio || audio_output == nullptr || eof) {
            return;
        }

        while (!eof && audio_output->queued_milliseconds() < audio_output->target_latency_ms()) {
            std::optional<DecodedVideoFrame> frame = read_next_sample();
            if (frame) {
                queued_video_frames.push_back(std::move(*frame));
            }
        }
    }

    std::optional<DecodedVideoFrame> read_next_sample()
    {
        const auto start = std::chrono::steady_clock::now();
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        throw_if_failed(
            reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM),
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

        if (info.has_audio && stream_index == audio_stream_index) {
            queue_audio_sample(*sample.Get(), timestamp);
            return std::nullopt;
        }

        if (!info.has_video || stream_index != video_stream_index) {
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

    void queue_audio_sample(IMFSample& sample, LONGLONG timestamp)
    {
        if (audio_output == nullptr) {
            return;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        throw_if_failed(sample.ConvertToContiguousBuffer(buffer.GetAddressOf()), "copy Media Foundation audio sample");

        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        throw_if_failed(buffer->Lock(&data, &max_length, &current_length), "lock Media Foundation audio buffer");
        static_cast<void>(max_length);
        bool locked = true;

        try {
            LONGLONG duration = 0;
            if (FAILED(sample.GetSampleDuration(&duration))) {
                const double bytes_per_second = static_cast<double>(output_sample_rate * output_channels * sizeof(float));
                duration = static_cast<LONGLONG>((static_cast<double>(current_length) / bytes_per_second) * 10000000.0);
            }

            audio_output->queue_float_pcm(data, current_length);
            const double audio_end_seconds = static_cast<double>(timestamp + duration) / 10000000.0;
            audio_output->mark_media_end(audio_end_seconds);
            last_audio_end_seconds = audio_end_seconds;

            throw_if_failed(buffer->Unlock(), "unlock Media Foundation audio buffer");
            locked = false;
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

    static constexpr UINT32 output_sample_rate = 48000;
    static constexpr UINT32 output_channels = 2;

    ComGuard com;
    MediaFoundationGuard media_foundation;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    MediaInfo info;
    DWORD video_stream_index = 0;
    DWORD audio_stream_index = 0;
    std::string path;
    double audio_target_ms = 150.0;
    const PlaybackPause& playback_pause;
    std::unique_ptr<MediaFoundationAudioWorker> audio_worker;
    std::unique_ptr<AudioOutput> audio_output;
    std::deque<DecodedVideoFrame> queued_video_frames;
    double last_audio_end_seconds = 0.0;
    bool eof = false;
};

MediaFoundationDecoder::MediaFoundationDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
    : impl_(std::make_unique<Impl>(path, audio_target_ms, playback_pause))
{
}

MediaFoundationDecoder::~MediaFoundationDecoder()
{
    stop();
}

MediaInfo MediaFoundationDecoder::media_info() const
{
    return impl_->info;
}

void MediaFoundationDecoder::start()
{
    if (impl_->info.has_audio && impl_->audio_output == nullptr) {
        impl_->audio_output = std::make_unique<AudioOutput>(
            static_cast<int>(impl_->output_sample_rate),
            static_cast<int>(impl_->output_channels),
            impl_->audio_target_ms);
    }
}

void MediaFoundationDecoder::stop()
{
    impl_->audio_worker.reset();
    impl_->audio_output.reset();
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
    return impl_->eof && impl_->queued_video_frames.empty();
}

bool MediaFoundationDecoder::has_audio() const
{
    return impl_->info.has_audio;
}

AudioOutput* MediaFoundationDecoder::audio_output()
{
    return impl_->audio_output.get();
}

bool MediaFoundationDecoder::has_audio_clock() const
{
    return impl_->audio_output != nullptr && impl_->last_audio_end_seconds > 0.0;
}

bool MediaFoundationDecoder::audio_preroll_ready() const
{
    impl_->pump_audio_preroll();
    return impl_->audio_output != nullptr
        && impl_->last_audio_end_seconds > 0.0
        && impl_->audio_output->queued_milliseconds() >= impl_->audio_output->target_latency_ms();
}

double MediaFoundationDecoder::audio_playback_seconds() const
{
    return impl_->audio_output != nullptr ? impl_->audio_output->playback_seconds() : 0.0;
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
