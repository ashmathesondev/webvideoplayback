#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

namespace webvideoplayback::player {

std::string av_error(int code);
void throw_if_error(int code, const std::string& context);

struct AvNetworkGuard {
    AvNetworkGuard();
    ~AvNetworkGuard();
};

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

struct StreamDecoder {
    int stream_index = -1;
    CodecContextPtr codec;
};

struct VideoStreamInfo {
    std::string codec_name;
    std::string pixel_format;
    double fps = 0.0;
    int bit_rate = 0;
};

struct PacketTiming {
    double demux_ms = 0.0;
};

FormatPtr open_input(const std::string& path);
double stream_fps(const AVStream& stream);
VideoStreamInfo video_stream_info(const AVStream& stream, const AVCodecContext& codec);
StreamDecoder open_decoder(AVFormatContext& format, AVMediaType type);
double frame_seconds(const AVFrame& frame, const AVStream& stream);
double frame_end_seconds(const AVFrame& frame, const AVStream& stream);
double packet_seconds(const AVPacket& packet, const AVStream& stream);

} // namespace webvideoplayback::player
