#pragma once

// FFmpeg support layer.
//
// This module owns FFmpeg RAII wrappers, decoder setup, timestamp helpers, and
// error conversion. SDL and playback policy code consume these types instead of
// directly managing FFmpeg lifetimes.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <optional>
#include <string>

namespace webvideoplayback::player {

// Converts a negative FFmpeg error code to readable text.
std::string av_error(int code);

// Throws std::runtime_error when an FFmpeg call returns an error.
void throw_if_error(int code, const std::string& context);

// Initializes process-wide FFmpeg networking for URL playback.
struct AvNetworkGuard {
    AvNetworkGuard();
    ~AvNetworkGuard();
};

// unique_ptr deleters for FFmpeg allocation families.
struct FormatDeleter {
    void operator()(AVFormatContext* context) const;
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const;
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const;
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const;
};

struct SwsDeleter {
    void operator()(SwsContext* context) const;
};

struct SwrDeleter {
    void operator()(SwrContext* context) const;
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

// Decoder plus stream index returned by stream discovery.
struct StreamDecoder {
    int stream_index = -1;
    CodecContextPtr codec;
};

// Metadata included in performance reports.
struct VideoStreamInfo {
    std::string codec_name;
    std::string pixel_format;
    double fps = 0.0;
    int bit_rate = 0;
};

struct VideoRenderConfig {
    int width = 0;
    int height = 0;
    AVPixelFormat pixel_format = AV_PIX_FMT_NONE;
};

// Timing captured while demuxing one packet.
struct PacketTiming {
    double demux_ms = 0.0;
};

struct DecodedVideoFrame {
    FramePtr frame;
    double media_time_s = 0.0;
    PacketTiming packet_timing;
    double send_packet_ms = 0.0;
    double decode_ms = 0.0;
};

struct DecodedAudioFrame {
    FramePtr frame;
    double media_end_s = 0.0;
};

struct MediaInfo {
    VideoStreamInfo video;
    VideoRenderConfig video_render;
    bool has_video = false;
    bool has_audio = false;
};

class IMediaDecoder {
public:
    virtual ~IMediaDecoder() = default;

    virtual MediaInfo media_info() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void seek(double seconds) = 0;
    virtual std::optional<DecodedVideoFrame> pop_video_frame() = 0;
    virtual std::optional<DecodedAudioFrame> pop_audio_frame() = 0;
};

// Opens a local file or URL and reads stream metadata.
FormatPtr open_input(const std::string& path);

// Derives stream frame rate from FFmpeg metadata.
double stream_fps(const AVStream& stream);

// Builds user-facing video metadata from the stream and codec.
VideoStreamInfo video_stream_info(const AVStream& stream, const AVCodecContext& codec);
VideoRenderConfig video_render_config(const AVCodecContext& codec);

// Finds and opens the best decoder for the requested media type.
StreamDecoder open_decoder(AVFormatContext& format, AVMediaType type);

// Converts FFmpeg frame or packet timestamps to media seconds.
double frame_seconds(const AVFrame& frame, const AVStream& stream);
double frame_end_seconds(const AVFrame& frame, const AVStream& stream);
double packet_seconds(const AVPacket& packet, const AVStream& stream);

} // namespace webvideoplayback::player
