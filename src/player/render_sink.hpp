#pragma once

#include "player/audio_output.hpp"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <memory>

namespace webvideoplayback::player {

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

class SdlTextureRenderSink final : public IRenderSink {
public:
    explicit SdlTextureRenderSink(const AVCodecContext& codec);
    ~SdlTextureRenderSink() override;

    void resize(int width, int height) override;
    VideoFrameTiming present(const RenderFrame& frame) override;
    SDL_Rect current_video_rect() const override;
    int window_width() const override;
    int window_height() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webvideoplayback::player
