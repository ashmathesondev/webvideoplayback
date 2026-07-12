// Native media player used by the playback test harness.
//
// The player opens local files or HTTP URLs through FFmpeg, decodes audio and
// video on worker threads, and keeps SDL rendering on the main thread. Audio is
// the master playback clock. Window moves and resizes explicitly pause decode
// and audio output, then resume once interaction settles.
#include "player/av_support.hpp"
#include "player/sdl_media.hpp"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace webvideoplayback::player;

struct PerformanceSample {
    std::uint64_t frame_number = 0;
    double media_time_s = 0.0;
    VideoFrameTiming timing;
    MemoryStats memory;
    Uint32 audio_queued_bytes = 0;
    double audio_queued_ms = 0.0;
    double audio_target_ms = 0.0;
    double av_sync_ms = 0.0;
    std::uint64_t audio_underruns = 0;
    int window_width = 0;
    int window_height = 0;
    int display_width = 0;
    int display_height = 0;
};

class PerformanceReport {
public:
    PerformanceReport(bool enabled, const std::string& media_path, const VideoStreamInfo& video_info, double audio_target_ms)
    {
        if (!enabled) {
            return;
        }

        const std::filesystem::path report_directory = WEBVIDEOPLAYBACK_REPORT_DIR;
        std::filesystem::create_directories(report_directory);
        const auto report_path = report_directory / ("webvideoplayback-performance-" + timestamp() + ".csv");
        file_.open(report_path, std::ios::out | std::ios::trunc);
        if (!file_) {
            return;
        }

        file_ << "media_path," << csv_escape(media_path) << "\n";
        file_ << "video_codec," << csv_escape(video_info.codec_name) << "\n";
        file_ << "pixel_format," << csv_escape(video_info.pixel_format) << "\n";
        file_ << "fps," << video_info.fps << "\n";
        file_ << "bit_rate," << video_info.bit_rate << "\n";
        file_ << "audio_target_ms," << audio_target_ms << "\n";
        file_ << "frame,media_time_s,total_ms,demux_ms,send_packet_ms,decode_ms,convert_ms,upload_ms,present_ms,"
              << "clear_ms,copy_ms,overlay_ms,swap_ms,"
              << "working_set_mb,private_mb,peak_working_set_mb,audio_queued_bytes,"
              << "audio_queued_ms,audio_target_ms,av_sync_ms,audio_underruns,"
              << "window_width,window_height,display_width,display_height\n";
    }

    void write(const PerformanceSample& sample)
    {
        if (!file_) {
            return;
        }

        file_ << sample.frame_number << ','
              << sample.media_time_s << ','
              << sample.timing.total_ms << ','
              << sample.timing.demux_ms << ','
              << sample.timing.send_packet_ms << ','
              << sample.timing.decode_ms << ','
              << sample.timing.convert_ms << ','
              << sample.timing.upload_ms << ','
              << sample.timing.present_ms << ','
              << sample.timing.clear_ms << ','
              << sample.timing.copy_ms << ','
              << sample.timing.overlay_ms << ','
              << sample.timing.swap_ms << ','
              << sample.memory.working_set_mb << ','
              << sample.memory.private_usage_mb << ','
              << sample.memory.peak_working_set_mb << ','
              << sample.audio_queued_bytes << ','
              << sample.audio_queued_ms << ','
              << sample.audio_target_ms << ','
              << sample.av_sync_ms << ','
              << sample.audio_underruns << ','
              << sample.window_width << ','
              << sample.window_height << ','
              << sample.display_width << ','
              << sample.display_height << '\n';
    }

private:
    static std::string timestamp()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local_time = {};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif

        std::ostringstream stream;
        stream << std::put_time(&local_time, "%Y%m%d-%H%M%S");
        return stream.str();
    }

    static std::string csv_escape(const std::string& value)
    {
        std::string escaped = "\"";
        for (char character : value) {
            if (character == '"') {
                escaped += "\"\"";
            } else {
                escaped += character;
            }
        }
        escaped += '"';
        return escaped;
    }

    std::ofstream file_;
};

struct PlayerOptions {
    std::optional<std::string> media_path;
    bool performance_report = false;
};

PlayerOptions parse_player_options(int argc, char** argv)
{
    PlayerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--performance-report" || arg == "--perf-report") {
            options.performance_report = true;
        } else if (!options.media_path) {
            options.media_path = arg;
        } else {
            throw std::runtime_error("usage: webvideoplayback.exe [--performance-report] [media-path-or-url]");
        }
    }
    return options;
}

