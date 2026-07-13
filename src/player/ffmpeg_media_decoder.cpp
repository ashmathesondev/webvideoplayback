#include "player/ffmpeg_media_decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace webvideoplayback::player {
namespace {

void decode_audio_packet(
    AVCodecContext& codec,
    const AVStream& stream,
    const AVPacket& packet,
    AudioOutput& output,
    std::atomic<double>& last_audio_end_seconds)
{
    throw_if_error(avcodec_send_packet(&codec, &packet), "send audio packet");

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::runtime_error("failed to allocate audio frame");
    }

    while (true) {
        const int result = avcodec_receive_frame(&codec, frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        throw_if_error(result, "receive audio frame");
        output.queue(*frame);
        const double audio_end_seconds = frame_end_seconds(*frame, stream);
        output.mark_media_end(audio_end_seconds);
        last_audio_end_seconds.store(audio_end_seconds);
        av_frame_unref(frame.get());
    }
}

class AudioDecoderWorker {
public:
    AudioDecoderWorker(CodecContextPtr codec, const AVStream& stream, double target_latency_ms, const PlaybackPause& playback_pause)
        : codec_(std::move(codec)), stream_(stream), output_(*codec_, target_latency_ms), playback_pause_(playback_pause)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~AudioDecoderWorker()
    {
        {
            std::lock_guard lock(mutex_);
            stop_requested_.store(true);
        }
        queue_changed_.notify_all();
        queue_space_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void push(const AVPacket& packet)
    {
        AVPacket* cloned = av_packet_clone(&packet);
        if (cloned == nullptr) {
            throw std::runtime_error("failed to clone audio packet");
        }

        PacketPtr packet_copy(cloned);
        std::unique_lock lock(mutex_);
        queue_space_.wait(lock, [this] {
            return stop_requested_.load() || packet_queue_.size() < max_packets_;
        });
        if (stop_requested_.load()) {
            return;
        }

        packet_queue_.push_back(std::move(packet_copy));
        lock.unlock();
        queue_changed_.notify_one();
    }

    AudioOutput& output()
    {
        return output_;
    }

    double audio_playback_seconds() const
    {
        return output_.playback_seconds();
    }

    bool has_clock() const
    {
        return last_audio_end_seconds_.load() > 0.0;
    }

    bool preroll_ready() const
    {
        return has_clock() && output_.queued_milliseconds() >= output_.target_latency_ms();
    }

private:
    void run()
    {
        while (true) {
            PacketPtr packet;
            {
                std::unique_lock lock(mutex_);
                queue_changed_.wait(lock, [this] {
                    return stop_requested_.load() || !packet_queue_.empty();
                });

                if (stop_requested_.load() && packet_queue_.empty()) {
                    break;
                }

                packet = std::move(packet_queue_.front());
                packet_queue_.pop_front();
                queue_space_.notify_one();
            }

            playback_pause_.wait(stop_requested_);
            decode_audio_packet(*codec_, stream_, *packet, output_, last_audio_end_seconds_);
            output_.wait_until_below(output_.target_latency_ms(), stop_requested_);
        }
    }

    static constexpr std::size_t max_packets_ = 256;

    CodecContextPtr codec_;
    const AVStream& stream_;
    AudioOutput output_;
    const PlaybackPause& playback_pause_;
    std::deque<PacketPtr> packet_queue_;
    std::mutex mutex_;
    std::condition_variable queue_changed_;
    std::condition_variable queue_space_;
    std::atomic_bool stop_requested_ = false;
    std::atomic<double> last_audio_end_seconds_ = 0.0;
    std::thread worker_;
};

class VideoDecoderWorker {
public:
    VideoDecoderWorker(
        AVFormatContext& format,
        CodecContextPtr codec,
        int video_stream_index,
        int audio_stream_index,
        AudioDecoderWorker* audio_worker,
        const PlaybackPause& playback_pause)
        : format_(format),
          codec_(std::move(codec)),
          video_stream_index_(video_stream_index),
          audio_stream_index_(audio_stream_index),
          audio_worker_(audio_worker),
          playback_pause_(playback_pause)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~VideoDecoderWorker()
    {
        stop_requested_.store(true);
        frame_available_.notify_all();
        frame_space_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::optional<DecodedVideoFrame> pop()
    {
        std::lock_guard lock(mutex_);
        if (error_) {
            std::rethrow_exception(error_);
        }
        if (frames_.empty()) {
            return std::nullopt;
        }

        DecodedVideoFrame frame = std::move(frames_.front());
        frames_.pop_front();
        frame_space_.notify_one();
        return frame;
    }

    bool finished()
    {
        std::lock_guard lock(mutex_);
        if (error_) {
            std::rethrow_exception(error_);
        }
        return eof_ && frames_.empty();
    }

private:
    void push_frame(DecodedVideoFrame frame)
    {
        {
            std::unique_lock lock(mutex_);
            frame_space_.wait(lock, [this] {
                return stop_requested_.load() || frames_.size() < max_frames_;
            });
            if (stop_requested_.load()) {
                return;
            }
            frames_.push_back(std::move(frame));
        }
        frame_available_.notify_one();
    }

    void decode_video_packet(const AVPacket& packet, PacketTiming packet_timing)
    {
        const auto send_start = std::chrono::steady_clock::now();
        throw_if_error(avcodec_send_packet(codec_.get(), &packet), "send video packet");
        const auto send_end = std::chrono::steady_clock::now();
        const double send_packet_ms = std::chrono::duration<double, std::milli>(send_end - send_start).count();

        FramePtr frame(av_frame_alloc());
        if (!frame) {
            throw std::runtime_error("failed to allocate video frame");
        }

        while (!stop_requested_.load()) {
            const auto decode_start = std::chrono::steady_clock::now();
            const int result = avcodec_receive_frame(codec_.get(), frame.get());
            const auto decode_end = std::chrono::steady_clock::now();
            const double decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_start).count();
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            throw_if_error(result, "receive video frame");

            AVFrame* cloned = av_frame_clone(frame.get());
            if (cloned == nullptr) {
                throw std::runtime_error("failed to clone video frame");
            }

            DecodedVideoFrame decoded;
            decoded.frame = FramePtr(cloned);
            decoded.media_time_s = frame_seconds(*frame, *format_.streams[video_stream_index_]);
            decoded.packet_timing = packet_timing;
            decoded.send_packet_ms = send_packet_ms;
            decoded.decode_ms = decode_ms;
            push_frame(std::move(decoded));
            av_frame_unref(frame.get());
        }
    }

