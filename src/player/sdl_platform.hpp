#pragma once

#include <optional>
#include <string>

namespace webvideoplayback::player {

std::optional<std::string> open_media_file_dialog();
double configured_audio_target_ms();

struct MemoryStats {
    double working_set_mb = 0.0;
    double private_usage_mb = 0.0;
    double peak_working_set_mb = 0.0;
};

MemoryStats current_memory_stats();

struct SdlGuard {
    SdlGuard();
    ~SdlGuard();
};

} // namespace webvideoplayback::player
