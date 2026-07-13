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
#include <condition_variable>
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
    if (subtype == MFVideoFormat_YUY2) {
        return "YUY2";
    }
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

int video_source_stride(const VideoRenderConfig& config)
{
    if (config.pixel_format == AV_PIX_FMT_YUYV422) {
        return config.width * 2;
    }
    if (config.pixel_format == AV_PIX_FMT_BGRA) {
        return config.width * 4;
    }
    throw std::runtime_error("unsupported Media Foundation video frame format");
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

class MediaFoundationReaderWorker {
public:
    MediaFoundationReaderWorker(
        Microsoft::WRL::ComPtr<IMFSourceReader> reader,
        MediaInfo media_info,
        DWORD video_stream_index,
        DWORD audio_stream_index,
        double audio_target_ms,
        const PlaybackPause& playback_pause)
        : reader_(std::move(reader)),
          media_info_(std::move(media_info)),
          video_stream_index_(video_stream_index),
          audio_stream_index_(audio_stream_index),
          audio_output_(media_info_.has_audio
              ? std::make_unique<AudioOutput>(
                  static_cast<int>(output_sample_rate_),
                  static_cast<int>(output_channels_),
                  audio_target_ms)
              : nullptr),
          playback_pause_(playback_pause)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~MediaFoundationReaderWorker()
    {
        stop_requested_.store(true);
        frame_space_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::optional<DecodedVideoFrame> pop_video_frame()
    {
        rethrow_if_failed();
        std::lock_guard lock(mutex_);
        if (video_frames_.empty()) {
            return std::nullopt;
        }

        DecodedVideoFrame frame = std::move(video_frames_.front());
        video_frames_.pop_front();
        frame_space_.notify_one();
        return frame;
    }

    bool finished()
    {
        rethrow_if_failed();
        std::lock_guard lock(mutex_);
        return eof_ && video_frames_.empty();
    }

    AudioOutput* output()
    {
        return audio_output_.get();
    }

    bool has_clock() const
    {
        rethrow_if_failed();
        return last_audio_end_seconds_.load() > 0.0;
    }

    bool preroll_ready() const
    {
        rethrow_if_failed();
        return audio_output_ != nullptr
            && last_audio_end_seconds_.load() > 0.0
            && audio_output_->queued_milliseconds() >= audio_output_->target_latency_ms();
    }

    double audio_playback_seconds() const
    {
        rethrow_if_failed();
        return audio_output_ != nullptr ? audio_output_->playback_seconds() : 0.0;
    }

private:
    void run()
    {
        try {
            ComGuard com;

            while (!stop_requested_.load()) {
                playback_pause_.wait(stop_requested_);
                throttle_read_ahead();

                DWORD stream_index = 0;
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                Microsoft::WRL::ComPtr<IMFSample> sample;
                throw_if_failed(
                    reader_->ReadSample(
                        static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM),
                        0,
                        &stream_index,
                        &flags,
                        &timestamp,
                        sample.GetAddressOf()),
                    "read Media Foundation sample");

                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    break;
                }
                if (sample == nullptr) {
                    continue;
                }

                if (media_info_.has_audio && stream_index == audio_stream_index_) {
                    queue_audio_sample(*sample.Get(), timestamp);
                } else if (media_info_.has_video && stream_index == video_stream_index_) {
                    push_video_frame(decode_video_sample(*sample.Get(), timestamp));
                }
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            error_ = std::current_exception();
        }

        {
            std::lock_guard lock(mutex_);
            eof_ = true;
        }
        frame_space_.notify_all();
    }

    void throttle_read_ahead()
    {
        while (!stop_requested_.load()) {
            const bool video_full = [&] {
                std::lock_guard lock(mutex_);
                return video_frames_.size() >= max_video_frames_;
            }();
            const bool audio_full = audio_output_ == nullptr
                || audio_output_->queued_milliseconds() >= max_audio_queue_ms_;
            if (!video_full || !audio_full) {
                return;
            }
            std::unique_lock lock(mutex_);
            frame_space_.wait_for(lock, std::chrono::milliseconds(2), [this] {
                return stop_requested_.load() || video_frames_.size() < max_video_frames_;
            });
        }
    }

    DecodedVideoFrame decode_video_sample(IMFSample& sample, LONGLONG timestamp)
    {
        const auto decode_start = std::chrono::steady_clock::now();
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        throw_if_failed(sample.ConvertToContiguousBuffer(buffer.GetAddressOf()), "copy Media Foundation video sample");

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

            frame->format = media_info_.video_render.pixel_format;
            frame->width = media_info_.video_render.width;
            frame->height = media_info_.video_render.height;
            throw_if_error(av_frame_get_buffer(frame.get(), 32), "allocate Media Foundation video frame buffer");

            const int source_stride = video_source_stride(media_info_.video_render);
            const int required_length = source_stride * media_info_.video_render.height;
            if (current_length < static_cast<DWORD>(required_length)) {
                throw std::runtime_error("Media Foundation video buffer is smaller than expected");
            }

            for (int row = 0; row < media_info_.video_render.height; ++row) {
                std::memcpy(
                    frame->data[0] + row * frame->linesize[0],
                    data + row * source_stride,
                    static_cast<std::size_t>(source_stride));
            }

            throw_if_failed(buffer->Unlock(), "unlock Media Foundation video buffer");
            locked = false;

            const auto decode_end = std::chrono::steady_clock::now();
            DecodedVideoFrame decoded;
            decoded.frame = std::move(frame);
            decoded.media_time_s = static_cast<double>(timestamp) / 10000000.0;
            decoded.decode_ms = elapsed_ms(decode_start, decode_end);
            return decoded;
        } catch (...) {
            if (locked) {
                buffer->Unlock();
            }
            throw;
        }
    }

    void push_video_frame(DecodedVideoFrame frame)
    {
        {
            std::unique_lock lock(mutex_);
            frame_space_.wait(lock, [this] {
                return stop_requested_.load() || video_frames_.size() < max_video_frames_;
            });
            if (stop_requested_.load()) {
                return;
            }
            video_frames_.push_back(std::move(frame));
        }
        frame_space_.notify_one();
    }

    void queue_audio_sample(IMFSample& sample, LONGLONG timestamp)
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

            if (audio_output_ != nullptr) {
                audio_output_->queue_float_pcm(data, current_length);
                const double audio_end_seconds = static_cast<double>(timestamp + duration) / 10000000.0;
                audio_output_->mark_media_end(audio_end_seconds);
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

    static double elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
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
    static constexpr std::size_t max_video_frames_ = 24;
    static constexpr double max_audio_queue_ms_ = 1200.0;

    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    MediaInfo media_info_;
    DWORD video_stream_index_ = 0;
    DWORD audio_stream_index_ = 0;
    const PlaybackPause& playback_pause_;
    mutable std::mutex mutex_;
    std::condition_variable frame_space_;
    std::deque<DecodedVideoFrame> video_frames_;
    std::unique_ptr<AudioOutput> audio_output_;
    std::exception_ptr error_;
    std::atomic_bool stop_requested_ = false;
    bool eof_ = false;
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

        throw_if_failed(
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE),
            "deselect Media Foundation streams");
        throw_if_failed(reader->SetStreamSelection(*stream_index, TRUE), "select Media Foundation video stream");

        HRESULT set_type_result = set_video_output_type(*stream_index, MFVideoFormat_YUY2);
        if (FAILED(set_type_result)) {
            set_type_result = set_video_output_type(*stream_index, MFVideoFormat_RGB32);
        }
        throw_if_failed(set_type_result, "set Media Foundation video output type");

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
        info.video_render.pixel_format = subtype == MFVideoFormat_YUY2 ? AV_PIX_FMT_YUYV422 : AV_PIX_FMT_BGRA;
    }

    HRESULT set_video_output_type(DWORD stream_index, const GUID& subtype)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> output_type;
        HRESULT result = MFCreateMediaType(output_type.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }

        result = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(result)) {
            return result;
        }

        result = output_type->SetGUID(MF_MT_SUBTYPE, subtype);
        if (FAILED(result)) {
            return result;
        }

        return reader->SetCurrentMediaType(stream_index, nullptr, output_type.Get());
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
        if (reader_worker == nullptr) {
            return std::nullopt;
        }
        return reader_worker->pop_video_frame();
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
    std::unique_ptr<MediaFoundationReaderWorker> reader_worker;
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
    if (impl_->reader_worker == nullptr) {
        impl_->reader_worker = std::make_unique<MediaFoundationReaderWorker>(
            impl_->reader,
            impl_->info,
            impl_->video_stream_index,
            impl_->audio_stream_index,
            impl_->audio_target_ms,
            impl_->playback_pause);
    }
}

void MediaFoundationDecoder::stop()
{
    impl_->reader_worker.reset();
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
    return impl_->reader_worker == nullptr || impl_->reader_worker->finished();
}

bool MediaFoundationDecoder::has_audio() const
{
    return impl_->info.has_audio;
}

AudioOutput* MediaFoundationDecoder::audio_output()
{
    return impl_->reader_worker != nullptr ? impl_->reader_worker->output() : nullptr;
}

bool MediaFoundationDecoder::has_audio_clock() const
{
    return impl_->reader_worker != nullptr && impl_->reader_worker->has_clock();
}

bool MediaFoundationDecoder::audio_preroll_ready() const
{
    return impl_->reader_worker != nullptr && impl_->reader_worker->preroll_ready();
}

double MediaFoundationDecoder::audio_playback_seconds() const
{
    return impl_->reader_worker != nullptr ? impl_->reader_worker->audio_playback_seconds() : 0.0;
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
