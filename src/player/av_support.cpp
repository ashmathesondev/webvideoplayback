#include "player/av_support.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <stdexcept>

namespace webvideoplayback::player {

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

AvNetworkGuard::AvNetworkGuard()
{
    throw_if_error(avformat_network_init(), "initialize FFmpeg network");
}

AvNetworkGuard::~AvNetworkGuard()
{
    avformat_network_deinit();
}

void FormatDeleter::operator()(AVFormatContext* context) const
{
    avformat_close_input(&context);
}

void CodecContextDeleter::operator()(AVCodecContext* context) const
{
    avcodec_free_context(&context);
}

void FrameDeleter::operator()(AVFrame* frame) const
{
    av_frame_free(&frame);
}

void PacketDeleter::operator()(AVPacket* packet) const
{
    av_packet_free(&packet);
}

void SwsDeleter::operator()(SwsContext* context) const
{
    sws_freeContext(context);
}

void SwrDeleter::operator()(SwrContext* context) const
{
    swr_free(&context);
}

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

double packet_seconds(const AVPacket& packet, const AVStream& stream)
{
    int64_t timestamp = packet.pts;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = packet.dts;
    }
    if (timestamp == AV_NOPTS_VALUE) {
        return 0.0;
    }

    return static_cast<double>(timestamp) * av_q2d(stream.time_base);
}

} // namespace webvideoplayback::player
