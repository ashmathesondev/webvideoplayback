#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <array>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& value)
{

    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("failed to convert path to UTF-8");
    }

    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<std::string> open_media_file_dialog()
{
    std::wstring path(32768, L'\0');
    const wchar_t filter[] =
        L"Media files\0*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.mpg;*.mpeg;*.mp3;*.m4a;*.aac;*.ogg;*.opus;*.flac;*.wav\0"
        L"All files\0*.*\0";

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"Open media file";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }

    return wide_to_utf8(path.c_str());
}
#else
std::optional<std::string> open_media_file_dialog()
{
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "Open media file",
        "Native file picker is not implemented for this platform yet.",
        nullptr);
    return std::nullopt;
}
#endif

std::string av_error(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void throw_if_error(int code, const std::string& context)
{
    if (code < 0) {
        throw std::runtime_error(context + ": " + av_error(code));
    }
}

struct SdlGuard {
    SdlGuard()
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
    }

    ~SdlGuard()
    {
        SDL_Quit();
    }
};

struct AvNetworkGuard {
    AvNetworkGuard()
    {
        throw_if_error(avformat_network_init(), "initialize FFmpeg network");
    }

    ~AvNetworkGuard()
    {
        avformat_network_deinit();
    }
};

struct FormatDeleter {
    void operator()(AVFormatContext* context) const
    {
        avformat_close_input(&context);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

struct SwsDeleter {
    void operator()(SwsContext* context) const
    {
        sws_freeContext(context);
    }
};

struct SwrDeleter {
    void operator()(SwrContext* context) const
    {
        swr_free(&context);
    }
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const
    {
        SDL_DestroyWindow(window);
    }
};

struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const
    {
        SDL_DestroyRenderer(renderer);
    }
};

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const
    {
        SDL_DestroyTexture(texture);
    }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

struct StreamDecoder {
    int stream_index = -1;
    CodecContextPtr codec;
};

struct DebugStats {
    double render_frame_ms = 0.0;
    double demux_ms = 0.0;
    double send_packet_ms = 0.0;
    double decode_ms = 0.0;
    double convert_ms = 0.0;
    double upload_ms = 0.0;
    double present_ms = 0.0;
    double clear_ms = 0.0;
    double copy_ms = 0.0;
    double overlay_ms = 0.0;
    double swap_ms = 0.0;
    Uint32 audio_queued_bytes = 0;
    double audio_queued_ms = 0.0;
    double audio_target_ms = 0.0;
    double av_sync_ms = 0.0;
    std::uint64_t audio_underruns = 0;
    int audio_sample_rate = 0;
    int audio_channels = 0;
    int video_width = 0;
    int video_height = 0;
    int window_width = 0;
    int window_height = 0;
    int display_width = 0;
    int display_height = 0;
    double scale = 0.0;
    double working_set_mb = 0.0;
    double private_usage_mb = 0.0;
    double peak_working_set_mb = 0.0;
};

struct VideoFrameTiming {
    double demux_ms = 0.0;
    double send_packet_ms = 0.0;
    double decode_ms = 0.0;
    double convert_ms = 0.0;
    double upload_ms = 0.0;
    double present_ms = 0.0;
    double clear_ms = 0.0;
    double copy_ms = 0.0;
    double overlay_ms = 0.0;
    double swap_ms = 0.0;
    double total_ms = 0.0;
};

struct VideoStreamInfo {
    std::string codec_name;
    std::string pixel_format;
    double fps = 0.0;
    int bit_rate = 0;
};

struct MemoryStats {
    double working_set_mb = 0.0;
    double private_usage_mb = 0.0;
    double peak_working_set_mb = 0.0;
};

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

struct PacketTiming {
    double demux_ms = 0.0;
};

class PerformanceReport {
public:
    PerformanceReport(const std::string& media_path, const VideoStreamInfo& video_info, double audio_target_ms)
    {
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

struct EventState {
    bool window_interaction = false;
    bool overlay_toggle = false;
};

FormatPtr open_input(const std::string& path)
{
    AVFormatContext* raw = nullptr;
    throw_if_error(avformat_open_input(&raw, path.c_str(), nullptr, nullptr), "open input");
    FormatPtr format(raw);
    throw_if_error(avformat_find_stream_info(format.get(), nullptr), "read stream info");
    return format;
}

double stream_fps(const AVStream& stream)
{
    const AVRational rate = stream.avg_frame_rate.num != 0 ? stream.avg_frame_rate : stream.r_frame_rate;
    if (rate.num == 0 || rate.den == 0) {
        return 0.0;
    }

    return av_q2d(rate);
}

VideoStreamInfo video_stream_info(const AVStream& stream, const AVCodecContext& codec)
{
    VideoStreamInfo info;
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec.codec_id);
    info.codec_name = descriptor != nullptr ? descriptor->name : "unknown";
    const char* pixel_format = av_get_pix_fmt_name(codec.pix_fmt);
    info.pixel_format = pixel_format != nullptr ? pixel_format : "unknown";
    info.fps = stream_fps(stream);
    info.bit_rate = static_cast<int>(codec.bit_rate);
    return info;
}

StreamDecoder open_decoder(AVFormatContext& format, AVMediaType type)
{
    const int stream_index = av_find_best_stream(&format, type, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        return {};
    }

    AVStream* stream = format.streams[stream_index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        throw std::runtime_error("no decoder found for stream " + std::to_string(stream_index));
    }

    AVCodecContext* raw_context = avcodec_alloc_context3(decoder);
    if (raw_context == nullptr) {
        throw std::runtime_error("failed to allocate decoder context");
    }

    CodecContextPtr context(raw_context);
    throw_if_error(avcodec_parameters_to_context(context.get(), stream->codecpar), "copy codec parameters");
    context->thread_count = 4;
    context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    throw_if_error(avcodec_open2(context.get(), decoder, nullptr), "open decoder");

    return {stream_index, std::move(context)};
}

double frame_seconds(const AVFrame& frame, const AVStream& stream)
{
    const int64_t timestamp = frame.best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        return 0.0;
    }