// Consolidated SDL event result. The render loop owns policy decisions.
struct EventState {
    bool window_interaction = false;
    bool interaction_started = false;
    bool interaction_finished = false;
    bool overlay_toggle = false;
};

// Shared gate used to stop demux and decode during window interactions.
class PlaybackPause {
public:
    void set_paused(bool paused)
    {
        paused_.store(paused);
    }

    bool paused() const
    {
        return paused_.load();
    }

    void wait(const std::atomic_bool& stop_requested) const
    {
        while (!stop_requested.load() && paused_.load()) {
            std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
        }
    }

private:
    std::atomic_bool paused_ = false;
};

EventState pump_events(bool& running)
{
    EventState state;
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            running = false;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
            running = false;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F1) {
            state.overlay_toggle = true;
        }
        if (event.type == SDL_WINDOWEVENT) {
            switch (event.window.event) {
            case SDL_WINDOWEVENT_MOVED:
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_MAXIMIZED:
            case SDL_WINDOWEVENT_RESTORED:
                state.window_interaction = true;
                break;
            default:
                break;
            }
        }
        if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            state.interaction_finished = true;
        }
    }

    return state;
}



// Owns all SDL video resources. Only the main thread calls this class.
// Converts decoded audio frames, queues them to SDL, and updates the clock.
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

// Audio decode worker. Packet input is bounded to limit memory growth.
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

    const AudioOutput& output() const
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

// Demuxes the shared media stream and decodes video frames off the UI thread.
// Audio packets are forwarded to AudioDecoderWorker from the same demux loop.
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

class FfmpegMediaDecoder final : public IMediaDecoder {
public:
    FfmpegMediaDecoder(const std::string& path, double audio_target_ms, const PlaybackPause& playback_pause)
        : format_(open_input(path)),
          video_(open_decoder(*format_, AVMEDIA_TYPE_VIDEO)),
          audio_(open_decoder(*format_, AVMEDIA_TYPE_AUDIO)),
          audio_target_ms_(audio_target_ms),
          playback_pause_(playback_pause)
    {
        if (!video_.codec && !audio_.codec) {
            throw std::runtime_error("input has no decodable audio or video streams");
        }

        info_.has_video = video_.codec != nullptr;
        info_.has_audio = audio_.codec != nullptr;
        if (info_.has_video) {
            info_.video = video_stream_info(*format_->streams[video_.stream_index], *video_.codec);
        }
    }

    ~FfmpegMediaDecoder() override
    {
        stop();
    }

    MediaInfo media_info() const override
    {
        return info_;
    }

    const AVCodecContext& video_codec() const
    {
        if (!video_.codec) {
            throw std::runtime_error("input has no video stream");
        }
        return *video_.codec;
    }

    void start() override
    {
        if (started_) {
            return;
        }

        if (audio_.codec) {
            audio_worker_ = std::make_unique<AudioDecoderWorker>(
                std::move(audio_.codec),
                *format_->streams[audio_.stream_index],
                audio_target_ms_,
                playback_pause_);
        }

        if (video_.codec) {
            video_worker_ = std::make_unique<VideoDecoderWorker>(
                *format_,
                std::move(video_.codec),
                video_.stream_index,
                audio_.stream_index,
                audio_worker_.get(),
                playback_pause_);
        } else if (audio_worker_) {
            audio_demux_worker_ = std::make_unique<AudioDemuxWorker>(
                *format_,
                audio_.stream_index,
                *audio_worker_,
                playback_pause_);
        }

        started_ = true;
    }

    void stop() override
    {
        audio_demux_worker_.reset();
        video_worker_.reset();
        audio_worker_.reset();
        started_ = false;
    }

    void seek(double seconds) override
    {
        static_cast<void>(seconds);
        throw std::runtime_error("seeking is not implemented yet");
    }

    std::optional<DecodedVideoFrame> pop_video_frame() override
    {
        if (!video_worker_) {
            return std::nullopt;
        }
        return video_worker_->pop();
    }

    std::optional<DecodedAudioFrame> pop_audio_frame() override
    {
        return std::nullopt;
    }

    bool finished()
    {
        if (video_worker_) {
            return video_worker_->finished();
        }
        if (audio_demux_worker_) {
            return audio_demux_worker_->finished();
        }
        return true;
    }

    bool has_audio() const
    {
        return info_.has_audio;
    }

