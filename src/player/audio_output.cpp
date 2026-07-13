#include "player/audio_output.hpp"

#include "player/av_support.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace webvideoplayback::player {
struct AudioOutput::Impl {
    explicit Impl(const AVCodecContext& codec, double target_latency_ms)
        : target_latency_ms(target_latency_ms)
    {
        open_device(output_sample_rate, output_channels);

        AVChannelLayout output_layout;
        av_channel_layout_default(&output_layout, output_channels);

        SwrContext* raw = nullptr;
        throw_if_error(
            swr_alloc_set_opts2(
                &raw,
                &output_layout,
                AV_SAMPLE_FMT_FLT,
                output_sample_rate,
                &codec.ch_layout,
                codec.sample_fmt,
                codec.sample_rate,
                0,
                nullptr),
            "allocate audio resampler");

        resampler.reset(raw);
        throw_if_error(swr_init(resampler.get()), "initialize audio resampler");
        av_channel_layout_uninit(&output_layout);

        SDL_PauseAudioDevice(device, 0);
    }

    Impl(int sample_rate, int channels, double target_latency_ms)
        : target_latency_ms(target_latency_ms)
    {
        open_device(sample_rate, channels);
        SDL_PauseAudioDevice(device, 0);
    }

    ~Impl()
    {
        if (device != 0) {
            SDL_CloseAudioDevice(device);
        }
    }

    void open_device(int sample_rate, int channels)
    {
        SDL_AudioSpec desired = {};
        desired.freq = sample_rate;
        desired.format = AUDIO_F32SYS;
        desired.channels = static_cast<Uint8>(channels);
        desired.samples = 4096;

        device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (device == 0) {
            throw std::runtime_error(std::string("SDL_OpenAudioDevice failed: ") + SDL_GetError());
        }
    }

    static constexpr int output_sample_rate = 48000;
    static constexpr int output_channels = 2;

    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec obtained = {};
    SwrPtr resampler;
    std::vector<std::uint8_t> buffer;
    double target_latency_ms = 150.0;
    std::atomic_bool clock_ready = false;
    std::atomic_bool clock_paused = false;
    std::atomic<double> clock_media_end_seconds = 0.0;
    std::atomic<double> clock_wall_seconds = 0.0;
    std::atomic<double> clock_queued_seconds = 0.0;
    mutable std::atomic_bool was_empty = false;
    mutable std::atomic_uint64_t underruns = 0;
};

AudioOutput::AudioOutput(const AVCodecContext& codec, double target_latency_ms)
    : impl_(std::make_unique<Impl>(codec, target_latency_ms))
{
}

AudioOutput::AudioOutput(int sample_rate, int channels, double target_latency_ms)
    : impl_(std::make_unique<Impl>(sample_rate, channels, target_latency_ms))
{
}

AudioOutput::~AudioOutput() = default;

void AudioOutput::queue(const AVFrame& frame)
{
    const int output_samples = swr_get_out_samples(impl_->resampler.get(), frame.nb_samples);
    const int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLT);
    const int buffer_size = output_samples * Impl::output_channels * bytes_per_sample;
    impl_->buffer.resize(static_cast<std::size_t>(buffer_size));

    auto* out = impl_->buffer.data();
    const int converted = swr_convert(
        impl_->resampler.get(),
        &out,
        output_samples,
        frame.extended_data,
        frame.nb_samples);

    throw_if_error(converted, "convert audio frame");

    const int bytes = converted * Impl::output_channels * bytes_per_sample;
    if (SDL_QueueAudio(impl_->device, impl_->buffer.data(), static_cast<Uint32>(bytes)) != 0) {
        throw std::runtime_error(std::string("SDL_QueueAudio failed: ") + SDL_GetError());
    }
}

void AudioOutput::queue_float_pcm(const void* data, std::size_t byte_count)
{
    if (SDL_QueueAudio(impl_->device, data, static_cast<Uint32>(byte_count)) != 0) {
        throw std::runtime_error(std::string("SDL_QueueAudio failed: ") + SDL_GetError());
    }
}

void AudioOutput::mark_media_end(double media_end_seconds)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double now_seconds = std::chrono::duration<double>(now).count();
    const double queued_seconds = queued_milliseconds() / 1000.0;

    impl_->clock_media_end_seconds.store(media_end_seconds);
    impl_->clock_wall_seconds.store(now_seconds);
    impl_->clock_queued_seconds.store(queued_seconds);
    impl_->clock_ready.store(true);
}

double AudioOutput::playback_seconds() const
{
    if (!impl_->clock_ready.load()) {
        return 0.0;
    }

    return impl_->clock_media_end_seconds.load() - clock_remaining_seconds();
}

double AudioOutput::clock_remaining_seconds() const
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double now_seconds = std::chrono::duration<double>(now).count();
    const double wall_seconds = impl_->clock_wall_seconds.load();
    const double queued_seconds = impl_->clock_queued_seconds.load();
    const double elapsed_seconds = impl_->clock_paused.load() ? 0.0 : std::max(0.0, now_seconds - wall_seconds);
    return std::max(0.0, queued_seconds - elapsed_seconds);
}

Uint32 AudioOutput::queued_size() const
{
    const Uint32 size = SDL_GetQueuedAudioSize(impl_->device);
    if (size == 0) {
        bool expected = false;
        if (impl_->was_empty.compare_exchange_strong(expected, true)) {
            impl_->underruns.fetch_add(1);
        }
    } else {
        impl_->was_empty.store(false);
    }

    return size;
}

double AudioOutput::queued_milliseconds() const
{
    const int bytes_per_frame = impl_->obtained.channels * static_cast<int>(sizeof(float));
    if (bytes_per_frame == 0 || impl_->obtained.freq == 0) {
        return 0.0;
    }

    const double queued_frames = static_cast<double>(queued_size()) / static_cast<double>(bytes_per_frame);
    return queued_frames * 1000.0 / static_cast<double>(impl_->obtained.freq);
}

int AudioOutput::sample_rate() const
{
    return impl_->obtained.freq;
}

int AudioOutput::channels() const
{
    return impl_->obtained.channels;
}

void AudioOutput::pause()
{
    if (impl_->clock_ready.load()) {
        impl_->clock_queued_seconds.store(clock_remaining_seconds());
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        impl_->clock_wall_seconds.store(std::chrono::duration<double>(now).count());
    }
    impl_->clock_paused.store(true);
    SDL_PauseAudioDevice(impl_->device, 1);
}

void AudioOutput::resume()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    impl_->clock_wall_seconds.store(std::chrono::duration<double>(now).count());
    impl_->clock_paused.store(false);
    SDL_PauseAudioDevice(impl_->device, 0);
}

void AudioOutput::wait_until_below(double queued_ms, const std::atomic_bool& stop_requested)
{
    while (!stop_requested.load() && queued_milliseconds() > queued_ms) {
        std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
    }
}

double AudioOutput::target_latency_ms() const
{
    return impl_->target_latency_ms;
}

std::uint64_t AudioOutput::underruns() const
{
    return impl_->underruns.load();
}

} // namespace webvideoplayback::player