    return static_cast<double>(timestamp) * av_q2d(stream.time_base);
}

double frame_end_seconds(const AVFrame& frame, const AVStream& stream)
{
    double seconds = frame_seconds(frame, stream);
    if (frame.sample_rate > 0 && frame.nb_samples > 0) {
        seconds += static_cast<double>(frame.nb_samples) / static_cast<double>(frame.sample_rate);
    }

    return seconds;
}

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
    }

    return state;
}

MemoryStats current_memory_stats()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))
        == 0) {
        return {};
    }

    constexpr double bytes_per_mb = 1024.0 * 1024.0;
    MemoryStats stats;
    stats.working_set_mb = static_cast<double>(counters.WorkingSetSize) / bytes_per_mb;
    stats.private_usage_mb = static_cast<double>(counters.PrivateUsage) / bytes_per_mb;
    stats.peak_working_set_mb = static_cast<double>(counters.PeakWorkingSetSize) / bytes_per_mb;
    return stats;
#else
    return {};
#endif
}

double configured_audio_target_ms()
{
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, "WEBVIDEOPLAYBACK_AUDIO_LATENCY_MS") != 0 || raw_value == nullptr) {
        return 150.0;
    }

    const std::unique_ptr<char, decltype(&std::free)> value_guard(raw_value, &std::free);
#else
    const char* raw_value = std::getenv("WEBVIDEOPLAYBACK_AUDIO_LATENCY_MS");
    if (raw_value == nullptr) {
        return 150.0;
    }
#endif

    try {
        const double value = std::stod(raw_value);
        if (value >= 20.0 && value <= 2000.0) {
            return value;
        }
    } catch (...) {
    }

    return 150.0;
}

class AudioOutput {
public:
    AudioOutput(const AVCodecContext& codec, double target_latency_ms)
        : target_latency_ms_(target_latency_ms)
    {
        SDL_AudioSpec desired = {};
        desired.freq = output_sample_rate_;
        desired.format = AUDIO_F32SYS;
        desired.channels = output_channels_;
        desired.samples = 4096;

        device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained_, 0);
        if (device_ == 0) {
            throw std::runtime_error(std::string("SDL_OpenAudioDevice failed: ") + SDL_GetError());
        }

        AVChannelLayout output_layout;
        av_channel_layout_default(&output_layout, output_channels_);

        SwrContext* raw = nullptr;
        throw_if_error(
            swr_alloc_set_opts2(
                &raw,
                &output_layout,
                AV_SAMPLE_FMT_FLT,
                output_sample_rate_,
                &codec.ch_layout,
                codec.sample_fmt,
                codec.sample_rate,
                0,
                nullptr),
            "allocate audio resampler");

