#include "PlaybackController.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace debrief {

static uint64_t wall_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

PlaybackController::PlaybackController() : current_ns_(wall_ns()) {}

void PlaybackController::play(float speed) noexcept {
    speed_  = (speed == 0.0f) ? 1.0f : speed;
    state_  = PlaybackState::Playing;
}

void PlaybackController::pause() noexcept {
    if (state_ == PlaybackState::Playing)
        state_ = PlaybackState::Paused;
}

void PlaybackController::stop() noexcept {
    state_      = PlaybackState::Live;
    speed_      = 1.0f;
    current_ns_ = wall_ns();
}

void PlaybackController::set_speed(float s) noexcept {
    // Skip through zero — no "stopped" speed; use pause() for that.
    if (s == 0.0f) s = (speed_ > 0) ? 0.1f : -0.1f;
    speed_ = std::clamp(s, -16.0f, 16.0f);
}

void PlaybackController::seek(uint64_t ts_ns) noexcept {
    current_ns_ = ts_ns;
    if (state_ == PlaybackState::Live)
        state_ = PlaybackState::Paused;
}

void PlaybackController::tick(float dt_seconds) noexcept {
    switch (state_) {
    case PlaybackState::Live:
        current_ns_ = wall_ns();
        break;
    case PlaybackState::Playing: {
        int64_t delta = static_cast<int64_t>(dt_seconds * speed_ * 1e9f);
        int64_t next  = static_cast<int64_t>(current_ns_) + delta;
        current_ns_   = (next < 0) ? 0 : static_cast<uint64_t>(next);
        break;
    }
    case PlaybackState::Paused:
        break;
    }
}

float PlaybackController::scrub_pos(uint64_t start_ns, uint64_t end_ns) const noexcept {
    if (end_ns <= start_ns) return 1.0f;
    float t = static_cast<float>(current_ns_ - start_ns) /
              static_cast<float>(end_ns - start_ns);
    return std::clamp(t, 0.0f, 1.0f);
}

uint64_t PlaybackController::scrub_to_ns(float frac,
                                          uint64_t start_ns,
                                          uint64_t end_ns) noexcept
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    return start_ns + static_cast<uint64_t>(frac * static_cast<float>(end_ns - start_ns));
}

void PlaybackController::clamp(uint64_t min_ns, uint64_t max_ns) noexcept {
    if (current_ns_ < min_ns) current_ns_ = min_ns;
    if (current_ns_ > max_ns) current_ns_ = max_ns;
}

} // namespace debrief
