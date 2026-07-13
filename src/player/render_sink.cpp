#include "player/render_sink.hpp"

#include "player/av_support.hpp"
#include "player/sdl_platform.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace webvideoplayback::player {
namespace {

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

std::array<std::uint8_t, 7> glyph(char character)
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

} // namespace

struct SdlTextureRenderSink::Impl {
    Impl(const VideoRenderConfig& config, const std::string& backend_name)
    {
        const std::string window_title = "Web Video Playback [" + backend_name + "]";
        window.reset(SDL_CreateWindow(
            window_title.c_str(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            config.width,
            config.height,
            SDL_WINDOW_RESIZABLE));
        if (!window) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED));
        if (!renderer) {
            throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
        }

        texture.reset(SDL_CreateTexture(
            renderer.get(),
            config.pixel_format == AV_PIX_FMT_YUV420P ? SDL_PIXELFORMAT_IYUV : SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            config.width,
            config.height));
        if (!texture) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") + SDL_GetError());
        }

        direct_yuv = config.pixel_format == AV_PIX_FMT_YUV420P;
        if (!direct_yuv) {
            scaler.reset(sws_getContext(
                config.width,
                config.height,
                config.pixel_format,
                config.width,
                config.height,
                AV_PIX_FMT_RGBA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr));
            if (!scaler) {
                throw std::runtime_error("failed to create video scaler");
            }
        }

        width = config.width;
        height = config.height;
        if (!direct_yuv) {
            pixels.resize(static_cast<std::size_t>(width * height * 4));
        }
    }

    SDL_Rect video_destination_rect(int window_width, int window_height) const
    {
        if (width <= 0 || height <= 0 || window_width <= 0 || window_height <= 0) {
            return {0, 0, window_width, window_height};
        }

        const double width_scale = static_cast<double>(window_width) / static_cast<double>(width);
        const double height_scale = static_cast<double>(window_height) / static_cast<double>(height);
        const double scale = width_scale < height_scale ? width_scale : height_scale;
        int display_width = static_cast<int>(static_cast<double>(width) * scale + 0.5);
        int display_height = static_cast<int>(static_cast<double>(height) * scale + 0.5);
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
        stats.render_frame_ms = last_timing.total_ms;
        stats.demux_ms = last_timing.demux_ms;
        stats.send_packet_ms = last_timing.send_packet_ms;
        stats.decode_ms = last_timing.decode_ms;
        stats.convert_ms = last_timing.convert_ms;
        stats.upload_ms = last_timing.upload_ms;
        stats.present_ms = last_timing.present_ms;
        stats.clear_ms = last_timing.clear_ms;
        stats.copy_ms = last_timing.copy_ms;
        stats.overlay_ms = last_timing.overlay_ms;
        stats.swap_ms = last_timing.swap_ms;
        stats.video_width = width;
        stats.video_height = height;
        stats.window_width = window_width;
        stats.window_height = window_height;
        stats.display_width = destination.w;
        stats.display_height = destination.h;
        stats.scale = width == 0 ? 0.0 : static_cast<double>(destination.w) / static_cast<double>(width);

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

    void draw_text(int x, int y, const std::string& text)
    {
        constexpr int scale = 2;
        constexpr int glyph_width = 5;
        constexpr int glyph_height = 7;
        constexpr int glyph_spacing = 2;

        SDL_SetRenderDrawColor(renderer.get(), 232, 238, 240, 255);
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
                    SDL_RenderFillRect(renderer.get(), &pixel);
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
                    << "TGT " << stats.audio_target_ms
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

        SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer.get(), 12, 18, 22, 210);
        SDL_RenderFillRect(renderer.get(), &panel);
        SDL_SetRenderDrawColor(renderer.get(), 72, 165, 180, 255);
        SDL_RenderDrawLine(renderer.get(), 0, panel_y, stats.window_width, panel_y);

        draw_text(12, panel_y + 10, first_line.str());
        draw_text(12, panel_y + 32, second_line.str());
        draw_text(12, panel_y + 54, third_line.str());
        draw_text(12, panel_y + 76, fourth_line.str());
        SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_NONE);
    }

    WindowPtr window;
    RendererPtr renderer;
    TexturePtr texture;
    SwsPtr scaler;
    int width = 0;
    int height = 0;
    bool direct_yuv = false;
    VideoFrameTiming last_timing;
    std::vector<std::uint8_t> pixels;
};