        resampler_.reset(raw);
        throw_if_error(swr_init(resampler_.get()), "initialize audio resampler");
        av_channel_layout_uninit(&output_layout);

        SDL_PauseAudioDevice(device_, 0);
    }

    ~AudioOutput()
    {
        if (device_ != 0) {
            SDL_CloseAudioDevice(device_);
        }
    }

    void queue(const AVFrame& frame)
    {
        const int output_samples = swr_get_out_samples(resampler_.get(), frame.nb_samples);
        const int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLT);
        const int buffer_size = output_samples * output_channels_ * bytes_per_sample;
        buffer_.resize(static_cast<std::size_t>(buffer_size));

        auto* out = buffer_.data();
        const int converted = swr_convert(
            resampler_.get(),
            &out,
            output_samples,
            frame.extended_data,
            frame.nb_samples);

        throw_if_error(converted, "convert audio frame");

        const int bytes = converted * output_channels_ * bytes_per_sample;
        if (SDL_QueueAudio(device_, buffer_.data(), static_cast<Uint32>(bytes)) != 0) {
            throw std::runtime_error(std::string("SDL_QueueAudio failed: ") + SDL_GetError());
        }
    }

    Uint32 queued_size() const
    {
        const Uint32 size = SDL_GetQueuedAudioSize(device_);
        if (size == 0) {
            bool expected = false;
            if (was_empty_.compare_exchange_strong(expected, true)) {
                underruns_.fetch_add(1);
            }
        } else {
            was_empty_.store(false);
        }

        return size;
    }

    double queued_milliseconds() const
    {
        const int bytes_per_frame = obtained_.channels * static_cast<int>(sizeof(float));
        if (bytes_per_frame == 0 || obtained_.freq == 0) {
            return 0.0;
        }

        const double queued_frames = static_cast<double>(queued_size()) / static_cast<double>(bytes_per_frame);
        return queued_frames * 1000.0 / static_cast<double>(obtained_.freq);
    }

    int sample_rate() const
    {
        return obtained_.freq;
    }

    int channels() const
    {
        return obtained_.channels;
    }

    void pause()
    {
        SDL_PauseAudioDevice(device_, 1);
    }

    void resume()
    {
        SDL_PauseAudioDevice(device_, 0);
    }

    void wait_until_below(double queued_ms, const std::atomic_bool& stop_requested)
    {
        while (!stop_requested.load() && queued_milliseconds() > queued_ms) {
            std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
        }
    }

    double target_latency_ms() const
    {
        return target_latency_ms_;
    }

    std::uint64_t underruns() const
    {
        return underruns_.load();
    }

private:
    static constexpr int output_sample_rate_ = 48000;
    static constexpr int output_channels_ = 2;

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec obtained_ = {};
    SwrPtr resampler_;
    std::vector<std::uint8_t> buffer_;
    double target_latency_ms_ = 150.0;
    mutable std::atomic_bool was_empty_ = false;
    mutable std::atomic_uint64_t underruns_ = 0;
};

