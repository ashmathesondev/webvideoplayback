#include "player/sdl_platform.hpp"

#include "common/string_utils.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#endif

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace webvideoplayback::player {

#ifdef _WIN32
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

    return webvideoplayback::utils::wide_to_utf8(path.c_str());
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

SdlGuard::SdlGuard()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
}

SdlGuard::~SdlGuard()
{
    SDL_Quit();
}

} // namespace webvideoplayback::player
