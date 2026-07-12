// SDL platform, audio output, and video rendering support.
#pragma once

#include "common/string_utils.hpp"
#include "player/av_support.hpp"

#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#endif

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace webvideoplayback::player {

#ifdef _WIN32
inline std::optional<std::string> open_media_file_dialog()
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

    return webvideoplayback::utils::wide_to_utf8(path.c_str());
}
#else
inline std::optional<std::string> open_media_file_dialog()
{
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "Open media file",
        "Native file picker is not implemented for this platform yet.",
        nullptr);
    return std::nullopt;
}
#endif

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

using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;
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
struct MemoryStats {
    double working_set_mb = 0.0;
    double private_usage_mb = 0.0;
    double peak_working_set_mb = 0.0;
};
inline MemoryStats current_memory_stats()
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

inline double configured_audio_target_ms()
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

    void mark_media_end(double media_end_seconds)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const double now_seconds = std::chrono::duration<double>(now).count();
        const double queued_seconds = queued_milliseconds() / 1000.0;

        clock_media_end_seconds_.store(media_end_seconds);
        clock_wall_seconds_.store(now_seconds);
        clock_queued_seconds_.store(queued_seconds);
        clock_ready_.store(true);
    }

    double playback_seconds() const
    {
        if (!clock_ready_.load()) {
            return 0.0;
        }

        return clock_media_end_seconds_.load() - clock_remaining_seconds();
    }

    double clock_remaining_seconds() const
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const double now_seconds = std::chrono::duration<double>(now).count();
        const double wall_seconds = clock_wall_seconds_.load();
        const double queued_seconds = clock_queued_seconds_.load();
        const double elapsed_seconds = clock_paused_.load() ? 0.0 : std::max(0.0, now_seconds - wall_seconds);
        return std::max(0.0, queued_seconds - elapsed_seconds);
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
        if (clock_ready_.load()) {
            clock_queued_seconds_.store(clock_remaining_seconds());
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            clock_wall_seconds_.store(std::chrono::duration<double>(now).count());
        }
        clock_paused_.store(true);
        SDL_PauseAudioDevice(device_, 1);
    }

    void resume()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        clock_wall_seconds_.store(std::chrono::duration<double>(now).count());
        clock_paused_.store(false);
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
    std::atomic_bool clock_ready_ = false;
    std::atomic_bool clock_paused_ = false;
    std::atomic<double> clock_media_end_seconds_ = 0.0;
    std::atomic<double> clock_wall_seconds_ = 0.0;
    std::atomic<double> clock_queued_seconds_ = 0.0;
    mutable std::atomic_bool was_empty_ = false;
    mutable std::atomic_uint64_t underruns_ = 0;
};

struct RenderFrame {
    const AVFrame& cpu_frame;
    const AudioOutput* audio = nullptr;
    double demux_ms = 0.0;
    double send_packet_ms = 0.0;
    double decode_ms = 0.0;
    bool show_overlay = false;
};

class IRenderSink {
public:
    virtual ~IRenderSink() = default;

    virtual void resize(int width, int height) = 0;
    virtual VideoFrameTiming present(const RenderFrame& frame) = 0;
    virtual SDL_Rect current_video_rect() const = 0;
    virtual int window_width() const = 0;
    virtual int window_height() const = 0;
};

// Owns all SDL texture resources. Only the main thread calls this class.
class SdlTextureRenderSink final : public IRenderSink {
public:
    explicit SdlTextureRenderSink(const AVCodecContext& codec)
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

    void resize(int width, int height) override
    {
        if (width > 0 && height > 0) {
            SDL_SetWindowSize(window_.get(), width, height);
        }
    }

    VideoFrameTiming present(const RenderFrame& frame) override
    {
        const auto total_start = std::chrono::steady_clock::now();

        const auto convert_start = std::chrono::steady_clock::now();
        if (!direct_yuv_) {
            std::uint8_t* scaled_pixels[] = {pixels_.data()};
            const int linesize[] = {width_ * 4};
            sws_scale(
                scaler_.get(),
                frame.cpu_frame.data,
                frame.cpu_frame.linesize,
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
                    frame.cpu_frame.data[0],
                    frame.cpu_frame.linesize[0],
                    frame.cpu_frame.data[1],
                    frame.cpu_frame.linesize[1],
                    frame.cpu_frame.data[2],
                    frame.cpu_frame.linesize[2])
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
        if (frame.show_overlay) {
            draw_debug_overlay(make_debug_stats(frame.audio, window_width, window_height, video_rect));
        }
        const auto overlay_end = std::chrono::steady_clock::now();
        const auto swap_start = std::chrono::steady_clock::now();
        SDL_RenderPresent(renderer_.get());
        const auto swap_end = std::chrono::steady_clock::now();
        const auto present_end = std::chrono::steady_clock::now();

        const auto total_end = std::chrono::steady_clock::now();
        last_timing_.demux_ms = frame.demux_ms;
        last_timing_.send_packet_ms = frame.send_packet_ms;
        last_timing_.decode_ms = frame.decode_ms;
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

    int window_width() const override
    {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &width, &height);
        return width;
    }

    int window_height() const override
    {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &width, &height);
        return height;
    }

    SDL_Rect current_video_rect() const override
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


} // namespace webvideoplayback::player