class VideoOutput {
public:
    explicit VideoOutput(const AVCodecContext& codec)
    {
        window_.reset(SDL_CreateWindow(
            "Web Video Playback",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            codec.width,
            codec.height,
            SDL_WINDOW_RESIZABLE));
        if (!window_) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED));
        if (!renderer_) {
            throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
        }

        texture_.reset(SDL_CreateTexture(
            renderer_.get(),
            codec.pix_fmt == AV_PIX_FMT_YUV420P ? SDL_PIXELFORMAT_IYUV : SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            codec.width,
            codec.height));
        if (!texture_) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") + SDL_GetError());
        }

        direct_yuv_ = codec.pix_fmt == AV_PIX_FMT_YUV420P;
        if (!direct_yuv_) {
            scaler_.reset(sws_getContext(
                codec.width,
                codec.height,
                codec.pix_fmt,
                codec.width,
                codec.height,
                AV_PIX_FMT_RGBA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr));
            if (!scaler_) {
                throw std::runtime_error("failed to create video scaler");
            }
        }

        width_ = codec.width;
        height_ = codec.height;
        if (!direct_yuv_) {
            pixels_.resize(static_cast<std::size_t>(width_ * height_ * 4));
        }
    }

    VideoFrameTiming render(
        const AVFrame& frame,
        const AudioOutput* audio,
        double demux_ms,
        double send_packet_ms,
        double decode_ms,
        bool show_overlay)
    {
        const auto total_start = std::chrono::steady_clock::now();

        const auto convert_start = std::chrono::steady_clock::now();
        if (!direct_yuv_) {
            std::uint8_t* scaled_pixels[] = {pixels_.data()};
            const int linesize[] = {width_ * 4};
            sws_scale(
                scaler_.get(),
                frame.data,
                frame.linesize,
                0,
                height_,
                scaled_pixels,
                linesize);
        }
        const auto convert_end = std::chrono::steady_clock::now();

        const auto upload_start = std::chrono::steady_clock::now();
        if (direct_yuv_) {
            if (SDL_UpdateYUVTexture(
                    texture_.get(),
                    nullptr,
                    frame.data[0],
                    frame.linesize[0],
                    frame.data[1],
                    frame.linesize[1],
                    frame.data[2],
                    frame.linesize[2])
                != 0) {
                throw std::runtime_error(std::string("SDL_UpdateYUVTexture failed: ") + SDL_GetError());
            }
        } else {
            if (SDL_UpdateTexture(texture_.get(), nullptr, pixels_.data(), width_ * 4) != 0) {
                throw std::runtime_error(std::string("SDL_UpdateTexture failed: ") + SDL_GetError());
            }
        }
        const auto upload_end = std::chrono::steady_clock::now();

        int window_width = 0;
        int window_height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &window_width, &window_height);
        const SDL_Rect video_rect = video_destination_rect(window_width, window_height);

        SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
        const auto present_start = std::chrono::steady_clock::now();
        const auto clear_start = std::chrono::steady_clock::now();
        SDL_RenderClear(renderer_.get());
        const auto clear_end = std::chrono::steady_clock::now();
        const auto copy_start = std::chrono::steady_clock::now();
        SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, &video_rect);
        const auto copy_end = std::chrono::steady_clock::now();
        const auto overlay_start = std::chrono::steady_clock::now();
        if (show_overlay) {
            draw_debug_overlay(make_debug_stats(audio, window_width, window_height, video_rect));
        }
        const auto overlay_end = std::chrono::steady_clock::now();
        const auto swap_start = std::chrono::steady_clock::now();
        SDL_RenderPresent(renderer_.get());
        const auto swap_end = std::chrono::steady_clock::now();
        const auto present_end = std::chrono::steady_clock::now();

        const auto total_end = std::chrono::steady_clock::now();
        last_timing_.demux_ms = demux_ms;
        last_timing_.send_packet_ms = send_packet_ms;
        last_timing_.decode_ms = decode_ms;
        last_timing_.convert_ms = std::chrono::duration<double, std::milli>(convert_end - convert_start).count();
        last_timing_.upload_ms = std::chrono::duration<double, std::milli>(upload_end - upload_start).count();
        last_timing_.present_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();
        last_timing_.clear_ms = std::chrono::duration<double, std::milli>(clear_end - clear_start).count();
        last_timing_.copy_ms = std::chrono::duration<double, std::milli>(copy_end - copy_start).count();
        last_timing_.overlay_ms = std::chrono::duration<double, std::milli>(overlay_end - overlay_start).count();
        last_timing_.swap_ms = std::chrono::duration<double, std::milli>(swap_end - swap_start).count();
        last_timing_.total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
        return last_timing_;
    }

    int window_width() const
    {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &width, &height);
        return width;
    }

    int window_height() const
    {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &width, &height);
        return height;
    }

    SDL_Rect current_video_rect() const
    {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &width, &height);
        return video_destination_rect(width, height);
    }

