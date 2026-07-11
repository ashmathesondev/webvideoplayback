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
#include <cstdint>
#include <array>
#include <iomanip>
#include <iostream>
#include <memory>
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
    Uint32 audio_queued_bytes = 0;
    double audio_queued_ms = 0.0;
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

struct MemoryStats {
    double working_set_mb = 0.0;
    double private_usage_mb = 0.0;
    double peak_working_set_mb = 0.0;
};

struct EventState {
    bool window_interaction = false;
};

FormatPtr open_input(const std::string& path)
{
    AVFormatContext* raw = nullptr;
    throw_if_error(avformat_open_input(&raw, path.c_str(), nullptr, nullptr), "open input");
    FormatPtr format(raw);
    throw_if_error(avformat_find_stream_info(format.get(), nullptr), "read stream info");
    return format;
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

class AudioOutput {
public:
    explicit AudioOutput(const AVCodecContext& codec)
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
        return SDL_GetQueuedAudioSize(device_);
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

private:
    static constexpr int output_sample_rate_ = 48000;
    static constexpr int output_channels_ = 2;

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec obtained_ = {};
    SwrPtr resampler_;
    std::vector<std::uint8_t> buffer_;
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

        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
        if (!renderer_) {
            throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
        }

        texture_.reset(SDL_CreateTexture(
            renderer_.get(),
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            codec.width,
            codec.height));
        if (!texture_) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") + SDL_GetError());
        }

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

        width_ = codec.width;
        height_ = codec.height;
        pixels_.resize(static_cast<std::size_t>(width_ * height_ * 4));
    }

    void render(const AVFrame& frame, const AudioOutput* audio)
    {
        const auto render_start = std::chrono::steady_clock::now();
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

        if (SDL_UpdateTexture(texture_.get(), nullptr, pixels_.data(), width_ * 4) != 0) {
            throw std::runtime_error(std::string("SDL_UpdateTexture failed: ") + SDL_GetError());
        }

        int window_width = 0;
        int window_height = 0;
        SDL_GetRendererOutputSize(renderer_.get(), &window_width, &window_height);
        const SDL_Rect video_rect = video_destination_rect(window_width, window_height);

        SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(renderer_.get());
        SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, &video_rect);
        draw_debug_overlay(make_debug_stats(audio, window_width, window_height, video_rect));
        SDL_RenderPresent(renderer_.get());

        const auto render_end = std::chrono::steady_clock::now();
        last_render_ms_ = std::chrono::duration<double, std::milli>(render_end - render_start).count();
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
        stats.render_frame_ms = last_render_ms_;
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
                   << "FRAME " << stats.render_frame_ms << " MS"
                   << " | AUDIO " << stats.audio_queued_ms << " MS "
                   << stats.audio_queued_bytes << " B "
                   << stats.audio_sample_rate << " HZ "
                   << stats.audio_channels << " CH";

        std::ostringstream second_line;
        second_line << std::fixed << std::setprecision(2)
                    << "SCALE " << stats.scale
                    << " | SOURCE " << stats.video_width << "X" << stats.video_height
                    << " -> DISPLAY " << stats.display_width << "X" << stats.display_height
                    << " | WINDOW " << stats.window_width << "X" << stats.window_height;

        std::ostringstream third_line;
        third_line << std::fixed << std::setprecision(1)
                   << "MEM WS " << stats.working_set_mb << " MB"
                   << " | PRIVATE " << stats.private_usage_mb << " MB"
                   << " | PEAK WS " << stats.peak_working_set_mb << " MB";

        constexpr int panel_height = 80;
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
        SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_NONE);
    }

    WindowPtr window_;
    RendererPtr renderer_;
    TexturePtr texture_;
    SwsPtr scaler_;
    int width_ = 0;
    int height_ = 0;
    double last_render_ms_ = 0.0;
    std::vector<std::uint8_t> pixels_;
};

void decode_audio_packet(AVCodecContext& codec, const AVPacket& packet, AudioOutput& output)
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
        av_frame_unref(frame.get());
    }
}

void decode_video_packet(
    AVFormatContext& format,
    AVCodecContext& codec,
    int stream_index,
    const AVPacket& packet,
    VideoOutput& output,
    AudioOutput* audio,
    std::chrono::steady_clock::time_point& playback_start,
    bool& running)
{
    throw_if_error(avcodec_send_packet(&codec, &packet), "send video packet");

    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::runtime_error("failed to allocate video frame");
    }

    while (true) {
        const int result = avcodec_receive_frame(&codec, frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        throw_if_error(result, "receive video frame");

        const double target_seconds = frame_seconds(*frame, *format.streams[stream_index]);
        const auto target_time = playback_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(target_seconds));
        while (running && std::chrono::steady_clock::now() < target_time) {
            const auto sleep_target = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            const EventState events = pump_events(running);
            if (events.window_interaction) {
                const auto pause_start = std::chrono::steady_clock::now();
                const auto pause_until = pause_start + std::chrono::milliseconds(120);
                if (audio != nullptr) {
                    audio->pause();
                }
                std::this_thread::sleep_until(pause_until);
                if (audio != nullptr) {
                    audio->resume();
                }
                playback_start += std::chrono::steady_clock::now() - pause_start;
            } else {
                std::this_thread::sleep_until(sleep_target);
            }
        }

        if (!running) {
            break;
        }

        output.render(*frame, audio);
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
    if (video.codec) {
        video_output = std::make_unique<VideoOutput>(*video.codec);
    }

    std::unique_ptr<AudioOutput> audio_output;
    if (audio.codec) {
        audio_output = std::make_unique<AudioOutput>(*audio.codec);
    }

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        throw std::runtime_error("failed to allocate packet");
    }

    bool running = true;
    auto playback_start = std::chrono::steady_clock::now();
    auto last_loop_time = playback_start;

    while (running) {
        const EventState events = pump_events(running);
        const auto now = std::chrono::steady_clock::now();
        const auto loop_gap = now - last_loop_time;
        if (events.window_interaction || loop_gap > std::chrono::milliseconds(250)) {
            playback_start += loop_gap;
        }
        last_loop_time = now;

        if (!running) {
            break;
        }

        const int read_result = av_read_frame(format.get(), packet.get());
        if (read_result == AVERROR_EOF) {
            break;
        }
        throw_if_error(read_result, "read frame");

        if (video.codec && packet->stream_index == video.stream_index) {
            decode_video_packet(
                *format,
                *video.codec,
                video.stream_index,
                *packet,
                *video_output,
                audio_output.get(),
                playback_start,
                running);
        } else if (audio.codec && packet->stream_index == audio.stream_index) {
            decode_audio_packet(*audio.codec, *packet, *audio_output);
        }

        av_packet_unref(packet.get());
    }

    while (audio_output && audio_output->queued_size() > 0) {
        const auto next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        pump_events(running);
        if (!running) {
            break;
        }
        std::this_thread::sleep_until(next_poll);
    }

    return 0;
}

} // namespace

int main(int, char**)
{
    try {
        const SdlGuard sdl;
        const std::optional<std::string> path = open_media_file_dialog();
        if (!path) {
            return 0;
        }

        return run(*path);
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Playback error", error.what(), nullptr);
        return 1;
    }
}
