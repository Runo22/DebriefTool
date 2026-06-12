#pragma once
#include <cstdint>
#include <chrono>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackController  —  VCR state machine
//
//  Manages the playback timeline position, speed, and mode.
//  The result (`current_time_ns()`) is read each frame by the interpolation
//  system and the UI scrubber.
//
//  Live mode:     current_time tracks wall clock (real-time telemetry).
//  Playback mode: current_time advances at `speed` × wall clock rate.
//
//  Speed:  1.0 = real-time,  2.0 = 2× fast-forward,  -1.0 = rewind.
//  Range:  typically [0.1, 8.0] forward, [-4.0, -0.1] rewind.
// ─────────────────────────────────────────────────────────────────────────────

enum class PlaybackState { Live, Playing, Paused };

class PlaybackController {
public:
    PlaybackController();

    // ── Transport controls ────────────────────────────────────────────────────
    void play(float speed = 1.0f) noexcept;
    void pause()                  noexcept;
    void stop()                   noexcept;  // returns to live mode
    void set_speed(float s)       noexcept;  // clamped to [-16, 16], skips 0
    void seek(uint64_t ts_ns)     noexcept;

    // Advance timeline by `dt_seconds` of wall time.  Call once per frame.
    void tick(float dt_seconds) noexcept;

    // ── Queries ───────────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t       current_time_ns()  const noexcept { return current_ns_; }
    [[nodiscard]] PlaybackState  state()             const noexcept { return state_; }
    [[nodiscard]] float          speed()             const noexcept { return speed_; }
    [[nodiscard]] bool           is_live()           const noexcept { return state_ == PlaybackState::Live; }

    // Normalized [0, 1] scrubber position within [session_start, session_end].
    [[nodiscard]] float scrub_pos(uint64_t session_start_ns,
                                  uint64_t session_end_ns) const noexcept;

    // Reverse: compute timeline position from scrubber fraction.
    [[nodiscard]] static uint64_t
    scrub_to_ns(float frac,
                uint64_t session_start_ns,
                uint64_t session_end_ns) noexcept;

    // Clamp current_time_ns to [min_ns, max_ns].
    void clamp(uint64_t min_ns, uint64_t max_ns) noexcept;

private:
    PlaybackState state_      = PlaybackState::Live;
    float         speed_      = 1.0f;
    uint64_t      current_ns_ = 0;
};

} // namespace afteraction