private:
    SDL_Rect video_destination_rect(int window_width, int window_height) const
    {
        if (width_ <= 0 || height_ <= 0 || window_width <= 0 || window_height <= 0) {
            return {0, 0, window_width, window_height};
        }

        const double width_scale = static_cast<double>(window_width) / static_cast<double>(width_);
        const double height_scale = static_cast<double>(window_height) / static_cast<double>(height_);
        const double scale = width_scale < height_scale ? width_scale : height_scale;
        int display_width = static_cast<int>(static_cast<double>(width_) * scale + 0.5);
        int display_height = static_cast<int>(static_cast<double>(height_) * scale + 0.5);
        display_width = display_width > 0 ? display_width : 1;
        display_height = display_height > 0 ? display_height : 1;
        const int display_x = (window_width - display_width) / 2;
        const int display_y = (window_height - display_height) / 2;

        return {display_x, display_y, display_width, display_height};
    }

    DebugStats make_debug_stats(
        const AudioOutput* audio,
        int window_width,
        int window_height,
        SDL_Rect destination) const
    {
        DebugStats stats;
        stats.render_frame_ms = last_timing_.total_ms;
        stats.demux_ms = last_timing_.demux_ms;
        stats.send_packet_ms = last_timing_.send_packet_ms;
        stats.decode_ms = last_timing_.decode_ms;
        stats.convert_ms = last_timing_.convert_ms;
        stats.upload_ms = last_timing_.upload_ms;
        stats.present_ms = last_timing_.present_ms;
        stats.clear_ms = last_timing_.clear_ms;
        stats.copy_ms = last_timing_.copy_ms;
        stats.overlay_ms = last_timing_.overlay_ms;
        stats.swap_ms = last_timing_.swap_ms;
        stats.video_width = width_;
        stats.video_height = height_;
        stats.window_width = window_width;
        stats.window_height = window_height;
        stats.display_width = destination.w;
        stats.display_height = destination.h;
        stats.scale = width_ == 0 ? 0.0 : static_cast<double>(destination.w) / static_cast<double>(width_);

        const MemoryStats memory = current_memory_stats();
        stats.working_set_mb = memory.working_set_mb;
        stats.private_usage_mb = memory.private_usage_mb;
        stats.peak_working_set_mb = memory.peak_working_set_mb;

        if (audio != nullptr) {
            stats.audio_queued_bytes = audio->queued_size();
            stats.audio_queued_ms = audio->queued_milliseconds();
            stats.audio_target_ms = audio->target_latency_ms();
            stats.audio_underruns = audio->underruns();
            stats.audio_sample_rate = audio->sample_rate();
            stats.audio_channels = audio->channels();
        }

        return stats;
    }

    static std::array<std::uint8_t, 7> glyph(char character)
    {
        switch (character) {
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};

        case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        case ':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
        case '|': return {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case '/': return {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '>': return {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10};
        default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        }
    }

    void draw_text(int x, int y, const std::string& text)
    {
        constexpr int scale = 2;
        constexpr int glyph_width = 5;
        constexpr int glyph_height = 7;
        constexpr int glyph_spacing = 2;

        SDL_SetRenderDrawColor(renderer_.get(), 232, 238, 240, 255);
        int cursor_x = x;
        for (char raw : text) {
            const char character = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 32) : raw;
            const auto rows = glyph(character);
            for (int row = 0; row < glyph_height; ++row) {
                for (int column = 0; column < glyph_width; ++column) {
                    if ((rows[static_cast<std::size_t>(row)] & (1 << (glyph_width - column - 1))) == 0) {
                        continue;
                    }

                    SDL_Rect pixel = {cursor_x + column * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer_.get(), &pixel);
                }
            }

            cursor_x += (glyph_width + glyph_spacing) * scale;
        }
    }

    void draw_debug_overlay(const DebugStats& stats)
    {
        std::ostringstream first_line;
        first_line << std::fixed << std::setprecision(2)
                   << "TOTAL " << stats.render_frame_ms << " MS"
                   << " | DEMUX " << stats.demux_ms
                   << " | SEND " << stats.send_packet_ms
                   << " | DEC " << stats.decode_ms << " MS"
                   << " | CONV " << stats.convert_ms << " MS"
                   << " | UP " << stats.upload_ms << " MS";

        std::ostringstream second_line;
        second_line << std::fixed << std::setprecision(2)
                   << "PRESENT " << stats.present_ms << " MS"
                   << " | CLR " << stats.clear_ms
                   << " CPY " << stats.copy_ms
                   << " OVR " << stats.overlay_ms
                   << " SWP " << stats.swap_ms
                   << " | AUDIO " << stats.audio_queued_ms << " MS "
                   << "TGT " << stats.audio_target_ms << " AV " << stats.av_sync_ms
                   << " UND " << stats.audio_underruns << " "
                   << stats.audio_queued_bytes << " B "
                   << stats.audio_sample_rate << " HZ "
                   << stats.audio_channels << " CH";

        std::ostringstream third_line;
        third_line << std::fixed << std::setprecision(2)
                    << "SCALE " << stats.scale
                    << " | SOURCE " << stats.video_width << "X" << stats.video_height
                    << " -> DISPLAY " << stats.display_width << "X" << stats.display_height
                    << " | WINDOW " << stats.window_width << "X" << stats.window_height;

        std::ostringstream fourth_line;
        fourth_line << std::fixed << std::setprecision(1)
                   << "MEM WS " << stats.working_set_mb << " MB"
                   << " | PRIVATE " << stats.private_usage_mb << " MB"
                   << " | PEAK WS " << stats.peak_working_set_mb << " MB";

        constexpr int panel_height = 102;
        const int panel_y = stats.window_height > panel_height ? stats.window_height - panel_height : 0;
        SDL_Rect panel = {0, panel_y, stats.window_width, panel_height};

        SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_.get(), 12, 18, 22, 210);
        SDL_RenderFillRect(renderer_.get(), &panel);
        SDL_SetRenderDrawColor(renderer_.get(), 72, 165, 180, 255);
        SDL_RenderDrawLine(renderer_.get(), 0, panel_y, stats.window_width, panel_y);

        draw_text(12, panel_y + 10, first_line.str());
        draw_text(12, panel_y + 32, second_line.str());
        draw_text(12, panel_y + 54, third_line.str());
        draw_text(12, panel_y + 76, fourth_line.str());
        SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_NONE);
    }

    WindowPtr window_;
    RendererPtr renderer_;
    TexturePtr texture_;
    SwsPtr scaler_;
    int width_ = 0;
    int height_ = 0;
    bool direct_yuv_ = false;
    VideoFrameTiming last_timing_;
    std::vector<std::uint8_t> pixels_;
};

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
        last_audio_end_seconds.store(frame_end_seconds(*frame, stream));
        av_frame_unref(frame.get());
    }
}

