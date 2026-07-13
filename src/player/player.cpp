// Native media player used by the playback test harness.
//
// The player opens local files or HTTP URLs through FFmpeg, decodes audio and
// video on worker threads, and keeps SDL rendering on the main thread. Audio is
// the master playback clock. Window moves and resizes explicitly pause decode
// and audio output, then resume once interaction settles.
#include "player/decoder_backend.hpp"
#include "player/playback_pause.hpp"
#include "player/sdl_media.hpp"

#include <SDL.h>

#include <chrono>
#include <ctime>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
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
    PerformanceReport(
        bool enabled,
        const std::string& media_path,
        const VideoStreamInfo& video_info,
        double audio_target_ms,
        const std::string& backend_name)
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
        file_ << "decoder_backend," << csv_escape(backend_name) << "\n";
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
    DecoderBackendPreference decoder_backend = configured_decoder_backend_preference();
    bool performance_report = false;
};

PlayerOptions parse_player_options(int argc, char** argv)
{
    PlayerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--performance-report" || arg == "--perf-report") {
            options.performance_report = true;
        } else if (arg == "--decoder-backend" && index + 1 < argc) {
            options.decoder_backend = parse_decoder_backend_preference(argv[++index]);
        } else if (!options.media_path) {
            options.media_path = arg;
        } else {
            throw std::runtime_error("usage: webvideoplayback.exe [--performance-report] [--decoder-backend auto|ffmpeg|native] [media-path-or-url]");
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



int run(const std::string& path, bool performance_report_enabled, DecoderBackendPreference decoder_backend)
{
    PlaybackPause playback_pause;
    const double audio_target_ms = configured_audio_target_ms();
    DecoderBackendSelection backend = create_decoder_backend(path, audio_target_ms, playback_pause, decoder_backend);
    FfmpegMediaDecoder& decoder = *backend.decoder;
    const MediaInfo media_info = decoder.media_info();

    std::unique_ptr<IRenderSink> render_sink;
    if (media_info.has_video) {
        render_sink = std::make_unique<SdlTextureRenderSink>(media_info.video_render);
    }
    decoder.start();

    bool running = true;
    bool show_overlay = false;
    bool playback_started = !decoder.has_audio();
    std::uint64_t frame_number = 0;
    PerformanceReport report(performance_report_enabled, path, media_info.video, audio_target_ms, backend.backend_name);
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

        return run(*options.media_path, options.performance_report, options.decoder_backend);
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Playback error", error.what(), nullptr);
        return 1;
    }
}
