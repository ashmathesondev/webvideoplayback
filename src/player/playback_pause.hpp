#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace webvideoplayback::player {

class PlaybackPause {
public:
    void set_paused(bool paused)
    {
        paused_.store(paused);
    }

    bool paused() const
    {
        return paused_.load();
    }

    void wait(const std::atomic_bool& stop_requested) const
    {
        while (!stop_requested.load() && paused_.load()) {
            std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
        }
    }

private:
    std::atomic_bool paused_ = false;
};

} // namespace webvideoplayback::player