class AudioDecoderWorker {
public:
    AudioDecoderWorker(CodecContextPtr codec, const AVStream& stream, double target_latency_ms)
        : codec_(std::move(codec)), stream_(stream), output_(*codec_, target_latency_ms)
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
        return last_audio_end_seconds_.load() - (output_.queued_milliseconds() / 1000.0);
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

            decode_audio_packet(*codec_, stream_, *packet, output_, last_audio_end_seconds_);
            output_.wait_until_below(output_.target_latency_ms(), stop_requested_);
        }
    }

    static constexpr std::size_t max_packets_ = 256;

    CodecContextPtr codec_;
    const AVStream& stream_;
    AudioOutput output_;
    std::deque<PacketPtr> packet_queue_;
    std::mutex mutex_;
    std::condition_variable queue_changed_;
    std::condition_variable queue_space_;
    std::atomic_bool stop_requested_ = false;
    std::atomic<double> last_audio_end_seconds_ = 0.0;
    std::thread worker_;
};

struct DecodedVideoFrame {
    FramePtr frame;
    double media_time_s = 0.0;
    PacketTiming packet_timing;
    double send_packet_ms = 0.0;
    double decode_ms = 0.0;
};

class VideoDecoderWorker {
public:
    VideoDecoderWorker(
        AVFormatContext& format,
        CodecContextPtr codec,
        int video_stream_index,
        int audio_stream_index,
        AudioDecoderWorker* audio_worker)
        : format_(format),
          codec_(std::move(codec)),
          video_stream_index_(video_stream_index),
          audio_stream_index_(audio_stream_index),
          audio_worker_(audio_worker)
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

    static constexpr std::size_t max_frames_ = 30;

    AVFormatContext& format_;
    CodecContextPtr codec_;
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    AudioDecoderWorker* audio_worker_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable frame_available_;
    std::condition_variable frame_space_;
    std::deque<DecodedVideoFrame> frames_;
    std::exception_ptr error_;
    std::atomic_bool stop_requested_ = false;
    bool eof_ = false;
    std::thread worker_;
};