    void run()
    {
        try {
            PacketPtr packet(av_packet_alloc());
            if (!packet) {
                throw std::runtime_error("failed to allocate packet");
            }

            while (!stop_requested_.load()) {
                playback_pause_.wait(stop_requested_);
                const auto demux_start = std::chrono::steady_clock::now();
                const int read_result = av_read_frame(&format_, packet.get());
                const auto demux_end = std::chrono::steady_clock::now();
                const PacketTiming packet_timing{
                    std::chrono::duration<double, std::milli>(demux_end - demux_start).count()};

                if (read_result == AVERROR_EOF) {
                    break;
                }
                throw_if_error(read_result, "read frame");

                if (packet->stream_index == video_stream_index_) {
                    decode_video_packet(*packet, packet_timing);
                } else if (audio_worker_ != nullptr && packet->stream_index == audio_stream_index_) {
                    audio_worker_->push(*packet);
                }

                if (audio_worker_ != nullptr && audio_worker_->has_clock()) {
                    const double packet_time = packet_seconds(*packet, *format_.streams[packet->stream_index]);
                    while (!stop_requested_.load() && packet_time > audio_worker_->audio_playback_seconds() + max_demux_ahead_seconds_) {
                        std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
                    }
                }

                av_packet_unref(packet.get());
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            error_ = std::current_exception();
        }

        {
            std::lock_guard lock(mutex_);
            eof_ = true;
        }
        frame_available_.notify_all();
    }

    static constexpr std::size_t max_frames_ = 6;
    static constexpr double max_demux_ahead_seconds_ = 3.0;

    AVFormatContext& format_;
    CodecContextPtr codec_;
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    AudioDecoderWorker* audio_worker_ = nullptr;
    const PlaybackPause& playback_pause_;
    mutable std::mutex mutex_;
    std::condition_variable frame_available_;
    std::condition_variable frame_space_;
    std::deque<DecodedVideoFrame> frames_;
    std::exception_ptr error_;
    std::atomic_bool stop_requested_ = false;
    bool eof_ = false;
    std::thread worker_;
};

class AudioDemuxWorker {
public:
    AudioDemuxWorker(
        AVFormatContext& format,
        int audio_stream_index,
        AudioDecoderWorker& audio_worker,
        const PlaybackPause& playback_pause)
        : format_(format),
          audio_stream_index_(audio_stream_index),
          audio_worker_(audio_worker),
          playback_pause_(playback_pause)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~AudioDemuxWorker()
    {
        stop_requested_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool finished()
    {
        std::lock_guard lock(mutex_);
        if (error_) {
            std::rethrow_exception(error_);
        }
        return eof_;
    }

private:
    void run()
    {
        try {
            PacketPtr packet(av_packet_alloc());
            if (!packet) {
                throw std::runtime_error("failed to allocate packet");
            }

            while (!stop_requested_.load()) {
                playback_pause_.wait(stop_requested_);
                const int read_result = av_read_frame(&format_, packet.get());
                if (read_result == AVERROR_EOF) {
                    break;
                }
                throw_if_error(read_result, "read frame");

                if (packet->stream_index == audio_stream_index_) {
                    audio_worker_.push(*packet);
                }

                av_packet_unref(packet.get());
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            error_ = std::current_exception();
        }

        {
            std::lock_guard lock(mutex_);
            eof_ = true;
        }
    }

    AVFormatContext& format_;
    int audio_stream_index_ = -1;
    AudioDecoderWorker& audio_worker_;
    const PlaybackPause& playback_pause_;
    mutable std::mutex mutex_;
    std::exception_ptr error_;
    std::atomic_bool stop_requested_ = false;
    bool eof_ = false;
    std::thread worker_;
};

} // namespace

struct FfmpegMediaDecoder::Impl {
    Impl(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
        : format(open_input(path)),
          video(open_decoder(*format, AVMEDIA_TYPE_VIDEO)),
          audio(open_decoder(*format, AVMEDIA_TYPE_AUDIO)),
          audio_target_ms(audio_target_ms),
          playback_pause(playback_pause)
    {
        if (!video.codec && !audio.codec) {
            throw std::runtime_error("input has no decodable audio or video streams");
        }

        info.has_video = video.codec != nullptr;
        info.has_audio = audio.codec != nullptr;
        if (info.has_video) {
            info.video = video_stream_info(*format->streams[video.stream_index], *video.codec);
            info.video_render = video_render_config(*video.codec);
        }
    }

    FormatPtr format;
    StreamDecoder video;
    StreamDecoder audio;
    MediaInfo info;
    double audio_target_ms = 150.0;
    const PlaybackPause& playback_pause;
    std::unique_ptr<AudioDecoderWorker> audio_worker;
    std::unique_ptr<VideoDecoderWorker> video_worker;
    std::unique_ptr<AudioDemuxWorker> audio_demux_worker;
    bool started = false;
};

FfmpegMediaDecoder::FfmpegMediaDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
    : impl_(std::make_unique<Impl>(path, audio_target_ms, playback_pause))
{
}

FfmpegMediaDecoder::~FfmpegMediaDecoder()
{
    stop();
}

MediaInfo FfmpegMediaDecoder::media_info() const
{
    return impl_->info;
}

void FfmpegMediaDecoder::start()
{
    if (impl_->started) {
        return;
    }

    if (impl_->audio.codec) {
        impl_->audio_worker = std::make_unique<AudioDecoderWorker>(
            std::move(impl_->audio.codec),
            *impl_->format->streams[impl_->audio.stream_index],
            impl_->audio_target_ms,
            impl_->playback_pause);
    }

    if (impl_->video.codec) {
        impl_->video_worker = std::make_unique<VideoDecoderWorker>(
            *impl_->format,
            std::move(impl_->video.codec),
            impl_->video.stream_index,
            impl_->audio.stream_index,
            impl_->audio_worker.get(),
            impl_->playback_pause);
    } else if (impl_->audio_worker) {
        impl_->audio_demux_worker = std::make_unique<AudioDemuxWorker>(
            *impl_->format,
            impl_->audio.stream_index,
            *impl_->audio_worker,
            impl_->playback_pause);
    }

    impl_->started = true;
}

void FfmpegMediaDecoder::stop()
{
    impl_->audio_demux_worker.reset();
    impl_->video_worker.reset();
    impl_->audio_worker.reset();
    impl_->started = false;
}

void FfmpegMediaDecoder::seek(double seconds)
{
    static_cast<void>(seconds);
    throw std::runtime_error("seeking is not implemented yet");
}

std::optional<DecodedVideoFrame> FfmpegMediaDecoder::pop_video_frame()
{
    if (!impl_->video_worker) {
        return std::nullopt;
    }
    return impl_->video_worker->pop();
}

std::optional<DecodedAudioFrame> FfmpegMediaDecoder::pop_audio_frame()
{
    return std::nullopt;
}

bool FfmpegMediaDecoder::finished()
{
    if (impl_->video_worker) {
        return impl_->video_worker->finished();
    }
    if (impl_->audio_demux_worker) {
        return impl_->audio_demux_worker->finished();
    }
    return true;
}

bool FfmpegMediaDecoder::has_audio() const
{
    return impl_->info.has_audio;
}

AudioOutput* FfmpegMediaDecoder::audio_output()
{
    return impl_->audio_worker ? &impl_->audio_worker->output() : nullptr;
}

bool FfmpegMediaDecoder::has_audio_clock() const
{
    return impl_->audio_worker != nullptr && impl_->audio_worker->has_clock();
}

bool FfmpegMediaDecoder::audio_preroll_ready() const
{
    return impl_->audio_worker != nullptr && impl_->audio_worker->preroll_ready();
}

double FfmpegMediaDecoder::audio_playback_seconds() const
{
    return impl_->audio_worker != nullptr ? impl_->audio_worker->audio_playback_seconds() : 0.0;
}

} // namespace webvideoplayback::player