SdlTextureRenderSink::SdlTextureRenderSink(const VideoRenderConfig& config, const std::string& backend_name)
    : impl_(std::make_unique<Impl>(config, backend_name))
{
}

SdlTextureRenderSink::~SdlTextureRenderSink() = default;

void SdlTextureRenderSink::resize(int width, int height)
{
    if (width > 0 && height > 0) {
        SDL_SetWindowSize(impl_->window.get(), width, height);
    }
}

VideoFrameTiming SdlTextureRenderSink::present(const RenderFrame& frame)
{
    const auto total_start = std::chrono::steady_clock::now();

    const auto convert_start = std::chrono::steady_clock::now();
    if (!impl_->direct_yuv) {
        std::uint8_t* scaled_pixels[] = {impl_->pixels.data()};
        const int linesize[] = {impl_->width * 4};
        sws_scale(
            impl_->scaler.get(),
            frame.cpu_frame.data,
            frame.cpu_frame.linesize,
            0,
            impl_->height,
            scaled_pixels,
            linesize);
    }
    const auto convert_end = std::chrono::steady_clock::now();

    const auto upload_start = std::chrono::steady_clock::now();
    if (impl_->direct_yuv) {
        if (SDL_UpdateYUVTexture(
                impl_->texture.get(),
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
        if (SDL_UpdateTexture(impl_->texture.get(), nullptr, impl_->pixels.data(), impl_->width * 4) != 0) {
            throw std::runtime_error(std::string("SDL_UpdateTexture failed: ") + SDL_GetError());
        }
    }
    const auto upload_end = std::chrono::steady_clock::now();

    int window_width = 0;
    int window_height = 0;
    SDL_GetRendererOutputSize(impl_->renderer.get(), &window_width, &window_height);
    const SDL_Rect video_rect = impl_->video_destination_rect(window_width, window_height);

    SDL_SetRenderDrawColor(impl_->renderer.get(), 0, 0, 0, 255);
    const auto present_start = std::chrono::steady_clock::now();
    const auto clear_start = std::chrono::steady_clock::now();
    SDL_RenderClear(impl_->renderer.get());
    const auto clear_end = std::chrono::steady_clock::now();
    const auto copy_start = std::chrono::steady_clock::now();
    SDL_RenderCopy(impl_->renderer.get(), impl_->texture.get(), nullptr, &video_rect);
    const auto copy_end = std::chrono::steady_clock::now();
    const auto overlay_start = std::chrono::steady_clock::now();
    if (frame.show_overlay) {
        impl_->draw_debug_overlay(impl_->make_debug_stats(frame.audio, window_width, window_height, video_rect));
    }
    const auto overlay_end = std::chrono::steady_clock::now();
    const auto swap_start = std::chrono::steady_clock::now();
    SDL_RenderPresent(impl_->renderer.get());
    const auto swap_end = std::chrono::steady_clock::now();
    const auto present_end = std::chrono::steady_clock::now();

    const auto total_end = std::chrono::steady_clock::now();
    impl_->last_timing.demux_ms = frame.demux_ms;
    impl_->last_timing.send_packet_ms = frame.send_packet_ms;
    impl_->last_timing.decode_ms = frame.decode_ms;
    impl_->last_timing.convert_ms = std::chrono::duration<double, std::milli>(convert_end - convert_start).count();
    impl_->last_timing.upload_ms = std::chrono::duration<double, std::milli>(upload_end - upload_start).count();
    impl_->last_timing.present_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();
    impl_->last_timing.clear_ms = std::chrono::duration<double, std::milli>(clear_end - clear_start).count();
    impl_->last_timing.copy_ms = std::chrono::duration<double, std::milli>(copy_end - copy_start).count();
    impl_->last_timing.overlay_ms = std::chrono::duration<double, std::milli>(overlay_end - overlay_start).count();
    impl_->last_timing.swap_ms = std::chrono::duration<double, std::milli>(swap_end - swap_start).count();
    impl_->last_timing.total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    return impl_->last_timing;
}

SDL_Rect SdlTextureRenderSink::current_video_rect() const
{
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(impl_->renderer.get(), &width, &height);
    return impl_->video_destination_rect(width, height);
}

int SdlTextureRenderSink::window_width() const
{
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(impl_->renderer.get(), &width, &height);
    return width;
}

int SdlTextureRenderSink::window_height() const
{
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(impl_->renderer.get(), &width, &height);
    return height;
}

} // namespace webvideoplayback::player