void decode_video_packet(
    AVFormatContext& format,
    AVCodecContext& codec,
    int stream_index,
    const AVPacket& packet,
    PacketTiming packet_timing,
    VideoOutput& output,
    AudioOutput* audio,
    const AudioDecoderWorker* audio_worker,
    PerformanceReport& report,
    std::uint64_t& frame_number,
    bool& show_overlay,
    std::chrono::steady_clock::time_point& playback_start,
    bool& running)
{
    const auto send_start = std::chrono::steady_clock::now();
    throw_if_error(avcodec_send_packet(&codec, &packet), "send video packet");
    const auto send_end = std::chrono::steady_clock::now();
    const double send_packet_ms = std::chrono::duration<double, std::milli>(send_end - send_start).count();

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::runtime_error("failed to allocate video frame");
    }

    while (true) {
        const auto decode_start = std::chrono::steady_clock::now();
        const int result = avcodec_receive_frame(&codec, frame.get());
        const auto decode_end = std::chrono::steady_clock::now();
        const double decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_start).count();
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        throw_if_error(result, "receive video frame");

        const double target_seconds = frame_seconds(*frame, *format.streams[stream_index]);
        if (audio_worker != nullptr && audio_worker->has_clock() && audio != nullptr && audio->queued_size() > 0) {
            while (running && audio->queued_size() > 0 && target_seconds > audio_worker->audio_playback_seconds() + 0.005) {
                const auto sleep_target = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
                const EventState events = pump_events(running);
                if (events.overlay_toggle) {
                    show_overlay = !show_overlay;
                }
                std::this_thread::sleep_until(sleep_target);
            }
        } else {
            const auto target_time = playback_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(target_seconds));
            while (running && std::chrono::steady_clock::now() < target_time) {
                const auto sleep_target = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
                const EventState events = pump_events(running);
                if (events.overlay_toggle) {
                    show_overlay = !show_overlay;
                }
                if (events.window_interaction) {
                    const auto pause_start = std::chrono::steady_clock::now();
                    std::this_thread::sleep_until(pause_start + std::chrono::milliseconds(120));
                    playback_start += std::chrono::steady_clock::now() - pause_start;
                } else {
                    std::this_thread::sleep_until(sleep_target);
                }
            }
        }

        if (!running) {
            break;
        }

        if (audio_worker != nullptr && audio_worker->has_clock() && audio != nullptr && audio->queued_size() > 0) {
            const double audio_seconds = audio_worker->audio_playback_seconds();
            if (target_seconds + 0.250 < audio_seconds) {
                av_frame_unref(frame.get());
                continue;
            }
        }

        const VideoFrameTiming timing =
            output.render(*frame, audio, packet_timing.demux_ms, send_packet_ms, decode_ms, show_overlay);
        const SDL_Rect video_rect = output.current_video_rect();

        PerformanceSample sample;
        sample.frame_number = ++frame_number;
        sample.media_time_s = target_seconds;
        sample.timing = timing;
        sample.memory = current_memory_stats();
        sample.audio_queued_bytes = audio != nullptr ? audio->queued_size() : 0;
        sample.audio_queued_ms = audio != nullptr ? audio->queued_milliseconds() : 0.0;
        sample.audio_target_ms = audio != nullptr ? audio->target_latency_ms() : 0.0;
        sample.av_sync_ms =
            audio_worker != nullptr && audio_worker->has_clock() ? (target_seconds - audio_worker->audio_playback_seconds()) * 1000.0 : 0.0;
        sample.audio_underruns = audio != nullptr ? audio->underruns() : 0;
        sample.window_width = output.window_width();
        sample.window_height = output.window_height();
        sample.display_width = video_rect.w;
        sample.display_height = video_rect.h;
        report.write(sample);

        av_frame_unref(frame.get());
    }
}