    AudioOutput* audio_output()
    {
        return audio_worker_ ? &audio_worker_->output() : nullptr;
    }

    bool has_audio_clock() const
    {
        return audio_worker_ != nullptr && audio_worker_->has_clock();
    }

    bool audio_preroll_ready() const
    {
        return audio_worker_ != nullptr && audio_worker_->preroll_ready();
    }

    double audio_playback_seconds() const
    {
        return audio_worker_ != nullptr ? audio_worker_->audio_playback_seconds() : 0.0;
    }

private:
    FormatPtr format_;
    StreamDecoder video_;
    StreamDecoder audio_;
    MediaInfo info_;
    double audio_target_ms_ = 150.0;
    const PlaybackPause& playback_pause_;
    std::unique_ptr<AudioDecoderWorker> audio_worker_;
    std::unique_ptr<VideoDecoderWorker> video_worker_;
    std::unique_ptr<AudioDemuxWorker> audio_demux_worker_;
    bool started_ = false;
};

int run(const std::string& path, bool performance_report_enabled)
{
    PlaybackPause playback_pause;
    const double audio_target_ms = configured_audio_target_ms();
    FfmpegMediaDecoder decoder(path, audio_target_ms, playback_pause);
    const MediaInfo media_info = decoder.media_info();

    std::unique_ptr<IRenderSink> render_sink;
    if (media_info.has_video) {
        render_sink = std::make_unique<SdlTextureRenderSink>(decoder.video_codec());
    }
    decoder.start();

    bool running = true;
    bool show_overlay = false;
    bool playback_started = !decoder.has_audio();
    std::uint64_t frame_number = 0;
    PerformanceReport report(performance_report_enabled, path, media_info.video, audio_target_ms);
    auto playback_start = std::chrono::steady_clock::now();
    auto last_loop_time = playback_start;
    bool interaction_paused = false;
    auto last_interaction_time = playback_start;

    if (media_info.has_video) {
        while (running && !decoder.finished()) {
            const EventState events = pump_events(running);
            if (events.overlay_toggle) {
                show_overlay = !show_overlay;
            }
            if (events.interaction_started || events.window_interaction) {
                // Pause the whole pipeline while Windows is actively moving or
                // resizing the SDL window. This avoids clock drift and queued
                // frame bursts when the event loop is blocked by interaction.
                playback_pause.set_paused(true);
                interaction_paused = true;
                last_interaction_time = std::chrono::steady_clock::now();
                if (AudioOutput* output = decoder.audio_output()) {
                    output->pause();
                }
            }
            if (events.interaction_finished) {
                playback_pause.set_paused(false);
                if (AudioOutput* output = decoder.audio_output()) {
                    output->resume();
                }
                playback_start = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(decoder.has_audio_clock() ? decoder.audio_playback_seconds() : 0.0));
                last_loop_time = std::chrono::steady_clock::now();
            }
            if (interaction_paused
                && std::chrono::steady_clock::now() - last_interaction_time > std::chrono::milliseconds(180)) {
                // Resize events do not always have a matching finish event, so
                // resume after a short quiet period.
                playback_pause.set_paused(false);
                interaction_paused = false;
                if (AudioOutput* output = decoder.audio_output()) {
                    output->resume();
                }
                playback_start = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(decoder.has_audio_clock() ? decoder.audio_playback_seconds() : 0.0));
                last_loop_time = std::chrono::steady_clock::now();
            }
            const auto now = std::chrono::steady_clock::now();
            const auto loop_gap = now - last_loop_time;
            if (events.window_interaction || loop_gap > std::chrono::milliseconds(250)) {
                playback_start += loop_gap;
            }
            last_loop_time = now;

            if (!running) {
                break;
            }

            if (playback_pause.paused()) {
                std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
                continue;
            }

            if (!playback_started && decoder.audio_preroll_ready()) {
                // Start video only after audio has a usable clock and buffer.
                playback_started = true;
                playback_start = std::chrono::steady_clock::now();
                last_loop_time = playback_start;
            }

            if (!playback_started) {
                std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
                continue;
            }

            std::optional<DecodedVideoFrame> decoded = decoder.pop_video_frame();
            if (!decoded) {
                std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
                continue;
            }

            const double target_seconds = decoded->media_time_s;
            AudioOutput* audio_output = decoder.audio_output();
            if (decoder.has_audio_clock() && audio_output != nullptr && audio_output->queued_size() > 0) {
                // Audio is the master clock. Video waits for audio and drops
                // stale frames only after a large delay.
                const double audio_seconds = decoder.audio_playback_seconds();
                if (target_seconds + 0.250 < audio_seconds) {
                    continue;
                }

                while (running
                    && audio_output->queued_size() > 0
                    && target_seconds > decoder.audio_playback_seconds() + 0.005) {
                    const auto sleep_target = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
                    const EventState wait_events = pump_events(running);
                    if (wait_events.overlay_toggle) {
                        show_overlay = !show_overlay;
                    }
                    if (wait_events.interaction_started || wait_events.window_interaction) {
                        playback_pause.set_paused(true);
                        if (AudioOutput* output = decoder.audio_output()) {
                            output->pause();
                        }
                        break;
                    }
                    if (wait_events.interaction_finished) {
                        playback_pause.set_paused(false);
                        if (AudioOutput* output = decoder.audio_output()) {
                            output->resume();
                        }
                    }
                    std::this_thread::sleep_until(sleep_target);
                }
            } else {
                const auto target_time = playback_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(target_seconds));
                while (running && std::chrono::steady_clock::now() < target_time) {
                    const auto sleep_target = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
                    const EventState wait_events = pump_events(running);
                    if (wait_events.overlay_toggle) {
                        show_overlay = !show_overlay;
                    }
                    if (wait_events.interaction_started || wait_events.window_interaction) {
                        playback_pause.set_paused(true);
                        if (AudioOutput* output = decoder.audio_output()) {
                            output->pause();
                        }
                        break;
                    }
                    if (wait_events.interaction_finished) {
                        playback_pause.set_paused(false);
                        if (AudioOutput* output = decoder.audio_output()) {
                            output->resume();
                        }
                    }
                    std::this_thread::sleep_until(sleep_target);
                }
            }

            if (!running) {
                break;
            }

            if (decoder.has_audio_clock() && audio_output != nullptr && audio_output->queued_size() > 0) {
                const double audio_seconds = decoder.audio_playback_seconds();
                if (target_seconds + 0.250 < audio_seconds) {
                    continue;
                }
            }

            const VideoFrameTiming timing = render_sink->present(RenderFrame{
                *decoded->frame,
                audio_output,
                decoded->packet_timing.demux_ms,
                decoded->send_packet_ms,
                decoded->decode_ms,
                show_overlay});
            const SDL_Rect video_rect = render_sink->current_video_rect();

            PerformanceSample sample;
            sample.frame_number = ++frame_number;
            sample.media_time_s = target_seconds;
            sample.timing = timing;
            sample.memory = current_memory_stats();
            sample.audio_queued_bytes = audio_output != nullptr ? audio_output->queued_size() : 0;
            sample.audio_queued_ms = audio_output != nullptr ? audio_output->queued_milliseconds() : 0.0;
            sample.audio_target_ms = audio_output != nullptr ? audio_output->target_latency_ms() : 0.0;
            sample.av_sync_ms =
                decoder.has_audio_clock() ? (target_seconds - decoder.audio_playback_seconds()) * 1000.0 : 0.0;
            sample.audio_underruns = audio_output != nullptr ? audio_output->underruns() : 0;
            sample.window_width = render_sink->window_width();
            sample.window_height = render_sink->window_height();
            sample.display_width = video_rect.w;
            sample.display_height = video_rect.h;
            report.write(sample);
        }
    } else {
        while (running && !decoder.finished()) {
            const EventState events = pump_events(running);
            if (events.overlay_toggle) {
                show_overlay = !show_overlay;
            }
            if (!running) {
                break;
            }
            std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
        }
    }

    while (AudioOutput* output = decoder.audio_output()) {
        if (output->queued_size() == 0) {
            break;
        }
        const auto next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        const EventState events = pump_events(running);
        if (events.overlay_toggle) {
            show_overlay = !show_overlay;
        }
        if (!running) {
            break;
        }
        std::this_thread::sleep_until(next_poll);
    }

    return 0;
}

} // namespace

int run_player(int argc, char** argv)
{
    try {
        const SdlGuard sdl;
        const AvNetworkGuard network;
        PlayerOptions options = parse_player_options(argc, argv);
        if (!options.media_path) {
            options.media_path = open_media_file_dialog();
        }
        if (!options.media_path) {
            return 0;
        }

        return run(*options.media_path, options.performance_report);
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Playback error", error.what(), nullptr);
        return 1;
    }
}