int run(const std::string& path)
{
    FormatPtr format = open_input(path);

    StreamDecoder video = open_decoder(*format, AVMEDIA_TYPE_VIDEO);
    StreamDecoder audio = open_decoder(*format, AVMEDIA_TYPE_AUDIO);

    if (!video.codec && !audio.codec) {
        throw std::runtime_error("input has no decodable audio or video streams");
    }

    std::unique_ptr<VideoOutput> video_output;
    VideoStreamInfo video_info;
    if (video.codec) {
        video_info = video_stream_info(*format->streams[video.stream_index], *video.codec);
        video_output = std::make_unique<VideoOutput>(*video.codec);
    }

    std::unique_ptr<AudioDecoderWorker> audio_worker;
    const double audio_target_ms = configured_audio_target_ms();
    if (audio.codec) {
        audio_worker =
            std::make_unique<AudioDecoderWorker>(std::move(audio.codec), *format->streams[audio.stream_index], audio_target_ms);
    }

    bool running = true;
    bool show_overlay = false;
    bool playback_started = audio_worker == nullptr;
    std::uint64_t frame_number = 0;
    PerformanceReport report(path, video_info, audio_target_ms);
    auto playback_start = std::chrono::steady_clock::now();
    auto last_loop_time = playback_start;

    if (video.codec) {
        VideoDecoderWorker video_worker(
            *format,
            std::move(video.codec),
            video.stream_index,
            audio.stream_index,
            audio_worker.get());

        while (running && !video_worker.finished()) {
            const EventState events = pump_events(running);
            if (events.overlay_toggle) {
                show_overlay = !show_overlay;
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

            if (!playback_started && audio_worker && audio_worker->preroll_ready()) {
                playback_started = true;
                playback_start = std::chrono::steady_clock::now();
                last_loop_time = playback_start;
            }

            if (!playback_started) {
                std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
                continue;
            }

            std::optional<DecodedVideoFrame> decoded = video_worker.pop();
            if (!decoded) {
                std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
                continue;
            }

            const double target_seconds = decoded->media_time_s;
            AudioOutput* audio_output = audio_worker ? &audio_worker->output() : nullptr;
            if (audio_worker != nullptr && audio_worker->has_clock() && audio_output != nullptr && audio_output->queued_size() > 0) {
                const double audio_seconds = audio_worker->audio_playback_seconds();
                if (target_seconds + 0.250 < audio_seconds) {
                    continue;
                }

                while (running
                    && audio_output->queued_size() > 0
                    && target_seconds > audio_worker->audio_playback_seconds() + 0.005) {
                    const auto sleep_target = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
                    const EventState wait_events = pump_events(running);
                    if (wait_events.overlay_toggle) {
                        show_overlay = !show_overlay;
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
                    std::this_thread::sleep_until(sleep_target);
                }
            }

            if (!running) {
                break;
            }

            if (audio_worker != nullptr && audio_worker->has_clock() && audio_output != nullptr && audio_output->queued_size() > 0) {
                const double audio_seconds = audio_worker->audio_playback_seconds();
                if (target_seconds + 0.250 < audio_seconds) {
                    continue;
                }
            }

            const VideoFrameTiming timing = video_output->render(
                *decoded->frame,
                audio_output,
                decoded->packet_timing.demux_ms,
                decoded->send_packet_ms,
                decoded->decode_ms,
                show_overlay);
            const SDL_Rect video_rect = video_output->current_video_rect();

            PerformanceSample sample;
            sample.frame_number = ++frame_number;
            sample.media_time_s = target_seconds;
            sample.timing = timing;
            sample.memory = current_memory_stats();
            sample.audio_queued_bytes = audio_output != nullptr ? audio_output->queued_size() : 0;
            sample.audio_queued_ms = audio_output != nullptr ? audio_output->queued_milliseconds() : 0.0;
            sample.audio_target_ms = audio_output != nullptr ? audio_output->target_latency_ms() : 0.0;
            sample.av_sync_ms =
                audio_worker != nullptr && audio_worker->has_clock() ? (target_seconds - audio_worker->audio_playback_seconds()) * 1000.0 : 0.0;
            sample.audio_underruns = audio_output != nullptr ? audio_output->underruns() : 0;
            sample.window_width = video_output->window_width();
            sample.window_height = video_output->window_height();
            sample.display_width = video_rect.w;
            sample.display_height = video_rect.h;
            report.write(sample);
        }
    } else {
        PacketPtr packet(av_packet_alloc());
        if (!packet) {
            throw std::runtime_error("failed to allocate packet");
        }

        while (running) {
            const EventState events = pump_events(running);
            if (events.overlay_toggle) {
                show_overlay = !show_overlay;
            }
            if (!running) {
                break;
            }

            const int read_result = av_read_frame(format.get(), packet.get());
            if (read_result == AVERROR_EOF) {
                break;
            }
            throw_if_error(read_result, "read frame");

            if (audio_worker && packet->stream_index == audio.stream_index) {
                audio_worker->push(*packet);
            }

            av_packet_unref(packet.get());
        }
    }

    while (audio_worker && audio_worker->output().queued_size() > 0) {
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

int main(int argc, char** argv)
{
    try {
        const SdlGuard sdl;
        const AvNetworkGuard network;
        const std::optional<std::string> path = argc > 1
            ? std::optional<std::string>(argv[1])
            : open_media_file_dialog();
        if (!path) {
            return 0;
        }

        return run(*path);
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Playback error", error.what(), nullptr);
        return 1;
    }
}
